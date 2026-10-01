// Offline verification for GrainLock. Runs the shipped processor with no host and exits
// non-zero if any check fails, so CI fails with it.
//
//   GrainLockTests                 run everything
//   GrainLockTests --only alloc    run just the allocation check (used for the Debug-CRT build)
//   GrainLockTests --snapshot DIR  render the editor to PNGs in DIR instead of testing

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"
#include "Presets.h"
#include "dsp/Lfo.h"
#include "ui/GrainLookAndFeel.h"
#include "ui/MainPanel.h"
#include "FingerprintScript.h"
#include "Fingerprints.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <vector>

//==============================================================================
// Allocation counter. Counting is switched on only for the calling thread, and only
// around processBlock, so it measures exactly what the audio thread would allocate.

namespace alloccount
{
    std::atomic<long long> count { 0 };
    thread_local bool active = false;

    inline void note() noexcept
    {
        if (active)
            count.fetch_add (1, std::memory_order_relaxed);
    }

    struct Scope
    {
        Scope() noexcept  { active = true; }
        ~Scope() noexcept { active = false; }
    };

    const char* coverage = "operator new/new[]";
}

void* operator new (std::size_t size)
{
    alloccount::note();
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    alloccount::note();
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void operator delete (void* p) noexcept                 { std::free (p); }
void operator delete[] (void* p) noexcept               { std::free (p); }
void operator delete (void* p, std::size_t) noexcept    { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept  { std::free (p); }

#if defined (__linux__) && defined (__GLIBC__)
// glibc lets the executable interpose the C allocator, which also catches JUCE's HeapBlock
// (it calls malloc directly, never operator new).
extern "C"
{
    void* __libc_malloc (size_t);
    void* __libc_calloc (size_t, size_t);
    void* __libc_realloc (void*, size_t);
    void* __libc_memalign (size_t, size_t);
    void  __libc_free (void*);

    void* malloc (size_t size) noexcept                 { alloccount::note(); return __libc_malloc (size); }
    void* calloc (size_t n, size_t size) noexcept       { alloccount::note(); return __libc_calloc (n, size); }
    void* realloc (void* p, size_t size) noexcept       { alloccount::note(); return __libc_realloc (p, size); }
    void  free (void* p) noexcept                       { __libc_free (p); }
    void* memalign (size_t align, size_t size) noexcept { alloccount::note(); return __libc_memalign (align, size); }
    void* aligned_alloc (size_t align, size_t size) noexcept { alloccount::note(); return __libc_memalign (align, size); }

    int posix_memalign (void** out, size_t align, size_t size) noexcept
    {
        alloccount::note();
        void* p = __libc_memalign (align, size);
        if (p == nullptr)
            return ENOMEM;
        *out = p;
        return 0;
    }
}
 #define GRAINLOCK_INSTALL_ALLOC_HOOK() alloccount::coverage = "malloc/calloc/realloc/memalign + operator new (glibc interposition)"

#elif defined (_MSC_VER) && defined (_DEBUG)
 #include <crtdbg.h>
// The debug CRT reports every heap allocation, including malloc calls that bypass operator new.
static int crtAllocHook (int allocType, void*, size_t, int, long, const unsigned char*, int)
{
    if (allocType == _HOOK_ALLOC || allocType == _HOOK_REALLOC)
        alloccount::note();
    return 1;
}
 #define GRAINLOCK_INSTALL_ALLOC_HOOK() (_CrtSetAllocHook (crtAllocHook), alloccount::coverage = "every CRT heap allocation (debug CRT hook)")

#else
 #define GRAINLOCK_INSTALL_ALLOC_HOOK() (void) 0
#endif

//==============================================================================
namespace
{
    using namespace grainlock;

    int failures = 0;

    /** printf-style formatting into a juce::String. (juce::String::formatted is wide-char on Windows,
        where %s would read a narrow string as UTF-16 and print garbage.) */
    juce::String fmt (const char* format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start (args, format);
        std::vsnprintf (buffer, sizeof (buffer), format, args);
        va_end (args);
        return juce::String (buffer);
    }

    void check (bool ok, const juce::String& what)
    {
        std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
        std::fflush (stdout);
        if (! ok)
            ++failures;
    }

    void section (const char* name)
    {
        std::printf ("\n== %s ==\n", name);
        std::fflush (stdout);
    }

    double noteHz (int midiNote) { return 440.0 * std::pow (2.0, (midiNote - 69) / 12.0); }

    struct FakePlayHead final : public juce::AudioPlayHead
    {
        double bpm = 128.0;
        double ppq = 0.0;
        bool playing = true;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (bpm);
            info.setPpqPosition (ppq);
            info.setIsPlaying (playing);
            return info;
        }
    };

    struct MidiEvent
    {
        juce::int64 time;
        juce::uint8 bytes[3];
    };

    MidiEvent noteOnAt (juce::int64 t, int note, int velocity) { return { t, { 0x90, (juce::uint8) note, (juce::uint8) velocity } }; }
    MidiEvent noteOffAt (juce::int64 t, int note)              { return { t, { 0x80, (juce::uint8) note, 0 } }; }

    /** A processor with no host around it. */
    struct Harness
    {
        Harness (double sampleRate, int maxBlock, int numInputs = 2) : rate (sampleRate), blockSize (maxBlock)
        {
            proc.setPlayConfigDetails (numInputs, 2, sampleRate, maxBlock);
            proc.prepareToPlay (sampleRate, maxBlock);
        }

        void set (const char* id, float plainValue)
        {
            auto* p = proc.apvts.getParameter (id);
            jassert (p != nullptr);
            p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        }

        /** Feeds uniform white noise (peak `amplitude`) for numSamples, with the given MIDI.
            Output channels are appended to outL/outR when they are non-null. */
        void run (juce::int64 numSamples, const std::vector<MidiEvent>& events, float amplitude,
                  std::vector<float>* outL, std::vector<float>* outR, juce::Random& rng)
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            midi.ensureSize (4096);
            size_t nextEvent = 0;

            for (juce::int64 pos = 0; pos < numSamples; pos += blockSize)
            {
                const int n = (int) std::min<juce::int64> (blockSize, numSamples - pos);
                buffer.setSize (2, n, false, false, true);

                for (int i = 0; i < n; ++i)
                {
                    const float l = (rng.nextFloat() * 2.0f - 1.0f) * amplitude;
                    const float r = (rng.nextFloat() * 2.0f - 1.0f) * amplitude;
                    buffer.setSample (0, i, l);
                    buffer.setSample (1, i, r);
                }

                midi.clear();
                while (nextEvent < events.size() && events[nextEvent].time < pos + n)
                {
                    const auto& e = events[nextEvent++];
                    midi.addEvent (e.bytes, 3, (int) std::max<juce::int64> (0, e.time - pos));
                }

                proc.processBlock (buffer, midi);

                if (outL != nullptr)
                    outL->insert (outL->end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + n);
                if (outR != nullptr)
                    outR->insert (outR->end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + n);
            }
        }

        /** Like run(), but the input is signal (time, channel) instead of noise. */
        void runSignal (juce::int64 numSamples, const std::vector<MidiEvent>& events,
                        const std::function<float (juce::int64, int)>& signal, std::vector<float>* outL)
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            midi.ensureSize (4096);
            size_t nextEvent = 0;

            for (juce::int64 pos = 0; pos < numSamples; pos += blockSize)
            {
                const int n = (int) std::min<juce::int64> (blockSize, numSamples - pos);
                buffer.setSize (2, n, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                        buffer.setSample (ch, i, signal (pos + i, ch));

                midi.clear();
                while (nextEvent < events.size() && events[nextEvent].time < pos + n)
                {
                    const auto& e = events[nextEvent++];
                    midi.addEvent (e.bytes, 3, (int) std::max<juce::int64> (0, e.time - pos));
                }

                proc.processBlock (buffer, midi);

                if (outL != nullptr)
                    outL->insert (outL->end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + n);
            }
        }

        GrainLockProcessor proc;
        double rate;
        int blockSize;
    };

    //==========================================================================
    // Pitch measurement

    /** Fundamental of the last 2^order samples of x: Blackman-Harris window, FFT, then the lowest
        spectral peak within 40 dB of the strongest one, refined by parabolic interpolation. */
    double fftFundamentalHz (const std::vector<float>& x, double sampleRate, int order)
    {
        const int n = 1 << order;
        std::vector<float> buf ((size_t) (2 * n), 0.0f);
        const size_t start = x.size() - (size_t) n;

        for (int i = 0; i < n; ++i)
        {
            const double t = juce::MathConstants<double>::twoPi * i / (n - 1);
            const double w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2 * t) - 0.01168 * std::cos (3 * t);
            buf[(size_t) i] = (float) (x[start + (size_t) i] * w);
        }

        juce::dsp::FFT fft (order);
        fft.performFrequencyOnlyForwardTransform (buf.data());

        const double binHz = sampleRate / n;
        const int lo = (int) std::ceil (20.0 / binHz);
        const int hi = std::min (n / 2 - 2, (int) (20000.0 / binHz));

        float strongest = 0.0f;
        for (int k = lo; k <= hi; ++k)
            strongest = std::max (strongest, buf[(size_t) k]);

        const float threshold = strongest * 0.01f;
        for (int k = lo + 1; k < hi; ++k)
        {
            const float m = buf[(size_t) k];
            if (m >= threshold && m >= buf[(size_t) (k - 1)] && m >= buf[(size_t) (k + 1)])
            {
                const double a = 20.0 * std::log10 (std::max (1e-20f, buf[(size_t) (k - 1)]));
                const double b = 20.0 * std::log10 (std::max (1e-20f, m));
                const double c = 20.0 * std::log10 (std::max (1e-20f, buf[(size_t) (k + 1)]));
                const double denom = a - 2.0 * b + c;
                const double delta = std::abs (denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
                return (k + delta) * binHz;
            }
        }
        return 0.0;
    }

    /** Fundamental by normalised autocorrelation: the shortest lag whose correlation is within
        90% of the best lag, refined by parabolic interpolation. */
    double autocorrFundamentalHz (const std::vector<float>& x, double sampleRate, double fMin, double fMax)
    {
        const int length = 16384;
        const int minLag = (int) std::floor (sampleRate / fMax);
        const int maxLag = (int) std::ceil (sampleRate / fMin);
        const size_t start = x.size() - (size_t) (length + maxLag + 2);

        std::vector<double> r ((size_t) (maxLag + 2), 0.0);
        for (int lag = minLag - 1; lag <= maxLag + 1; ++lag)
        {
            double xy = 0.0, xx = 0.0, yy = 0.0;
            for (int i = 0; i < length; ++i)
            {
                const double a = x[start + (size_t) i];
                const double b = x[start + (size_t) (i + lag)];
                xy += a * b; xx += a * a; yy += b * b;
            }
            r[(size_t) lag] = xy / std::sqrt (std::max (1e-30, xx * yy));
        }

        double best = -1.0;
        for (int lag = minLag; lag <= maxLag; ++lag)
            best = std::max (best, r[(size_t) lag]);

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            const double v = r[(size_t) lag];
            if (v >= 0.9 * best && v >= r[(size_t) (lag - 1)] && v >= r[(size_t) (lag + 1)])
            {
                const double a = r[(size_t) (lag - 1)], c = r[(size_t) (lag + 1)];
                const double denom = a - 2.0 * v + c;
                const double delta = std::abs (denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
                return sampleRate / (lag + delta);
            }
        }
        return 0.0;
    }

    double centsBetween (double measured, double expected)
    {
        return measured > 0.0 ? 1200.0 * std::log2 (measured / expected) : 1.0e9;
    }

    /** Holds one note over white noise and returns the wet output (left channel). */
    std::vector<float> holdNoteOverNoise (Harness& h, int midiNote, juce::int64 lengthAfterNote)
    {
        juce::Random rng (0x5eed + midiNote);
        const juce::int64 preRoll = (juce::int64) (0.5 * h.rate);   // input the note can grab
        std::vector<float> out;
        out.reserve ((size_t) (preRoll + lengthAfterNote));
        h.run (preRoll + lengthAfterNote, { noteOnAt (preRoll, midiNote, 127) }, 0.25f, &out, nullptr, rng);
        out.erase (out.begin(), out.begin() + (std::ptrdiff_t) preRoll);
        return out;
    }

    //==========================================================================
    void testPitchAccuracy()
    {
        section ("1. Pitch: white noise in, held note out (FFT fundamental within 3 cents)");

        // Hold mode freezes one capture, so the output is exactly periodic and the spectrum
        // is a clean line series. Everything else is at its default (Grain 2, Pitch Lock on).
        constexpr int fftOrder = 19;   // 524288 samples, 0.09 Hz bins at 48 kHz
        const struct { int note; const char* name; } notes[] = {
            { 69, "A4 (440 Hz)" }, { 36, "C2" }, { 60, "C4" }, { 84, "C6" }
        };

        for (const auto& n : notes)
        {
            Harness h (48000.0, 512);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            const auto out = holdNoteOverNoise (h, n.note, (juce::int64) h.rate + (1 << fftOrder));
            const double expected = noteHz (n.note);
            const double measured = fftFundamentalHz (out, h.rate, fftOrder);
            const double cents = centsBetween (measured, expected);
            check (std::abs (cents) <= 3.0,
                   fmt ("%s: expected %.3f Hz, measured %.3f Hz (%+.3f cents)",
                                            n.name, expected, measured, cents));
        }

        {
            Harness h (48000.0, 512);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::live);
            const auto out = holdNoteOverNoise (h, 69, (juce::int64) (2.0 * h.rate));
            const double measured = autocorrFundamentalHz (out, h.rate, 50.0, 2000.0);
            const double cents = centsBetween (measured, 440.0);
            check (std::abs (cents) <= 3.0,
                   fmt ("Live mode A4 (autocorrelation): measured %.3f Hz (%+.3f cents)", measured, cents));
        }

        {
            // Pitch Lock off: the loop really is Grain Cycles periods long, so 2 cycles sound an octave down.
            Harness h (48000.0, 512);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::pitchLock, 0.0f);
            const auto out = holdNoteOverNoise (h, 69, (juce::int64) h.rate + (1 << fftOrder));
            const double measured = fftFundamentalHz (out, h.rate, fftOrder);
            const double cents = centsBetween (measured, 220.0);
            check (std::abs (cents) <= 3.0,
                   fmt ("Pitch Lock off, 2 cycles, A4: expected 220 Hz, measured %.3f Hz (%+.3f cents)", measured, cents));
        }
    }

    //==========================================================================
    /** peakBound > 0 also checks the signal arriving at the limiter, which catches gain-staging
        mistakes that the limiter would otherwise hide from the output check. */
    void runStaccato (const char* label, float peakBound, const std::function<void (Harness&)>& configure)
    {
        Harness h (48000.0, 256);
        configure (h);
        h.proc.resetLimiterStats();

        juce::Random rng (777);
        const juce::int64 sixteenth = 6000;           // 1/16 at 120 BPM, 48 kHz
        const juce::int64 total = (juce::int64) (30.0 * h.rate);

        std::vector<MidiEvent> events;
        for (juce::int64 step = 0; step * sixteenth < total - sixteenth; ++step)
        {
            const juce::int64 t = step * sixteenth + 3000;
            const int chordSize = (step % 4 == 3) ? 4 : 1;   // every 4th step a chord, to force voice stealing
            for (int c = 0; c < chordSize; ++c)
            {
                const int note = 36 + rng.nextInt (60);
                events.push_back (noteOnAt (t, note, 20 + rng.nextInt (108)));
                events.push_back (noteOffAt (t + sixteenth / 2, note));
            }
        }
        std::sort (events.begin(), events.end(), [] (const MidiEvent& a, const MidiEvent& b) { return a.time < b.time; });

        std::vector<float> outL, outR;
        outL.reserve ((size_t) total);
        outR.reserve ((size_t) total);
        juce::Random noise (4242);
        h.run (total, events, 0.5f, &outL, &outR, noise);

        bool finite = true;
        float peak = 0.0f;
        for (size_t i = 0; i < outL.size(); ++i)
        {
            finite = finite && std::isfinite (outL[i]) && std::isfinite (outR[i]);
            peak = std::max (peak, std::max (std::abs (outL[i]), std::abs (outR[i])));
        }

        const auto stats = h.proc.getLimiterStats();
        check (finite && stats.nonFiniteInputs == 0,
               fmt ("%s: no NaN or inf (output, and %d non-finite samples reaching the limiter)", label, stats.nonFiniteInputs));
        if (peakBound > 0.0f)
            check (stats.maxInputPeak < peakBound,
                   fmt ("%s: level before the limiter peaks at %.3f (bound %.1f)", label, stats.maxInputPeak, peakBound));
        check (peak <= 1.0f, fmt ("%s: peak %.4f (%.2f dBFS) <= 0 dBFS", label, peak,
                                                      juce::Decibels::gainToDecibels (peak, -200.0f)));
    }

    void testStaccato()
    {
        section ("2. Staccato 1/16 pattern over noise: finite, peak <= 0 dBFS");

        runStaccato ("defaults (Live)", 6.0f, [] (Harness& h)
        {
            h.set (ParamID::attack, 0.0f);
            h.set (ParamID::release, 30.0f);
        });

        runStaccato ("worst case (+24 dB, 16 cycles, lock off, formant +12, all three LFOs at 100%)", 0.0f, [] (Harness& h)
        {
            h.set (ParamID::attack, 0.0f);
            h.set (ParamID::release, 30.0f);
            h.set (ParamID::outGain, 24.0f);
            h.set (ParamID::grainCycles, 16.0f);
            h.set (ParamID::pitchLock, 0.0f);
            h.set (ParamID::formant, 12.0f);
            h.set (ParamID::smooth, 0.0f);
            // All three LFOs at full depth at once.
            const struct { int lfo; LfoShape shape; float rate; } lfos[] = {
                { (int) LfoTarget::pitch, LfoShape::square, 7.0f },
                { (int) LfoTarget::formant, LfoShape::triangle, 3.0f },
                { (int) LfoTarget::grainCycles, LfoShape::sampleHold, 12.0f },
            };
            for (const auto& l : lfos)
            {
                const auto& ids = ParamID::lfo[l.lfo];
                h.set (ids.on, 1.0f);
                h.set (ids.shape, (float) (int) l.shape);
                h.set (ids.rate, l.rate);
                h.set (ids.depth, 100.0f);
            }
        });

        runStaccato ("Hold mode, mono with glide", 6.0f, [] (Harness& h)
        {
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::mono, 1.0f);
            h.set (ParamID::glide, 80.0f);
            h.set (ParamID::offset, 300.0f);
        });
    }

    //==========================================================================
    void testDryWhenIdle()
    {
        section ("3. No MIDI, Dry When Idle on: output equals input within 1e-6");

        for (const auto& [numInputs, amplitude] : { std::pair<int, float> { 2, 0.5f }, { 1, 0.5f }, { 2, 1.0f } })
        {
            Harness h (48000.0, 4096, numInputs);   // defaults: Dry When Idle on, Mix 100%
            juce::Random rng (31337);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> buffer (2, 4096);

            float maxDiff = 0.0f;
            juce::int64 processed = 0;
            while (processed < (juce::int64) (10.0 * h.rate))
            {
                const int n = 1 + rng.nextInt (4096);   // every block size from 1 to 4096 is fair game
                buffer.setSize (2, n, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                        buffer.setSample (ch, i, (rng.nextFloat() * 2.0f - 1.0f) * amplitude);

                const juce::AudioBuffer<float> input (buffer);
                h.proc.processBlock (buffer, midi);

                for (int ch = 0; ch < 2; ++ch)
                {
                    const int sourceChannel = numInputs == 1 ? 0 : ch;
                    for (int i = 0; i < n; ++i)
                        maxDiff = std::max (maxDiff, std::abs (buffer.getSample (ch, i) - input.getSample (sourceChannel, i)));
                }
                processed += n;
            }

            check (maxDiff <= 1.0e-6f, fmt ("%s input, peaks up to %.2f: max |out - in| = %.3g",
                                            numInputs == 2 ? "stereo" : "mono", (double) amplitude, (double) maxDiff));
        }
    }

    //==========================================================================
    void testStateRoundTrip()
    {
        section ("4. State save/restore round-trips every parameter");

        GrainLockProcessor a;
        const auto& aParams = a.getParameters();
        check (aParams.size() == (int) std::size (ParamID::all) + 1,
               fmt ("%d parameters exposed (expected %d plus the bypass switch)", aParams.size(), (int) std::size (ParamID::all)));

        for (const char* id : ParamID::all)
            check (a.apvts.getParameter (id) != nullptr, juce::String ("parameter id present: ") + id);

        // Random but legal values: a host can only put a switch or a choice in one of its states,
        // and JUCE keeps the unsnapped float for bools, so snap through the parameter's own range.
        juce::Random rng (2024);
        for (const char* id : ParamID::all)
        {
            auto* p = a.apvts.getParameter (id);
            p->setValueNotifyingHost (p->convertTo0to1 (p->convertFrom0to1 (rng.nextFloat())));
        }

        juce::MemoryBlock state;
        a.getStateInformation (state);

        GrainLockProcessor b;
        b.setStateInformation (state.getData(), (int) state.getSize());

        int mismatches = 0;
        for (const char* id : ParamID::all)
        {
            const float va = a.apvts.getParameter (id)->getValue();
            const float vb = b.apvts.getParameter (id)->getValue();
            const float ra = a.apvts.getRawParameterValue (id)->load();
            const float rb = b.apvts.getRawParameterValue (id)->load();
            if (std::abs (va - vb) > 1.0e-6f || std::abs (ra - rb) > 1.0e-4f * std::max (1.0f, std::abs (ra)))
            {
                ++mismatches;
                std::printf ("    %s: saved %.6f (%.4f), restored %.6f (%.4f)\n", id, va, ra, vb, rb);
            }
        }
        check (mismatches == 0, fmt ("all %d parameters restored exactly (%d mismatches)",
                                                         (int) std::size (ParamID::all), mismatches));
    }

    //==========================================================================
    void testNoAllocations (double minutes)
    {
        section ("5. Long run at 48 kHz / 64-sample blocks: no allocations on the audio thread");

        const double rate = 48000.0;
        const int block = 64;
        Harness h (rate, block);
        FakePlayHead playHead;
        h.proc.setPlayHead (&playHead);

        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        midi.ensureSize (8192);
        juce::Random rng (99);

        const juce::int64 totalBlocks = (juce::int64) (minutes * 60.0 * rate / block);
        const juce::int64 blocksPerChange = (juce::int64) (2.0 * rate / block);
        std::array<int, 16> held {};
        int numHeld = 0;
        bool finite = true;

        h.proc.resetLimiterStats();
        alloccount::count = 0;
        const auto startMs = juce::Time::getMillisecondCounterHiRes();

        for (juce::int64 b = 0; b < totalBlocks; ++b)
        {
            // Everything a host would do between callbacks happens outside the counted scope.
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                for (int i = 0; i < block; ++i)
                    d[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.4f;
            }

            midi.clear();
            if (rng.nextInt (6) == 0 && numHeld < (int) held.size())
            {
                const int note = 30 + rng.nextInt (70);
                const juce::uint8 on[3] = { 0x90, (juce::uint8) note, (juce::uint8) (1 + rng.nextInt (127)) };
                midi.addEvent (on, 3, rng.nextInt (block));
                held[(size_t) numHeld++] = note;
            }
            if (numHeld > 0 && rng.nextInt (5) == 0)
            {
                const int idx = rng.nextInt (numHeld);
                const juce::uint8 off[3] = { 0x80, (juce::uint8) held[(size_t) idx], 0 };
                midi.addEvent (off, 3, rng.nextInt (block));
                held[(size_t) idx] = held[(size_t) --numHeld];
            }
            if (rng.nextInt (40) == 0)
            {
                const int bend = rng.nextInt (16384);
                const juce::uint8 pb[3] = { 0xe0, (juce::uint8) (bend & 0x7f), (juce::uint8) (bend >> 7) };
                midi.addEvent (pb, 3, 0);
            }
            if (rng.nextInt (20000) == 0)
            {
                const juce::uint8 allOff[3] = { 0xb0, 123, 0 };
                midi.addEvent (allOff, 3, 0);
                numHeld = 0;
            }

            // Controllers, pressure (a 2-byte message, placed last), sound-off and reset: whatever the
            // engine does with each, it must not allocate.
            if (rng.nextInt (60) == 0)
            {
                static const int controllers[] = { 1, 11, 64, 120, 121 };
                const juce::uint8 cc[3] = { 0xb0, (juce::uint8) controllers[rng.nextInt (5)], (juce::uint8) rng.nextInt (128) };
                midi.addEvent (cc, 3, rng.nextInt (block));
            }
            if (rng.nextInt (90) == 0)
            {
                const juce::uint8 polyPressure[3] = { 0xa0, (juce::uint8) (30 + rng.nextInt (70)), (juce::uint8) rng.nextInt (128) };
                midi.addEvent (polyPressure, 3, rng.nextInt (block));
            }
            if (rng.nextInt (90) == 0)
            {
                const juce::uint8 pressure[2] = { 0xd0, (juce::uint8) rng.nextInt (128) };
                midi.addEvent (pressure, 2, block - 1);
            }

            if (b % blocksPerChange == 0)
            {
                // Every parameter to a random legal value, so no feature can go untested by being left
                // at its default. (A list written by hand had already missed the 0.3 additions.)
                for (const char* id : ParamID::all)
                {
                    auto* p = h.proc.apvts.getParameter (id);
                    p->setValueNotifyingHost (p->convertTo0to1 (p->convertFrom0to1 (rng.nextFloat())));
                }
                playHead.playing = rng.nextInt (5) != 0;
                if (rng.nextInt (4) == 0)
                    playHead.bpm = 60.0 + rng.nextFloat() * 140.0;      // a tempo change
                if (rng.nextInt (6) == 0)
                    playHead.ppq = rng.nextFloat() * 64.0;              // a locate
            }

            {
                const alloccount::Scope counting;
                h.proc.processBlock (buffer, midi);
            }

            playHead.ppq += block / rate * playHead.bpm / 60.0;

            const float* l = buffer.getReadPointer (0);
            const float* r = buffer.getReadPointer (1);
            for (int i = 0; i < block; ++i)
                finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
        }

        const double seconds = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
        std::printf ("    %.1f minutes of audio in %.1f s; counting %s\n", minutes, seconds, alloccount::coverage);

        check (finite && h.proc.getLimiterStats().nonFiniteInputs == 0,
               fmt ("long run: no NaN or inf (%d non-finite samples reaching the limiter)", h.proc.getLimiterStats().nonFiniteInputs));
        check (alloccount::count.load() == 0,
               fmt ("allocations inside processBlock: %lld", alloccount::count.load()));
    }

    //==========================================================================
    void testRatesBlocksAndLayouts()
    {
        section ("Extra: sample-rate changes, 1..4096-sample blocks, bus layouts, latency");

        GrainLockProcessor proc;
        juce::Random rng (5);
        bool finite = true;
        proc.resetLimiterStats();

        for (const auto& [rate, block] : { std::pair<double, int> { 44100.0, 512 }, { 96000.0, 1 },
                                          { 192000.0, 4096 }, { 22050.0, 33 }, { 48000.0, 4096 } })
        {
            proc.setPlayConfigDetails (2, 2, rate, block);
            proc.prepareToPlay (rate, block);

            juce::AudioBuffer<float> buffer (2, block);
            juce::MidiBuffer midi;
            const juce::int64 total = (juce::int64) (0.6 * rate);
            for (juce::int64 pos = 0; pos < total; pos += block)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block; ++i)
                        buffer.setSample (ch, i, rng.nextFloat() - 0.5f);

                midi.clear();
                if (pos == 0 || rng.nextInt (std::max (1, (int) (0.05 * rate) / block)) == 0)
                {
                    const juce::uint8 on[3] = { 0x90, (juce::uint8) (24 + rng.nextInt (100)), 100 };
                    midi.addEvent (on, 3, 0);
                }
                proc.processBlock (buffer, midi);

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block; ++i)
                        finite = finite && std::isfinite (buffer.getSample (ch, i));
            }
        }
        check (finite && proc.getLimiterStats().nonFiniteInputs == 0,
               "44.1/96/192/22.05/48 kHz with 512/1/4096/33/4096-sample blocks: all output finite");
        check (proc.getLatencySamples() == 0, "reports zero latency");

        using Set = juce::AudioChannelSet;
        auto layout = [] (Set in, Set out)
        {
            juce::AudioProcessor::BusesLayout l;
            l.inputBuses.add (in);
            l.outputBuses.add (out);
            return l;
        };
        check (proc.checkBusesLayoutSupported (layout (Set::stereo(), Set::stereo())), "stereo in / stereo out supported");
        check (proc.checkBusesLayoutSupported (layout (Set::mono(), Set::stereo())), "mono in / stereo out supported");
        check (! proc.checkBusesLayoutSupported (layout (Set::stereo(), Set::mono())), "stereo in / mono out rejected");
    }
}

namespace
{
    using namespace grainlock;

    void testEnvelopeRelease()
    {
        section ("Extra: every release keeps its own length (no cut-off at a block boundary)");

        // A constant input makes the wet output exactly 0.25 x envelope, so the envelope can be read
        // straight off the output. Dry is muted (Dry When Idle off, Mix 100%) and Smooth 0 removes the seam.
        auto runCase = [] (const char* label, float attackMs, float decayMs, float sustainPercent, float releaseMs,
                           double noteOffAfterMs, double halfAtMs, double silentAtMs)
        {
            Harness h (48000.0, 512);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::grainCycles, 1.0f);
            h.set (ParamID::smooth, 0.0f);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::attack, attackMs);
            h.set (ParamID::decay, decayMs);
            h.set (ParamID::sustain, sustainPercent);
            h.set (ParamID::release, releaseMs);

            const juce::int64 on = 24000;
            const juce::int64 off = on + (juce::int64) (noteOffAfterMs * 48.0);
            std::vector<float> out;
            h.runSignal (off + 48000, { noteOnAt (on, 60, 127), noteOffAt (off, 60) },
                         [] (juce::int64, int) { return 0.25f; }, &out);

            const float atOff = out[(size_t) (off - 1)];
            const juce::int64 silentAt = off + (juce::int64) (silentAtMs * 48.0);
            float biggestStep = 0.0f;
            for (juce::int64 i = off; i < silentAt; ++i)
                biggestStep = std::max (biggestStep, std::abs (out[(size_t) i] - out[(size_t) (i - 1)]));

            const float half = out[(size_t) (off + (juce::int64) (halfAtMs * 48.0))];
            const float after = std::abs (out[(size_t) silentAt]);

            check (atOff > 0.05f && biggestStep < 0.002f,
                   fmt ("%s: level %.4f at note-off, largest one-sample step while releasing %.5f", label, atOff, biggestStep));
            check (std::abs (half - 0.5f * atOff) < 0.1f * atOff,
                   fmt ("%s: halfway through the release %.4f (expected %.4f)", label, half, 0.5f * atOff));
            check (after < 1.0e-6f, fmt ("%s: silent once the release time has passed (%.6f)", label, after));
        };

        runCase ("sustain 0%, released mid-decay", 0.0f, 500.0f, 0.0f, 200.0f, 100.0, 100.0, 210.0);
        runCase ("sustain 50%, released at the attack peak", 50.0f, 250.0f, 50.0f, 100.0f, 50.0, 50.0, 110.0);
    }

    void testBypass()
    {
        section ("Extra: host bypass passes audio and leaves no note stuck");

        Harness h (48000.0, 512);   // defaults: Live, Dry When Idle on, Mix 100%
        juce::Random rng (8);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        midi.ensureSize (256);
        float maxDiff = 0.0f;

        for (int b = 0; b < 300; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buffer.setSample (ch, i, (rng.nextFloat() * 2.0f - 1.0f) * 0.4f);
            const juce::AudioBuffer<float> input (buffer);

            midi.clear();
            if (b == 10)  { const juce::uint8 on[3] = { 0x90, 60, 100 }; midi.addEvent (on, 3, 0); }
            if (b == 120) { const juce::uint8 off[3] = { 0x80, 60, 0 };  midi.addEvent (off, 3, 0); }   // lands while bypassed

            const bool bypassed = b >= 100 && b < 150;
            if (bypassed)
                h.proc.processBlockBypassed (buffer, midi);
            else
                h.proc.processBlock (buffer, midi);

            if (bypassed || b >= 150)
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                        maxDiff = std::max (maxDiff, std::abs (buffer.getSample (ch, i) - input.getSample (ch, i)));
        }

        check (maxDiff <= 1.0e-6f, fmt ("while bypassed and after (note-off sent during bypass): max |out - in| = %.3g", (double) maxDiff));
    }

    void testBypassSwitch()
    {
        section ("Extra: Cubase's bypass switch fades instead of clicking, and a held note is still there afterwards");

        Harness h (48000.0, 512);
        h.set (ParamID::captureMode, 0.0f);   // Hold
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        midi.ensureSize (256);

        std::vector<float> in, out;
        double phase = 0.0;
        for (int b = 0; b < 200; ++b)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.4f * (float) std::sin (phase);
                phase += juce::MathConstants<double>::twoPi * 220.0 / 48000.0;
                buffer.setSample (0, i, v);
                buffer.setSample (1, i, v);
                in.push_back (v);
            }
            midi.clear();
            if (b == 20) { const juce::uint8 on[3] = { 0x90, 67, 100 }; midi.addEvent (on, 3, 0); }
            if (b == 100) h.set (ParamID::bypass, 1.0f);
            if (b == 150) h.set (ParamID::bypass, 0.0f);
            h.proc.processBlock (buffer, midi);
            for (int i = 0; i < 512; ++i)
                out.push_back (buffer.getSample (0, i));
        }

        auto maxStep = [] (const std::vector<float>& x, size_t a, size_t b)
        {
            float m = 0.0f;
            for (size_t i = a + 1; i < b; ++i) m = std::max (m, std::abs (x[i] - x[i - 1]));
            return m;
        };
        auto maxDiff = [&] (size_t a, size_t b)
        {
            float m = 0.0f;
            for (size_t i = a; i < b; ++i) m = std::max (m, std::abs (out[i] - in[i]));
            return m;
        };

        const size_t on = 100 * 512, off = 150 * 512;
        const float normal = std::max (maxStep (out, 60 * 512, on), maxStep (in, 0, in.size()));
        const float goingIn = maxStep (out, on - 1, on + 2400), comingOut = maxStep (out, off - 1, off + 2400);
        check (goingIn <= 1.25f * normal + 0.005f && comingOut <= 1.25f * normal + 0.005f,
               fmt ("largest step going into bypass %.4f and coming out %.4f, against %.4f in the steady sound (no click)",
                    (double) goingIn, (double) comingOut, (double) normal));
        check (juce::exactlyEqual (maxDiff (on + 2400, off), 0.0f),
               fmt ("from 50 ms into the bypass the output is the input exactly (max diff %g)", (double) maxDiff (on + 2400, off)));
        check (maxDiff (off + 4800, out.size()) > 0.05f, "bypass lifted with the key still held: the frozen note is still sounding");
    }

    //==========================================================================
    // 0.3

    /** Level (dB) of the spectral line nearest hz in the last 2^order samples of x (Hann window). */
    double lineLevelDb (const std::vector<float>& x, double sampleRate, double hz, int order = 15)
    {
        const int n = 1 << order;
        std::vector<float> buf ((size_t) (2 * n), 0.0f);
        const size_t start = x.size() - (size_t) n;
        for (int i = 0; i < n; ++i)
            buf[(size_t) i] = x[start + (size_t) i] * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (n - 1)));
        juce::dsp::FFT fft (order);
        fft.performFrequencyOnlyForwardTransform (buf.data());
        const int k = (int) std::lround (hz * n / sampleRate);
        float peak = 0.0f;
        for (int j = std::max (1, k - 2); j <= std::min (n / 2 - 1, k + 2); ++j)
            peak = std::max (peak, buf[(size_t) j]);
        return 20.0 * std::log10 (std::max (1.0e-12f, peak));
    }

    double rmsDb (const std::vector<float>& x, size_t from, size_t to)
    {
        double s = 0.0;
        for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
        return 10.0 * std::log10 (std::max (1.0e-20, s / (double) std::max<size_t> (1, to - from)));
    }

    float largestStep (const std::vector<float>& x, size_t from, size_t to)
    {
        float m = 0.0f;
        for (size_t i = std::max<size_t> (from, 1); i < to && i < x.size(); ++i)
            m = std::max (m, std::abs (x[i] - x[i - 1]));
        return m;
    }

    void testSeamFlatPower()
    {
        section ("0.3 G21: a source at the note's own pitch loops without a bump at the seam");

        // A pure 220 Hz sine frozen on A2 (220 Hz) with one cycle and a 25% seam. The two sides of the
        // seam are the same audio, so a plain equal-power fade swells by 3 dB there once per cycle,
        // which adds harmonics to a sine. A flat-power seam leaves a sine a sine. The same build with
        // 0.2's seam switched back in must show the harmonics, or this test proves nothing.
        auto addedHarmonics = [] (bool legacySeam)
        {
            Harness h (48000.0, 512);
            h.proc.setLegacySeamForTests (legacySeam);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::grainCycles, 1.0f);
            h.set (ParamID::smooth, 25.0f);
            std::vector<float> out;
            h.runSignal (96000, { noteOnAt (24000, 57, 127) },
                         [] (juce::int64 t, int) { return 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * (double) t / 48000.0); }, &out);

            const double fundamental = lineLevelDb (out, 48000.0, 220.0);
            double worst = -200.0;
            for (int harmonic = 2; harmonic <= 6; ++harmonic)
                worst = std::max (worst, lineLevelDb (out, 48000.0, 220.0 * harmonic) - fundamental);
            return worst;
        };

        const double before = addedHarmonics (true), after = addedHarmonics (false);
        check (after <= -50.0 && before >= -40.0,
               fmt ("sine in, sine out: strongest added harmonic %.1f dB under the fundamental (limit -50); with 0.2's seam it is %.1f dB", after, before));
    }

    void testLoopPointNudge()
    {
        section ("0.3 G21: with Smooth at 0 the grab ends where the loop joins best; with a seam it stays where the key put it");

        // A 300 Hz sine frozen on A2 with no seam: the loop jumps between two places 218 samples apart
        // in a 160-sample wave. Twice per wave those two places hold the same value.
        auto wrapStep = [] (bool legacySeam)
        {
            Harness h (48000.0, 512);
            h.proc.setLegacySeamForTests (legacySeam);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::grainCycles, 1.0f);
            h.set (ParamID::smooth, 0.0f);
            std::vector<float> out;
            h.runSignal (96000, { noteOnAt (24100, 57, 127) },
                         [] (juce::int64 t, int) { return 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 300.0 * (double) t / 48000.0); }, &out);
            return largestStep (out, 72000, 96000);
        };

        const float nudged = wrapStep (false), plain = wrapStep (true);
        check (nudged <= 0.5f * plain && plain > 0.05f,
               fmt ("Smooth 0: the jump at the loop point is %.4f, against %.4f without the nudge", (double) nudged, (double) plain));

        // Default Smooth (10%): a burst 8 ms before a low key after silence must still be caught,
        // exactly as 0.2 caught it (the nudge does not run when there is a seam).
        auto burstLevel = [] (bool legacySeam)
        {
            Harness h (48000.0, 512);
            h.proc.setLegacySeamForTests (legacySeam);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            std::vector<float> out;
            juce::Random rng (5);
            std::vector<float> source ((size_t) 96000, 0.0f);
            for (size_t i = 47616; i < 48000; ++i)   // 8 ms of noise ending at the key
                source[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.4f;
            h.runSignal (96000, { noteOnAt (48000, 36, 127) },
                         [&source] (juce::int64 t, int) { return source[(size_t) t]; }, &out);
            return rmsDb (out, 60000, 96000);
        };
        const double caught = burstLevel (false), reference = burstLevel (true);
        check (reference > -60.0 && std::abs (caught - reference) <= 1.0,
               fmt ("a hit 8 ms before a C1 key: frozen at %.1f dB, 0.2's seam gives %.1f dB (within 1 dB)", caught, reference));
    }

    void testAutoGain()
    {
        section ("0.3 G33: Auto Gain keeps the level when Grain changes");

        // Eight harmonics of 220 Hz, frozen on A2: every cycle is alike, the worst case for level.
        auto source = [] (juce::int64 t, int)
        {
            double s = 0.0;
            for (int k = 1; k <= 8; ++k)
                s += std::sin (juce::MathConstants<double>::twoPi * 220.0 * k * (double) t / 48000.0 + 0.7 * k) / k;
            return (float) (0.03 * s);
        };

        auto level = [&source] (float cycles, bool autoGain)
        {
            Harness h (48000.0, 512);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::grainCycles, cycles);
            h.set (ParamID::autoGain, autoGain ? 1.0f : 0.0f);
            std::vector<float> out;
            h.runSignal (96000, { noteOnAt (48000, 57, 127) }, source, &out);
            return rmsDb (out, 72000, 96000);
        };

        const double rise = level (16.0f, false) - level (1.0f, false);
        const double held = level (16.0f, true) - level (1.0f, true);
        check (rise >= 9.0, fmt ("Auto Gain off: Grain 16 is %.1f dB louder than Grain 1 on a source at the note's pitch (the fault: about 12)", rise));
        check (std::abs (held) <= 1.5, fmt ("Auto Gain on: Grain 16 is within %.2f dB of Grain 1 (limit 1.5)", held));

        // Noise has unrelated cycles, so Auto Gain must leave it (almost) alone.
        auto noiseLevel = [] (bool autoGain)
        {
            Harness h (48000.0, 512);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::grainCycles, 8.0f);
            h.set (ParamID::autoGain, autoGain ? 1.0f : 0.0f);
            juce::Random rng (77);
            std::vector<float> out;
            h.run (96000, { noteOnAt (48000, 57, 127) }, 0.2f, &out, nullptr, rng);
            return rmsDb (out, 72000, 96000);
        };
        const double noiseChange = noiseLevel (true) - noiseLevel (false);
        check (std::abs (noiseChange) <= 1.0, fmt ("noise at Grain 8: Auto Gain changes the level by %.2f dB (limit 1)", noiseChange));

        // Formant moved under a held note: the gain has to follow all the way, not stop half-way.
        // (A second of this source is 220 whole cycles, so the two halves join seamlessly.)
        auto settled = [&source] (float formantAtKey, float formantAfter)
        {
            Harness h (48000.0, 512);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::grainCycles, 16.0f);
            h.set (ParamID::formant, formantAtKey);
            std::vector<float> out;
            h.runSignal (48000, { noteOnAt (24000, 57, 127) }, source, &out);
            h.set (ParamID::formant, formantAfter);
            h.runSignal (96000, {}, source, &out);
            return rmsDb (out, 120000, 144000);
        };
        const double down = settled (1.0f, 0.0f) - settled (0.0f, 0.0f);
        const double up = settled (0.0f, 1.0f) - settled (1.0f, 1.0f);
        check (std::abs (down) <= 1.5 && std::abs (up) <= 1.5,
               fmt ("Formant moved under a held Grain 16 note: the level ends within %+.2f dB (+1 to 0) and %+.2f dB (0 to +1) of a note started there (limit 1.5)", down, up));

        // A key over digital silence must stay silent and finite.
        {
            Harness h (48000.0, 512);
            h.proc.resetLimiterStats();
            h.set (ParamID::grainCycles, 8.0f);
            std::vector<float> out;
            h.runSignal (48000, { noteOnAt (24000, 60, 100) }, [] (juce::int64, int) { return 0.0f; }, &out);
            float peak = 0.0f;
            for (const float v : out) peak = std::max (peak, std::abs (v));
            check (juce::exactlyEqual (peak, 0.0f) && h.proc.getLimiterStats().nonFiniteInputs == 0,
                   fmt ("a key over silence: output peak %g, %d non-finite samples", (double) peak, h.proc.getLimiterStats().nonFiniteInputs));
        }
    }

    void testFormantTrack()
    {
        section ("0.3 G09: the Formant Track switch makes the tone follow the key");

        // Noise with one resonance at 1 kHz. Frozen an octave above the root (C4), tracking reads the
        // source twice as fast, so the resonance moves to 2 kHz; with the switch off it stays put.
        std::vector<float> source ((size_t) 96000);
        {
            juce::Random rng (31);
            const double w = juce::MathConstants<double>::twoPi * 1000.0 / 48000.0, r = 0.985;
            double y1 = 0.0, y2 = 0.0;
            for (auto& v : source)
            {
                const double x = rng.nextDouble() * 2.0 - 1.0;
                const double y = x + 2.0 * r * std::cos (w) * y1 - r * r * y2;
                y2 = y1;
                y1 = y;
                v = (float) (0.004 * y);
            }
        }

        auto bandBalance = [&source] (bool track)
        {
            Harness h (48000.0, 512);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::grainCycles, 8.0f);
            h.set (ParamID::formantTrack, track ? 1.0f : 0.0f);
            std::vector<float> out;
            h.runSignal (96000, { noteOnAt (48000, 72, 127) },
                         [&source] (juce::int64 t, int) { return source[(size_t) t]; }, &out);

            // Energy near 2 kHz against energy near 1 kHz, from the note's harmonics (C4 = 523.25 Hz).
            const double f0 = 440.0 * std::pow (2.0, (72 - 69) / 12.0);
            auto power = [&] (int harmonic) { return std::pow (10.0, lineLevelDb (out, 48000.0, f0 * harmonic) / 10.0); };
            return 10.0 * std::log10 ((power (4) + power (3)) / (power (2) + power (1) * 0.5));
        };

        const double off = bandBalance (false), on = bandBalance (true);
        check (off < -6.0 && on > 6.0,
               fmt ("C4 over a 1 kHz resonance: energy at 2 kHz against 1 kHz is %+.1f dB with the switch off and %+.1f dB with it on", off, on));

        // At the root (C3) the switch must change nothing, also under a pitch bend that arrives after
        // the key: the engine already raises the formant with the bend, and tracking must not add
        // the same rise a second time.
        auto bentRoot = [&source] (bool track)
        {
            Harness h (48000.0, 512);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::grainCycles, 8.0f);
            h.set (ParamID::formantTrack, track ? 1.0f : 0.0f);
            std::vector<float> out;
            const MidiEvent bendUp { 60000, { 0xe0, 0x7f, 0x7f } };
            h.runSignal (96000, { noteOnAt (48000, 60, 127), bendUp },
                         [&source] (juce::int64 t, int) { return source[(size_t) t]; }, &out);
            return out;
        };
        const auto tracked = bentRoot (true), plain = bentRoot (false);
        float difference = 0.0f, peak = 0.0f;
        for (size_t i = 0; i < plain.size(); ++i)
        {
            difference = std::max (difference, std::abs (tracked[i] - plain[i]));
            peak = std::max (peak, std::abs (plain[i]));
        }
        check (difference <= 1.0e-4f && peak > 0.001f,
               fmt ("C3 with a bend after the key: switch on and off differ by %g at most (peak %.3f)", (double) difference, (double) peak));
    }

    void testOlderStates()
    {
        section ("0.3: an older project or preset loads the same onto any instance (Auto Gain off; nothing left over)");

        // Which IDs 0.3 added: everything after Output Gain in the list. Later stages join by themselves.
        std::vector<const char*> added;
        {
            bool past = false;
            for (const char* id : ParamID::all)
            {
                if (past) added.push_back (id);
                if (juce::String (id) == ParamID::outGain) past = true;
            }
        }
        auto isAdded = [&added] (const juce::String& id)
        {
            for (const char* a : added) if (id == a) return true;
            return false;
        };

        // A state as an older version saved it: the defaults, without the IDs listed in keep == false.
        auto olderState = [&] (const std::function<bool (const juce::String&)>& keep)
        {
            GrainLockProcessor source;
            auto xml = source.apvts.copyState().createXml();
            juce::Array<juce::XmlElement*> drop;
            for (auto* node : xml->getChildWithTagNameIterator ("PARAM"))
                if (! keep (node->getStringAttribute ("id")))
                    drop.add (node);
            for (auto* node : drop)
                xml->removeChildElement (node, true);
            juce::MemoryBlock block;
            juce::AudioProcessor::copyXmlToBinary (*xml, block);
            return block;
        };

        // The instance it lands on has been used: every parameter somewhere else.
        auto scrambled = [] (GrainLockProcessor& p)
        {
            juce::Random rng (606);
            for (const char* id : ParamID::all)
            {
                auto* param = p.apvts.getParameter (id);
                param->setValueNotifyingHost (param->convertTo0to1 (param->convertFrom0to1 (rng.nextFloat())));
            }
        };
        auto plain = [] (GrainLockProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        auto atDefault = [] (GrainLockProcessor& p, const char* id)
        {
            auto* param = p.apvts.getParameter (id);
            return std::abs (param->getValue() - param->getDefaultValue()) < 1.0e-6f;
        };

        {
            // Saved by 0.2: none of the 0.3 IDs.
            const auto block = olderState ([&] (const juce::String& id) { return ! isAdded (id); });
            GrainLockProcessor used;
            scrambled (used);
            used.setStateInformation (block.getData(), (int) block.getSize());
            int wrong = 0;
            for (const char* id : ParamID::all)
            {
                const bool legacyOff = juce::String (id) == ParamID::autoGain || juce::String (id) == ParamID::wheelDest;
                const bool ok = legacyOff ? plain (used, id) < 0.5f : atDefault (used, id);
                wrong += ok ? 0 : 1;
            }
            check (wrong == 0, fmt ("0.2 state onto a used instance: Auto Gain off, mod wheel off, all %d others at their defaults (%d wrong)",
                                    (int) std::size (ParamID::all) - 2, wrong));
        }
        {
            // Saved by the first 0.3 build: has Auto Gain (on) and Formant Track, nothing later.
            const auto block = olderState ([&] (const juce::String& id)
                                           { return ! isAdded (id) || id == ParamID::autoGain || id == ParamID::formantTrack; });
            GrainLockProcessor used;
            scrambled (used);
            used.setStateInformation (block.getData(), (int) block.getSize());
            int wrong = 0;
            for (const char* id : ParamID::all)
                wrong += (juce::String (id) == ParamID::wheelDest ? plain (used, id) < 0.5f : atDefault (used, id)) ? 0 : 1;
            check (wrong == 0 && plain (used, ParamID::autoGain) >= 0.5f,
                   fmt ("a state that saved Auto Gain on keeps it on; everything it did not save gets its older value (%d wrong)", wrong));
        }
        {
            GrainLockProcessor fresh;
            check (plain (fresh, ParamID::autoGain) >= 0.5f, "a new instance starts with Auto Gain on");
        }
    }

    //==========================================================================
    // 0.3 stage 2: LFO trigger modes, note envelope, tape stop, keyboard sources

    struct ScriptEvent
    {
        juce::int64 time;
        juce::uint8 bytes[3];
        int size;
    };

    ScriptEvent keyDown (juce::int64 t, int note, int velocity = 127) { return { t, { 0x90, (juce::uint8) note, (juce::uint8) velocity }, 3 }; }
    ScriptEvent keyUp (juce::int64 t, int note)                       { return { t, { 0x80, (juce::uint8) note, 0 }, 3 }; }
    ScriptEvent controller (juce::int64 t, int number, int value)     { return { t, { 0xb0, (juce::uint8) number, (juce::uint8) value }, 3 }; }
    ScriptEvent bendTo (juce::int64 t, int value14)                   { return { t, { 0xe0, (juce::uint8) (value14 & 0x7f), (juce::uint8) ((value14 >> 7) & 0x7f) }, 3 }; }
    ScriptEvent channelPressure (juce::int64 t, int value)            { return { t, { 0xd0, (juce::uint8) value, 0 }, 2 }; }
    ScriptEvent polyPressure (juce::int64 t, int note, int value)     { return { t, { 0xa0, (juce::uint8) note, (juce::uint8) value }, 3 }; }

    /** Plays input (both channels) through h with the events. startPpq >= 0 adds a running transport
        at bpm whose position is worked out from the sample count. Returns the left output. */
    std::vector<float> play (Harness& h, const std::vector<float>& input, const std::vector<ScriptEvent>& events,
                             double startPpq = -1.0, double bpm = 120.0)
    {
        FakePlayHead playHead;
        playHead.bpm = bpm;
        if (startPpq >= 0.0)
            h.proc.setPlayHead (&playHead);

        juce::AudioBuffer<float> buffer (2, h.blockSize);
        juce::MidiBuffer midi;
        midi.ensureSize (4096);
        std::vector<float> out (input.size());
        size_t next = 0;

        for (size_t pos = 0; pos < input.size(); pos += (size_t) h.blockSize)
        {
            const int n = (int) std::min<size_t> ((size_t) h.blockSize, input.size() - pos);
            buffer.setSize (2, n, false, false, true);
            for (int i = 0; i < n; ++i)
            {
                buffer.setSample (0, i, input[pos + (size_t) i]);
                buffer.setSample (1, i, input[pos + (size_t) i]);
            }

            midi.clear();
            while (next < events.size() && events[next].time < (juce::int64) (pos + (size_t) n))
            {
                midi.addEvent (events[next].bytes, events[next].size, (int) std::max<juce::int64> (0, events[next].time - (juce::int64) pos));
                ++next;
            }

            playHead.ppq = std::max (0.0, startPpq) + (double) pos * bpm / 60.0 / h.rate;
            h.proc.processBlock (buffer, midi);
            for (int i = 0; i < n; ++i)
                out[pos + (size_t) i] = buffer.getSample (0, i);
        }

        h.proc.setPlayHead (nullptr);
        return out;
    }

    std::vector<float> noiseInput (size_t length, int seed, float amplitude)
    {
        juce::Random rng (seed);
        std::vector<float> x (length);
        for (auto& v : x)
            v = (rng.nextFloat() * 2.0f - 1.0f) * amplitude;
        return x;
    }

    /** Noise with one resonance at 1 kHz: a tone colour whose position can be measured. */
    std::vector<float> resonantNoise (size_t length)
    {
        std::vector<float> x (length);
        juce::Random rng (31);
        const double w = juce::MathConstants<double>::twoPi * 1000.0 / 48000.0, r = 0.985;
        double y1 = 0.0, y2 = 0.0;
        for (auto& v : x)
        {
            const double in = rng.nextDouble() * 2.0 - 1.0;
            const double y = in + 2.0 * r * std::cos (w) * y1 - r * r * y2;
            y2 = y1;
            y1 = y;
            v = (float) (0.004 * y);
        }
        return x;
    }

    std::vector<float> harmonicInput (size_t length, double hz, int harmonics, double amplitude)
    {
        std::vector<float> x (length);
        for (size_t i = 0; i < length; ++i)
        {
            double s = 0.0;
            for (int k = 1; k <= harmonics; ++k)
                s += std::sin (juce::MathConstants<double>::twoPi * hz * k * (double) i / 48000.0 + 0.7 * k) / k;
            x[i] = (float) (amplitude * s);
        }
        return x;
    }

    std::vector<float> slice (const std::vector<float>& x, size_t from, size_t length)
    {
        return std::vector<float> (x.begin() + (std::ptrdiff_t) from, x.begin() + (std::ptrdiff_t) (from + length));
    }

    /** Pitch of 2^order samples starting at from. */
    double pitchAt (const std::vector<float>& x, size_t from, int order = 13)
    {
        return fftFundamentalHz (slice (x, from, (size_t) 1 << order), 48000.0, order);
    }

    /** Pitch of a short stretch by autocorrelation: for sounds whose pitch is moving. */
    double shortPitchHz (const std::vector<float>& x, size_t from, int length, double minHz, double maxHz)
    {
        const int minLag = (int) (48000.0 / maxHz), maxLag = (int) (48000.0 / minHz);
        double best = -2.0;
        int bestLag = 0;
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            double xy = 0.0, xx = 0.0, yy = 0.0;
            for (int i = 0; i < length; ++i)
            {
                const double a = x[from + (size_t) i], b = x[from + (size_t) (i + lag)];
                xy += a * b; xx += a * a; yy += b * b;
            }
            const double r = xy / std::sqrt (std::max (1.0e-30, xx * yy));
            if (r > best + 0.02) { best = r; bestLag = lag; }   // the shortest of near-equal peaks
        }
        return bestLag > 0 ? 48000.0 / bestLag : 0.0;
    }

    /** Energy of the note's harmonics near 2 kHz against those near 1 kHz, in dB, over the last 2^order samples. */
    double toneBalanceDb (const std::vector<float>& x, double f0, int order = 15)
    {
        double low = 0.0, high = 0.0;
        for (int k = 1; k * f0 < 3000.0; ++k)
        {
            const double f = k * f0, p = std::pow (10.0, lineLevelDb (x, 48000.0, f, order) / 10.0);
            if (f > 750.0 && f < 1300.0)  low += p;
            if (f > 1500.0 && f < 2600.0) high += p;
        }
        return 10.0 * std::log10 (std::max (1.0e-20, high) / std::max (1.0e-20, low));
    }

    void testParameterGuards()
    {
        section ("0.3 guard rails: lists, names and typed values");

        check (lfoShapeChoices().size() == numLfoShapes, fmt ("%d LFO shapes in the one shared list (expected %d)", lfoShapeChoices().size(), numLfoShapes));

        // Every shape draws something different (a shape missing from the oscillator would repeat another).
        {
            std::array<std::array<float, 64>, (size_t) numLfoShapes> curves {};
            for (int s = 0; s < numLfoShapes; ++s)
            {
                Lfo lfo;
                lfo.reset (7);
                for (auto& v : curves[(size_t) s])
                    v = lfo.next (1.0 / 64.0, (LfoShape) s);
            }
            int alike = 0;
            for (int a = 0; a < numLfoShapes; ++a)
                for (int b = a + 1; b < numLfoShapes; ++b)
                {
                    float difference = 0.0f;
                    for (size_t i = 0; i < 64; ++i)
                        difference += std::abs (curves[(size_t) a][i] - curves[(size_t) b][i]);
                    alike += difference < 1.0f ? 1 : 0;
                }
            check (alike == 0, fmt ("all %d shapes differ from each other (%d pairs alike)", numLfoShapes, alike));
        }

        GrainLockProcessor p;

        // The length of a choice list is fixed once released (automation stores index / (count - 1)).
        {
            const std::pair<const char*, int> lists[] = {
                { ParamID::captureMode, 2 },
                { ParamID::pitchLfoSync, 13 }, { ParamID::formantLfoSync, 13 }, { ParamID::grainLfoSync, 13 },
                { ParamID::pitchLfoShape, 6 }, { ParamID::formantLfoShape, 6 }, { ParamID::grainLfoShape, 6 },
                { ParamID::pitchLfoTrig, 4 }, { ParamID::formantLfoTrig, 4 }, { ParamID::grainLfoTrig, 4 },
                { ParamID::wheelDest, 5 }, { ParamID::touchDest, 5 }, { ParamID::exprDest, 5 },
            };
            int wrong = 0, choices = 0;
            for (const auto& [id, count] : lists)
            {
                auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter (id));
                wrong += (choice != nullptr && choice->choices.size() == count) ? 0 : 1;
            }
            for (const char* id : ParamID::all)
                choices += dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter (id)) != nullptr ? 1 : 0;
            check (wrong == 0 && choices == (int) std::size (lists),
                   fmt ("%d choice lists, each with its recorded length (%d wrong, %d lists found)", (int) std::size (lists), wrong, choices));
        }

        {
            juce::StringArray names;
            for (const char* id : ParamID::all)
                names.addIfNotAlreadyThere (p.apvts.getParameter (id)->getName (64));
            check (names.size() == (int) std::size (ParamID::all), fmt ("%d parameters, %d distinct names", (int) std::size (ParamID::all), names.size()));
        }

        // What a parameter shows must be something it can read back (typing into a host's value field).
        {
            int wrong = 0;
            juce::String firstWrong;
            for (const char* id : ParamID::all)
            {
                auto* param = p.apvts.getParameter (id);
                for (const float v : { 0.0f, param->getDefaultValue(), 1.0f })
                {
                    const auto text = param->getText (v, 64);
                    const float back = param->getValueForText (text);
                    // one displayed digit of slack: the text is rounded for reading
                    const float shownAgain = param->getValueForText (param->getText (back, 64));
                    if (std::abs (back - v) > 0.02f || std::abs (shownAgain - back) > 1.0e-4f)
                    {
                        if (wrong++ == 0)
                            firstWrong = juce::String (id) + " shows '" + text + "' for " + juce::String (v, 3) + ", reads it as " + juce::String (back, 3);
                    }
                }
            }
            check (wrong == 0, wrong == 0 ? juce::String ("every parameter reads back the text it shows, at minimum, default and maximum")
                                          : fmt ("%d values do not read back; first: %s", wrong, firstWrong.toRawUTF8()));
        }
    }

    void testLfoTriggerModes()
    {
        section ("0.3 G11: Free follows the song, Note and Voice start at the key, and a restarted S&H repeats itself");

        // Pitch LFO: square, synced to 1/4, full depth (+/-100 cents). The transport is half a cycle in
        // when the key lands, where a song-locked square reads -100 cents and a restarted one +100.
        const auto input = noiseInput (72000, 3, 0.25f);
        auto centsAfterKey = [&input] (LfoTrig trig, int block)
        {
            Harness h (48000.0, block);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::pitchLfoOn, 1.0f);
            h.set (ParamID::pitchLfoShape, (float) (int) LfoShape::square);
            h.set (ParamID::pitchLfoSync, 5.0f);   // 1/4
            h.set (ParamID::pitchLfoDepth, 100.0f);
            h.set (ParamID::pitchLfoTrig, (float) (int) trig);
            const auto out = play (h, input, { keyDown (24037, 69) }, 0.5, 120.0);
            return centsBetween (pitchAt (out, 24037 + 800), 440.0);
        };

        double worstFree = 0.0, worstNote = 0.0, worstVoice = 0.0;
        for (const int block : { 64, 4096 })
        {
            worstFree = std::max (worstFree, std::abs (centsAfterKey (LfoTrig::free, block) + 100.0));
            worstNote = std::max (worstNote, std::abs (centsAfterKey (LfoTrig::note, block) - 100.0));
            worstVoice = std::max (worstVoice, std::abs (centsAfterKey (LfoTrig::voice, block) - 100.0));
        }
        check (worstFree <= 10.0, fmt ("Free, synced: the note starts at -100 cents with the song (off by %.1f at most, blocks of 64 and 4096)", worstFree));
        check (worstNote <= 10.0 && worstVoice <= 10.0,
               fmt ("Note and Voice, synced: the note starts at +100 cents, the top of its own cycle (off by %.1f and %.1f)", worstNote, worstVoice));

        // S&H at 1/16 on pitch: two keys at unrelated song positions. Restarted, both hear the same
        // first three steps; free-running, they hear whatever the song position gives.
        const auto longInput = noiseInput (192000, 4, 0.25f);
        auto stepDifference = [&longInput] (LfoTrig trig)
        {
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::pitchLfoOn, 1.0f);
            h.set (ParamID::pitchLfoShape, (float) (int) LfoShape::sampleHold);
            h.set (ParamID::pitchLfoSync, 9.0f);   // 1/16: 6000 samples at 120 BPM
            h.set (ParamID::pitchLfoDepth, 100.0f);
            h.set (ParamID::pitchLfoTrig, (float) (int) trig);
            h.set (ParamID::release, 5.0f);
            const juce::int64 first = 24000, second = 24000 + 96000 + 1777;
            const auto out = play (h, longInput, { keyDown (first, 69), keyUp (first + 20000, 69), keyDown (second, 69) }, 0.37, 120.0);
            double worst = 0.0;
            for (int step = 0; step < 3; ++step)
            {
                const double a = pitchAt (out, (size_t) (first + step * 6000 + 900), 12);
                const double b = pitchAt (out, (size_t) (second + step * 6000 + 900), 12);
                worst = std::max (worst, std::abs (centsBetween (a, b)));
            }
            return worst;
        };
        const double restarted = stepDifference (LfoTrig::note), running = stepDifference (LfoTrig::free);
        check (restarted <= 12.0 && running > 12.0,
               fmt ("S&H: with Note the two keys' first three steps agree within %.1f cents; with Free they differ by up to %.1f", restarted, running));

        // Once: one cycle, then the last value is held. A square ends its cycle at the bottom, and stays there.
        {
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::pitchLfoOn, 1.0f);
            h.set (ParamID::pitchLfoShape, (float) (int) LfoShape::square);
            h.set (ParamID::pitchLfoRate, 4.0f);   // one cycle = 12000 samples: +100 for 6000, -100 for 6000
            h.set (ParamID::pitchLfoDepth, 100.0f);
            h.set (ParamID::pitchLfoTrig, (float) (int) LfoTrig::once);
            const auto out = play (h, input, { keyDown (12000, 69) });
            const double early = centsBetween (pitchAt (out, 12000 + 600, 12), 440.0);
            const double later = centsBetween (pitchAt (out, 12000 + 6000 + 600, 12), 440.0);
            const double held = centsBetween (pitchAt (out, 12000 + 30000), 440.0);
            check (std::abs (early - 100.0) <= 12.0 && std::abs (later + 100.0) <= 12.0 && std::abs (held + 100.0) <= 6.0,
                   fmt ("Once, square: %+.0f cents, then %+.0f, then held at %+.0f after its one cycle", early, later, held));
        }

        // Fade: the LFO comes in over the fade time, per note.
        {
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::pitchLfoOn, 1.0f);
            h.set (ParamID::pitchLfoShape, (float) (int) LfoShape::square);
            h.set (ParamID::pitchLfoRate, 0.25f);   // +100 cents for the first two seconds
            h.set (ParamID::pitchLfoDepth, 100.0f);
            h.set (ParamID::pitchLfoTrig, (float) (int) LfoTrig::note);
            h.set (ParamID::pitchLfoFade, 1000.0f);
            const auto out = play (h, noiseInput (96000, 6, 0.25f), { keyDown (12000, 69) });
            const double start = centsBetween (pitchAt (out, 12000 + 300, 12), 440.0);
            const double half = centsBetween (pitchAt (out, 12000 + 24000 - 2048, 12), 440.0);
            const double full = centsBetween (pitchAt (out, 12000 + 60000), 440.0);
            check (start < 15.0 && std::abs (half - 50.0) <= 12.0 && std::abs (full - 100.0) <= 6.0,
                   fmt ("Fade 1 s: %+.0f cents at the start, %+.0f half-way, %+.0f once it is in", start, half, full));
        }
    }

    void testNoteEnvelope()
    {
        section ("0.3 G10: the note envelope bends pitch, formant and grain per note; tape stop slows a released note to a halt");

        {
            // Pitch +12 with a slow decay: the note starts an octave up.
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::envPitch, 12.0f);
            h.set (ParamID::envDecay, 5000.0f);
            const auto out = play (h, noiseInput (48000, 8, 0.25f), { keyDown (12000, 57) });
            // (Autocorrelation, not the FFT: a noise grain read at twice its speed folds faint tones under
            // the note, as Formant +12 always has, and the FFT finder would take the lowest of them.)
            const double cents = centsBetween (shortPitchHz (out, 12000 + 300, 4096, 300.0, 600.0), 220.0);
            check (cents > 1120.0 && cents <= 1225.0, fmt ("Env Pitch +12, slow decay: the note starts %+.0f cents up", cents));
        }
        {
            // Pitch +24 with a short decay: once it has passed, the note must be the same note with the
            // same tone as without the envelope (the grab is sized for the note, not for the dive).
            const auto input = resonantNoise (96000);
            auto settled = [&input] (float envPitch)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
                h.set (ParamID::grainCycles, 4.0f);
                h.set (ParamID::envPitch, envPitch);
                h.set (ParamID::envDecay, 100.0f);
                return play (h, input, { keyDown (48000, 60) });
            };
            const auto with = settled (24.0f), without = settled (0.0f);
            const double f0 = noteHz (60);
            double worst = 0.0;
            for (int k = 1; k <= 6; ++k)
                worst = std::max (worst, std::abs (lineLevelDb (with, 48000.0, f0 * k) - lineLevelDb (without, 48000.0, f0 * k)));
            check (worst <= 1.0, fmt ("Env Pitch +24, 100 ms decay: afterwards the first six harmonics are within %.2f dB of the plain note", worst));
        }
        {
            // Grain +8 in Live at Grain 2 must be audible from the first grab: ten alike cycles add up to
            // about 7 dB more than two (Auto Gain off so the level shows it).
            const auto input = harmonicInput (96000, 220.0, 8, 0.03);
            auto level = [&input] (float envGrain)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::autoGain, 0.0f);
                h.set (ParamID::envGrain, envGrain);
                h.set (ParamID::envDecay, 5000.0f);
                const auto out = play (h, input, { keyDown (48000, 57) });
                return rmsDb (out, 52000, 62000);
            };
            const double rise = level (8.0f) - level (0.0f);
            check (rise >= 5.0, fmt ("Env Grain +8 over Grain 2 in Live: %.1f dB louder on a source at the note's pitch (ten cycles against two: about 7)", rise));
        }
        {
            // Formant +12 in Live: the first grab already reads an octave up.
            const auto input = resonantNoise (96000);
            auto balance = [&input] (float envFormant)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::grainCycles, 4.0f);
                h.set (ParamID::refresh, 500.0f);
                h.set (ParamID::envFormant, envFormant);
                h.set (ParamID::envDecay, 5000.0f);
                const auto out = play (h, input, { keyDown (48000, 60) });
                return toneBalanceDb (slice (out, 48000 + 600, 8192), noteHz (60), 13);
            };
            const double up = balance (12.0f), plain = balance (0.0f);
            check (plain < -6.0 && up > 6.0, fmt ("Env Formant +12 in Live: energy at 2 kHz against 1 kHz is %+.1f dB at the start (%+.1f without)", up, plain));
        }
        {
            // Tape stop in Live with a 5 s release, the input cut at key-up. Half-way through the
            // release the note is two octaves down, and still sounding: it kept its grain instead of
            // grabbing the silence. Without tape stop the same release grabs silence.
            auto input = noiseInput (192000, 9, 0.25f);
            for (size_t i = 48000; i < input.size(); ++i)
                input[i] = 0.0f;
            auto run = [&input] (bool tapeStop)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::release, 5000.0f);
                h.set (ParamID::tapeStop, tapeStop ? 1.0f : 0.0f);
                return play (h, input, { keyDown (24000, 69), keyUp (48000, 69) });
            };
            const auto stopping = run (true), plain = run (false);
            const size_t half = 48000 + 120000;
            const double level = rmsDb (stopping, half - 2400, half + 2400), plainLevel = rmsDb (plain, half - 2400, half + 2400);
            const double cents = centsBetween (shortPitchHz (stopping, half - 1024, 2048, 60.0, 400.0), 110.0);
            check (level - plainLevel >= 20.0 && std::abs (cents) <= 150.0,
                   fmt ("Tape stop, half-way through a 5 s release: %+.0f cents from two octaves down, %.0f dB above the same release without it", cents, level - plainLevel));
        }
    }

    void testKeyboardSources()
    {
        section ("0.3 G05: bend range, mod wheel, aftertouch and expression pedal");

        const auto input = noiseInput (96000, 12, 0.25f);

        {
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::bendUp, 12.0f);
            h.set (ParamID::bendDown, 5.0f);
            const auto out = play (h, input, { keyDown (12000, 57), bendTo (30000, 16383), bendTo (60000, 0) });
            const double up = centsBetween (shortPitchHz (out, 40000, 8192, 300.0, 600.0), 220.0);
            const double down = centsBetween (pitchAt (out, 96000 - 16384, 14), 220.0);
            check (std::abs (up - 1200.0) <= 25.0 && std::abs (down + 500.0) <= 10.0,
                   fmt ("Bend Up 12, Bend Down 5: the wheel reaches %+.0f and %+.0f cents", up, down));
        }
        {
            // The default: the wheel brings in a vibrato of +/-50 cents.
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            const auto out = play (h, input, { keyDown (12000, 69), controller (24000, 1, 127) });
            double lo = 1.0e9, hi = -1.0e9;
            for (size_t at = 36000; at + 4096 < 84000; at += 1024)
            {
                const double c = centsBetween (shortPitchHz (out, at, 2048, 380.0, 500.0), 440.0);
                lo = std::min (lo, c);
                hi = std::max (hi, c);
            }
            const double still = centsBetween (pitchAt (out, 12000 + 600, 13), 440.0);
            check (std::abs (still) <= 5.0 && hi - lo >= 45.0 && hi - lo <= 150.0,
                   fmt ("mod wheel: steady before it (%+.1f cents), then a vibrato from %+.0f to %+.0f cents", still, lo, hi));
        }
        {
            // Level: the wheel fades the note out; "reset controllers" brings it back.
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::wheelDest, (float) (int) ModDest::level);
            const auto out = play (h, input, { keyDown (12000, 57), controller (36000, 1, 127), controller (60000, 121, 0) });
            const double before = rmsDb (out, 24000, 36000), faded = rmsDb (out, 48000, 60000), back = rmsDb (out, 72000, 96000);
            check (before - faded >= 40.0 && std::abs (back - before) <= 1.0,
                   fmt ("wheel to Level: down %.0f dB at full wheel, back within %.2f dB after a controller reset", before - faded, back - before));
        }
        {
            // Channel pressure is a two-byte message; here it is the only (so the last) event in its block.
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::touchDest, (float) (int) ModDest::level);
            const auto out = play (h, input, { keyDown (12000, 57), channelPressure (36000, 127) });
            const double drop = rmsDb (out, 24000, 36000) - rmsDb (out, 60000, 96000);
            check (drop >= 40.0, fmt ("channel pressure (2 bytes) to Level: down %.0f dB", drop));
        }
        {
            // Poly pressure reaches only its own key's voice.
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::touchDest, (float) (int) ModDest::level);
            const auto pressed = play (h, input, { keyDown (12000, 57), keyDown (12000, 64), polyPressure (36000, 64, 127) });
            Harness plainHarness (48000.0, 256);
            plainHarness.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            const auto plain = play (plainHarness, input, { keyDown (12000, 57), keyDown (12000, 64) });
            const double low = lineLevelDb (pressed, 48000.0, noteHz (57)) - lineLevelDb (plain, 48000.0, noteHz (57));
            const double high = lineLevelDb (pressed, 48000.0, noteHz (64)) - lineLevelDb (plain, 48000.0, noteHz (64));
            check (std::abs (low) <= 1.0 && high <= -30.0,
                   fmt ("poly pressure on E3 to Level: A2 changes by %+.2f dB, E3 by %+.0f dB", low, high));
        }
        {
            // The expression pedal rests at full; pulled back it moves its destination.
            const auto tone = resonantNoise (96000);
            auto balance = [&tone] (int pedal)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
                h.set (ParamID::grainCycles, 4.0f);
                h.set (ParamID::exprDest, (float) (int) ModDest::formant);
                const auto out = play (h, tone, { keyDown (24000, 60), controller (36000, 11, pedal) });
                return toneBalanceDb (out, noteHz (60));
            };
            const double rest = balance (127), back = balance (0);
            check (rest < -6.0 && back > 6.0,
                   fmt ("pedal to Formant: 2 kHz against 1 kHz is %+.1f dB with the pedal at rest and %+.1f dB pulled back (an octave up)", rest, back));
        }
        {
            // A 0.2 project never reacted to the mod wheel, and must not start to.
            auto render = [&input] (bool withWheel)
            {
                GrainLockProcessor source;
                auto xml = source.apvts.copyState().createXml();
                juce::Array<juce::XmlElement*> drop;
                bool past = false;
                for (const char* id : ParamID::all)
                {
                    if (past)
                        for (auto* node : xml->getChildWithTagNameIterator ("PARAM"))
                            if (node->getStringAttribute ("id") == id)
                                drop.add (node);
                    if (juce::String (id) == ParamID::outGain) past = true;
                }
                for (auto* node : drop)
                    xml->removeChildElement (node, true);
                juce::MemoryBlock block;
                juce::AudioProcessor::copyXmlToBinary (*xml, block);

                Harness h (48000.0, 256);
                h.proc.setStateInformation (block.getData(), (int) block.getSize());
                std::vector<ScriptEvent> events { keyDown (12000, 57) };
                if (withWheel)
                    for (int k = 0; k < 20; ++k)
                        events.push_back (controller (20000 + k * 3000, 1, (k * 37) % 128));
                return play (h, input, events);
            };
            const auto wheel = render (true), still = render (false);
            float difference = 0.0f;
            for (size_t i = 0; i < still.size(); ++i)
                difference = std::max (difference, std::abs (wheel[i] - still[i]));
            check (juce::exactlyEqual (difference, 0.0f), fmt ("a 0.2 project with the mod wheel moving: identical to the same notes without it (max difference %g)", (double) difference));
        }
    }

    //==========================================================================
    // Reference sounds

    void compareWithTable (const char* name, const std::vector<float>& now, const float* table, int count)
    {
        if (count == 0)
        {
            std::printf ("    %s: no table committed yet, nothing compared\n", name);
            return;
        }
        if ((int) now.size() != count)
        {
            check (false, fmt ("%s: %d numbers now, %d in the table", name, (int) now.size(), count));
            return;
        }

        int wrong = 0, worstAt = -1;
        double worst = 0.0;
        for (int i = 0; i < count; ++i)
        {
            const double a = now[(size_t) i], b = table[i];
            const double excess = std::abs (a - b) - (0.001 * std::max (std::abs (a), std::abs (b)) + 1.0e-6);
            if (excess > 0.0)
            {
                ++wrong;
                const double relative = std::abs (a - b) / std::max (1.0e-9, std::max (std::abs (a), std::abs (b)));
                if (relative > worst) { worst = relative; worstAt = i; }
            }
        }
        check (wrong == 0, wrong == 0 ? fmt ("%s: all %d numbers within 0.1%%", name, count)
                                      : fmt ("%s: %d of %d numbers differ; worst %.1f%% at %s", name, wrong, count, 100.0 * worst,
                                             fingerprint::describe (worstAt).toRawUTF8()));
    }

    void testFingerprints()
    {
        section ("Reference sounds: with 0.2's seam this build is 0.2; the 0.3 default sound has not drifted");
        compareWithTable ("table A (0.2)", fingerprint::table (true), fingerprint::tableA, fingerprint::countA);
        compareWithTable ("table B (0.3 defaults)", fingerprint::table (false), fingerprint::tableB, fingerprint::countB);
    }

    void testBlockSizes()
    {
        section ("The same notes give the same sound at any block size");

        for (const int config : { 0, 4, 6 })   // defaults, Glitch Drums (synced S&H), mono glide + LFOs
        {
            const auto reference = fingerprint::render (config, 0, false, [] (int) { return 256; });
            float worst = 0.0f;
            int worstBlock = 0;
            for (const int block : { 1, 64, 480, 4096, -1 })
            {
                juce::Random sizes (11);
                const auto out = fingerprint::render (config, 0, false, [&] (int) { return block > 0 ? block : 1 + sizes.nextInt (700); });
                float d = 0.0f;
                for (size_t i = 0; i < out.size(); ++i)
                    d = std::max (d, std::abs (out[i] - reference[i]));
                if (d > worst) { worst = d; worstBlock = block; }
            }
            check (worst <= 1.0e-4f, fmt ("%s: blocks of 1, 64, 480, 4096 and random sizes differ from 256 by %g at most (worst: %d)",
                                          fingerprint::describe (config * fingerprint::numSources * fingerprint::numbersPerRun).upToFirstOccurrenceOf (",", false, false).toRawUTF8(),
                                          (double) worst, worstBlock));
        }
    }

    int writeFingerprints (const juce::String& directory)
    {
        const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (directory);
        dir.createDirectory();
        bool ok = fingerprint::write (dir.getChildFile ("fingerprint-A.txt"), fingerprint::table (true));
        ok = fingerprint::write (dir.getChildFile ("fingerprint-B.txt"), fingerprint::table (false)) && ok;
        std::printf ("%s fingerprints to %s\n", ok ? "wrote" : "FAILED to write", dir.getFullPathName().toRawUTF8());
        return ok ? 0 : 1;
    }

    void testLfoSyncedSampleHold()
    {
        section ("Extra: tempo-synced S&H steps once per division");

        Lfo lfo;
        lfo.reset (7);
        const double rate = 48000.0, bpm = 120.0, beats = 0.25;   // 1/16 at 120 BPM = 6000 samples
        const int block = 512;
        const double increment = bpm / 60.0 / beats / rate;
        const int blocks = (int) (100.0 * (60.0 / bpm * beats) * rate / block);

        int changes = 0;
        float last = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            const double cycles = (double) b * block / rate * bpm / 60.0 / beats;
            lfo.syncTo (cycles - std::floor (cycles), (juce::int64) std::floor (cycles));
            for (int i = 0; i < block; ++i)
            {
                const float v = lfo.next (increment, LfoShape::sampleHold);
                if ((b > 0 || i > 0) && ! juce::exactlyEqual (v, last))
                    ++changes;
                last = v;
            }
        }

        check (changes >= 98 && changes <= 100, fmt ("%d steps across 99 division boundaries", changes));
    }

    void testThreeLfosTogether()
    {
        section ("Extra: pitch, formant and grain LFOs run at the same time");

        // Hold A4. Pitch LFO: square at 0.5 Hz, full depth, so the note sits at +100 cents for a second,
        // then -100 cents. Formant and grain LFOs run at full depth on top; with Pitch Lock on neither
        // may move the pitch.
        Harness h (48000.0, 512);
        h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
        const struct { int lfo; LfoShape shape; float rate; } lfos[] = {
            { (int) LfoTarget::pitch, LfoShape::square, 0.5f },
            { (int) LfoTarget::formant, LfoShape::sine, 0.7f },
            { (int) LfoTarget::grainCycles, LfoShape::triangle, 0.45f },
        };
        for (const auto& l : lfos)
        {
            const auto& ids = ParamID::lfo[l.lfo];
            h.set (ids.on, 1.0f);
            h.set (ids.shape, (float) (int) l.shape);
            h.set (ids.rate, l.rate);
            h.set (ids.depth, 100.0f);
        }

        const auto out = holdNoteOverNoise (h, 69, (juce::int64) (6.0 * h.rate));

        const double up = 440.0 * std::pow (2.0, 1.0 / 12.0), down = 440.0 * std::pow (2.0, -1.0 / 12.0);
        int atUp = 0, atDown = 0, windows = 0;
        const size_t windowLength = 16384 + 962 + 2;
        for (size_t start = 0; start + windowLength <= out.size(); start += 4800)
        {
            const std::vector<float> window (out.begin() + (std::ptrdiff_t) start,
                                             out.begin() + (std::ptrdiff_t) (start + windowLength));
            const double f = autocorrFundamentalHz (window, h.rate, 50.0, 2000.0);
            ++windows;
            if (std::abs (centsBetween (f, up)) <= 6.0)   ++atUp;
            if (std::abs (centsBetween (f, down)) <= 6.0) ++atDown;
        }

        ScopeFrame frame;
        const bool gotFrame = h.proc.getScopeFifo().pullLatest (frame);

        check (atUp >= 5 && atDown >= 5,
               fmt ("pitch LFO square: %d of %d windows at +100 cents, %d at -100 cents (formant and grain LFOs running)",
                    atUp, windows, atDown));
        check (gotFrame && frame.lfoActive[0] && frame.lfoActive[1] && frame.lfoActive[2],
               "the display is told all three LFOs are on");
    }

    void testLegacyLfoMigration()
    {
        section ("Extra: a v0.1 session (one shared LFO) loads onto the matching new LFO");

        auto legacyState = [] (double depth)
        {
            GrainLockProcessor old;
            auto xml = old.apvts.copyState().createXml();

            // Strip the new LFO parameters and add v0.1's, so this is exactly what v0.1 saved.
            juce::Array<juce::XmlElement*> newNodes;
            for (auto* node : xml->getChildWithTagNameIterator ("PARAM"))
                if (node->getStringAttribute ("id").containsIgnoreCase ("Lfo"))
                    newNodes.add (node);
            for (auto* node : newNodes)
                xml->removeChildElement (node, true);

            const std::pair<const char*, double> legacy[] = {
                { "lfoRate", 3.5 }, { "lfoSync", 5.0 }, { "lfoShape", 2.0 }, { "lfoDepth", depth }, { "lfoTarget", 1.0 }
            };
            for (const auto& [id, value] : legacy)
            {
                auto* node = xml->createNewChildElement ("PARAM");
                node->setAttribute ("id", id);
                node->setAttribute ("value", value);
            }

            juce::MemoryBlock block;
            juce::AudioProcessor::copyXmlToBinary (*xml, block);
            return block;
        };

        {
            const auto block = legacyState (40.0);
            GrainLockProcessor restored;
            restored.setStateInformation (block.getData(), (int) block.getSize());
            auto value = [&restored] (const char* id) { return restored.apvts.getRawParameterValue (id)->load(); };

            const bool formantMoved = value (ParamID::formantLfoOn) > 0.5f
                                   && std::abs (value (ParamID::formantLfoRate) - 3.5f) < 0.01f
                                   && juce::roundToInt (value (ParamID::formantLfoSync)) == 5
                                   && juce::roundToInt (value (ParamID::formantLfoShape)) == 2
                                   && std::abs (value (ParamID::formantLfoDepth) - 40.0f) < 0.01f;
            const bool othersOff = value (ParamID::pitchLfoOn) < 0.5f && value (ParamID::grainLfoOn) < 0.5f;
            check (formantMoved && othersOff, "old Formant-target LFO (3.5 Hz, 1/4, square, 40%) lands on the Formant LFO, others off");
        }

        {
            const auto block = legacyState (0.0);
            GrainLockProcessor restored;
            restored.setStateInformation (block.getData(), (int) block.getSize());
            bool allOff = true;
            for (const auto& ids : ParamID::lfo)
                allOff = allOff && restored.apvts.getRawParameterValue (ids.on)->load() < 0.5f
                                && std::abs (restored.apvts.getRawParameterValue (ids.depth)->load() - 50.0f) < 0.01f;
            check (allOff, "old LFO at zero depth: all three new LFOs off, at their default depth");
        }
    }

    void testHostGarbage()
    {
        section ("Extra: NaN and infinite host values are ignored");

        Harness h (48000.0, 256);
        FakePlayHead playHead;
        playHead.bpm = std::numeric_limits<double>::infinity();
        playHead.ppq = std::numeric_limits<double>::quiet_NaN();
        h.proc.setPlayHead (&playHead);

        for (const auto& ids : ParamID::lfo)
        {
            h.set (ids.on, 1.0f);
            h.set (ids.sync, 9.0f);
            h.set (ids.shape, (float) (int) LfoShape::sampleHold);
            h.set (ids.depth, 100.0f);
        }
        for (const char* id : { ParamID::formant, ParamID::fine, ParamID::smooth, ParamID::sustain })
            h.proc.apvts.getParameter (id)->setValueNotifyingHost (std::numeric_limits<float>::quiet_NaN());

        h.proc.resetLimiterStats();
        juce::Random rng (12);
        std::vector<MidiEvent> events;
        for (int k = 0; k < 20; ++k)
        {
            events.push_back (noteOnAt (4800 + k * 4800, 40 + k * 2, 100));
            events.push_back (noteOffAt (4800 + k * 4800 + 3000, 40 + k * 2));
        }

        std::vector<float> out;
        h.run ((juce::int64) (2.5 * h.rate), events, 0.4f, &out, nullptr, rng);

        bool finite = true;
        float peak = 0.0f;
        for (float v : out)
        {
            finite = finite && std::isfinite (v);
            peak = std::max (peak, std::abs (v));
        }

        check (finite && h.proc.getLimiterStats().nonFiniteInputs == 0,
               fmt ("infinite tempo, NaN position and NaN parameters: %d non-finite samples", h.proc.getLimiterStats().nonFiniteInputs));
        check (peak > 0.01f, fmt ("and the notes still sound (peak %.3f)", peak));
    }
}

namespace
{
    using namespace grainlock;

    void testFactoryPresets()
    {
        section ("Presets: each one sets exactly its values (defaults elsewhere) and plays safely");

        const auto& presets = factoryPresets();
        const char* expected[] = { "Init", "Robot Voice", "Stutter Gate", "Drone Pad", "Glitch Drums", "Formant Choir" };
        bool namesMatch = presets.size() == std::size (expected);
        for (size_t i = 0; namesMatch && i < presets.size(); ++i)
            namesMatch = juce::String (presets[i].name) == expected[i];
        check (namesMatch, fmt ("%d factory presets, in menu order: Init, Robot Voice, Stutter Gate, Drone Pad, Glitch Drums, Formant Choir",
                                (int) presets.size()));

        for (int index = 0; index < (int) presets.size(); ++index)
        {
            const auto& preset = presets[(size_t) index];
            Harness h (48000.0, 256);

            // Scramble everything first, so a value the preset forgets to reset would show up.
            juce::Random scramble (100 + index);
            for (const char* id : ParamID::all)
            {
                auto* p = h.proc.apvts.getParameter (id);
                p->setValueNotifyingHost (p->convertTo0to1 (p->convertFrom0to1 (scramble.nextFloat())));
            }

            h.proc.loadPreset (index);

            int wrong = 0;
            for (const char* id : ParamID::all)
            {
                auto* p = h.proc.apvts.getParameter (id);
                float want = p->convertFrom0to1 (p->getDefaultValue());
                for (const auto& v : preset.values)
                    if (juce::String (v.id) == id)
                        want = p->convertFrom0to1 (p->convertTo0to1 (v.value));

                const float got = h.proc.apvts.getRawParameterValue (id)->load();
                if (std::abs (got - want) > 1.0e-3f * std::max (1.0f, std::abs (want)))
                {
                    ++wrong;
                    std::printf ("    %s: %s is %.4f, expected %.4f\n", preset.name, id, got, want);
                }
            }
            check (wrong == 0 && h.proc.getCurrentPresetName() == preset.name,
                   fmt ("%s: all %d parameters as specified, name shown as \"%s\"", preset.name,
                        (int) std::size (ParamID::all), h.proc.getCurrentPresetName().toRawUTF8()));

            // Play it: a chord, then a run of short notes, over noise.
            std::vector<MidiEvent> events;
            for (int k = 0; k < 4; ++k)
                events.push_back (noteOnAt (24000, 48 + 4 * k, 100));
            for (int k = 0; k < 4; ++k)
                events.push_back (noteOffAt (72000, 48 + 4 * k));
            for (int k = 0; k < 16; ++k)
            {
                events.push_back (noteOnAt (84000 + k * 6000, 55 + (k * 5) % 24, 90));
                events.push_back (noteOffAt (84000 + k * 6000 + 3000, 55 + (k * 5) % 24));
            }
            std::sort (events.begin(), events.end(), [] (const MidiEvent& a, const MidiEvent& b) { return a.time < b.time; });

            h.proc.resetLimiterStats();
            juce::Random rng (40 + index);
            std::vector<float> out;
            h.run (240000, events, 0.4f, &out, nullptr, rng);

            float peak = 0.0f;
            bool finite = true;
            for (float v : out)
            {
                finite = finite && std::isfinite (v);
                peak = std::max (peak, std::abs (v));
            }
            check (finite && h.proc.getLimiterStats().nonFiniteInputs == 0 && peak <= 1.0f && peak > 0.01f,
                   fmt ("%s: plays cleanly (peak %.3f, %d non-finite)", preset.name, peak, h.proc.getLimiterStats().nonFiniteInputs));
        }
    }
}

namespace
{
    using namespace grainlock;

    /** Plays a chord into a preset and renders the real interface (2x) to a PNG, exactly as the
        editor draws it, including the loop display fed through the audio thread's FIFO. */
    bool renderSnapshot (const juce::File& file, const char* presetName, std::initializer_list<int> chord, int lfoTab)
    {
        const double rate = 48000.0;
        const int block = 1600;   // one display frame per block

        GrainLockProcessor proc;
        proc.setPlayConfigDetails (2, 2, rate, block);
        proc.prepareToPlay (rate, block);
        if (const int index = proc.getPresetIndex (presetName); index >= 0)
            proc.loadPreset (index);
        proc.apvts.state.setProperty ("lfoTab", lfoTab, nullptr);

        ui::GrainLookAndFeel lookAndFeel;
        ui::MainPanel panel (proc);
        panel.setLookAndFeel (&lookAndFeel);
        panel.setSize (ui::Theme::baseWidth, ui::Theme::baseHeight);

        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        double phase = 0.0;

        for (int b = 0; b < 60; ++b)
        {
            // A buzzy, voice-like source: 110 Hz with twelve harmonics at staggered phases.
            for (int i = 0; i < block; ++i)
            {
                float v = 0.0f;
                for (int k = 1; k <= 12; ++k)
                    v += (float) std::sin (phase * k + 0.3 * k * k) / (float) k;
                phase += juce::MathConstants<double>::twoPi * 110.0 / rate;
                buffer.setSample (0, i, 0.18f * v);
                buffer.setSample (1, i, 0.18f * v);
            }

            midi.clear();
            if (b == 10)
                for (int note : chord)
                {
                    const juce::uint8 on[3] = { 0x90, (juce::uint8) note, 100 };
                    midi.addEvent (on, 3, 0);
                }

            proc.processBlock (buffer, midi);

            if (b >= 10)
            {
                ScopeFrame frame;
                const bool fresh = proc.getScopeFifo().pullLatest (frame);
                panel.tick (fresh ? &frame : nullptr);
            }
        }

        const auto image = panel.createComponentSnapshot (panel.getLocalBounds(), true, 2.0f);
        file.deleteFile();
        bool ok = false;
        {
            juce::FileOutputStream out (file);
            juce::PNGImageFormat png;
            ok = out.openedOk() && image.isValid() && png.writeImageToStream (image, out);
        }
        panel.setLookAndFeel (nullptr);

        std::printf ("%s %s (%d x %d)\n", ok ? "wrote" : "FAILED to write", file.getFullPathName().toRawUTF8(),
                     image.getWidth(), image.getHeight());
        return ok;
    }

    int renderSnapshots (const juce::String& directory)
    {
        const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (directory);
        dir.createDirectory();

        bool ok = renderSnapshot (dir.getChildFile ("grainlock-formant-choir.png"), "Formant Choir", { 57, 60, 64, 67 }, (int) LfoTarget::formant);
        ok = renderSnapshot (dir.getChildFile ("grainlock-glitch-drums.png"), "Glitch Drums", { 48, 55 }, (int) LfoTarget::grainCycles) && ok;
        return ok ? 0 : 1;
    }
}

int main (int argc, char** argv)
{
    GRAINLOCK_INSTALL_ALLOC_HOOK();
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::String only;
    double allocMinutes = 10.0;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "--only" && i + 1 < argc)
            only = argv[++i];
        else if (arg == "--alloc-minutes" && i + 1 < argc)
            allocMinutes = juce::String (argv[++i]).getDoubleValue();
        else if (arg == "--snapshot" && i + 1 < argc)
            return renderSnapshots (argv[++i]);
        else if (arg == "--write-fingerprint" && i + 1 < argc)
            return writeFingerprints (argv[++i]);
    }

    auto wants = [&only] (const char* name) { return only.isEmpty() || only == name; };

    if (wants ("pitch"))    testPitchAccuracy();
    if (wants ("staccato")) testStaccato();
    if (wants ("dry"))      testDryWhenIdle();
    if (wants ("state"))    testStateRoundTrip();
    if (wants ("alloc"))    testNoAllocations (allocMinutes);
    if (wants ("presets"))  testFactoryPresets();
    if (wants ("extra"))
    {
        testRatesBlocksAndLayouts();
        testEnvelopeRelease();
        testBypass();
        testBypassSwitch();
        testLfoSyncedSampleHold();
        testHostGarbage();
        testThreeLfosTogether();
        testLegacyLfoMigration();
    }
    if (wants ("v03"))
    {
        testSeamFlatPower();
        testLoopPointNudge();
        testAutoGain();
        testFormantTrack();
        testOlderStates();
    }
    if (wants ("v03b"))
    {
        testParameterGuards();
        testLfoTriggerModes();
        testNoteEnvelope();
        testKeyboardSources();
    }
    if (wants ("reference"))
    {
        testFingerprints();
        testBlockSizes();
    }

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
