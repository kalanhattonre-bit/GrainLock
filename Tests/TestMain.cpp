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
                const bool legacyOff = juce::String (id) == ParamID::autoGain || juce::String (id) == ParamID::wheelDest
                                       || juce::String (id) == ParamID::sustainPedal;
                const bool ok = legacyOff ? plain (used, id) < 0.5f : atDefault (used, id);
                wrong += ok ? 0 : 1;
            }
            check (wrong == 0, fmt ("0.2 state onto a used instance: Auto Gain, mod wheel and sustain pedal off, all %d others at their defaults (%d wrong)",
                                    (int) std::size (ParamID::all) - 3, wrong));
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
                wrong += ((juce::String (id) == ParamID::wheelDest || juce::String (id) == ParamID::sustainPedal) ? plain (used, id) < 0.5f
                                                                                                                 : atDefault (used, id)) ? 0 : 1;
            check (wrong == 0 && plain (used, ParamID::autoGain) >= 0.5f,
                   fmt ("a state that saved Auto Gain on keeps it on; everything it did not save gets its older value (%d wrong)", wrong));
        }
        {
            GrainLockProcessor fresh;
            check (plain (fresh, ParamID::autoGain) >= 0.5f && plain (fresh, ParamID::sustainPedal) >= 0.5f,
                   "a new instance starts with Auto Gain and the sustain pedal on");
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
                             double startPpq = -1.0, double bpm = 120.0, std::vector<float>* rightOut = nullptr)
    {
        if (rightOut != nullptr)
            rightOut->assign (input.size(), 0.0f);

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
            {
                out[pos + (size_t) i] = buffer.getSample (0, i);
                if (rightOut != nullptr)
                    (*rightOut)[pos + (size_t) i] = buffer.getSample (1, i);
            }
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
                { ParamID::grabAt, 2 }, { ParamID::waitSync, 10 }, { ParamID::offsetSync, 10 }, { ParamID::refreshSync, 13 },
                { ParamID::holdMode, 4 }, { ParamID::holdTime, 9 }, { ParamID::spreadMode, 3 },
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
    // 0.3 stage 3: when a note grabs

    float largestDifference (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
    {
        float m = 0.0f;
        for (size_t i = from; i < to && i < a.size() && i < b.size(); ++i)
            m = std::max (m, std::abs (a[i] - b[i]));
        return m;
    }

    float largestSample (const std::vector<float>& x, size_t from, size_t to)
    {
        float m = 0.0f;
        for (size_t i = from; i < to && i < x.size(); ++i)
            m = std::max (m, std::abs (x[i]));
        return m;
    }

    void testWaitAndAtKey()
    {
        section ("0.3 G01: Wait and At Key put the grab after the key; a waiting note is silent and still plays its length");

        const auto input = noiseInput (144000, 21, 0.25f);

        {
            // A waited note is exactly the note a later key would have played (Mix 100 with Dry When
            // Idle off, so the output is the frozen sound alone).
            auto run = [&input] (juce::int64 key, float waitMs, float offsetMs)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::wait, waitMs);
                h.set (ParamID::offset, offsetMs);
                return play (h, input, { keyDown (key, 60) });
            };
            const auto waited = run (24000, 100.0f, 0.0f), later = run (28800, 0.0f, 0.0f);
            check (juce::exactlyEqual (largestSample (waited, 0, 28800), 0.0f)
                       && juce::exactlyEqual (largestDifference (waited, later, 28800, waited.size()), 0.0f)
                       && largestSample (waited, 30000, 60000) > 0.01f,
                   fmt ("Wait 100 ms: silent until the grab (peak %g), then identical to a key pressed 100 ms later (max difference %g)",
                        (double) largestSample (waited, 0, 28800), (double) largestDifference (waited, later, 28800, waited.size())));

            // Offset longer than the Wait: nothing to wait for; the slice just ends that much less far back.
            const auto both = run (24000, 100.0f, 300.0f), offsetOnly = run (24000, 0.0f, 200.0f);
            check (juce::exactlyEqual (largestDifference (both, offsetOnly, 0, both.size()), 0.0f) && largestSample (both, 24000, 60000) > 0.01f,
                   fmt ("Wait 100 ms with Offset 300 ms is Offset 200 ms: max difference %g", (double) largestDifference (both, offsetOnly, 0, both.size())));
        }

        {
            // At Key: the input is silent until the key and a steady tone after it. Before Key freezes
            // the silence; At Key waits for one loop region of tone and freezes that.
            auto tone = harmonicInput (144000, 220.0, 8, 0.03);
            for (size_t i = 0; i < 48000; ++i)
                tone[i] = 0.0f;
            auto run = [&tone] (juce::int64 key, bool atKey, int block)
            {
                Harness h (48000.0, block);
                h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
                h.set (ParamID::grainCycles, 8.0f);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::grabAt, atKey ? 1.0f : 0.0f);
                return play (h, tone, { keyDown (key, 57) });
            };
            const auto before = run (48000, false, 256), at = run (48000, true, 256), midTone = run (96000, false, 256);
            const double atLevel = rmsDb (at, 60000, 84000), reference = rmsDb (midTone, 108000, 132000);
            size_t firstSound = 0;
            while (firstSound < at.size() && juce::exactlyEqual (at[firstSound], 0.0f))
                ++firstSound;
            const int expected = 48000 + (int) std::ceil (8.1 * 48000.0 / 220.0);   // Smooth is 10%
            check (juce::exactlyEqual (largestSample (before, 0, before.size()), 0.0f) && std::abs (atLevel - reference) <= 1.0,
                   fmt ("a key pressed where a tone starts: Before Key freezes silence, At Key is within %+.2f dB of a note pressed mid-tone", atLevel - reference));
            check (std::abs ((int) firstSound - expected) <= 2,
                   fmt ("At Key, Grain 8 on A2: the note starts %d samples after the key (eight cycles of the note and the seam = %d)", (int) firstSound - 48000, expected - 48000));

            const auto bigBlocks = run (48000, true, 4096), smallBlocks = run (48000, true, 64);
            check (juce::exactlyEqual (largestDifference (bigBlocks, smallBlocks, 0, bigBlocks.size()), 0.0f),
                   "At Key: the same output at blocks of 64 and 4096");
        }

        {
            // A tap shorter than the wait, over input hotter than the limiter's ceiling: the dry is
            // untouched while the note waits, the note then sounds for as long as the key was down,
            // and the output is the dry again afterwards.
            const auto hot = noiseInput (96000, 22, 0.95f);
            Harness h (48000.0, 256);
            h.set (ParamID::wait, 100.0f);
            h.set (ParamID::release, 30.0f);
            const auto out = play (h, hot, { keyDown (24000, 60), keyUp (24000 + 2880, 60) });
            const float waiting = largestDifference (out, hot, 0, 28800);
            const float sounding = largestDifference (out, hot, 28800 + 480, 28800 + 2880);
            const float after = largestDifference (out, hot, 28800 + 2880 + 1440 + 24000, out.size());
            check (juce::exactlyEqual (waiting, 0.0f) && sounding > 0.05f && juce::exactlyEqual (after, 0.0f),
                   fmt ("a 60 ms tap with Wait 100 ms: dry untouched while it waits (%g), the note plays (%.2f), dry again afterwards (%g)",
                        (double) waiting, (double) sounding, (double) after));

            // And it is, to the sample, the note the same tap would have played 100 ms later.
            Harness plain (48000.0, 256);
            plain.set (ParamID::release, 30.0f);
            const auto reference = play (plain, hot, { keyDown (28800, 60), keyUp (28800 + 2880, 60) });
            check (juce::exactlyEqual (largestDifference (out, reference, 0, out.size()), 0.0f),
                   fmt ("the tapped note keeps its length: identical to the same tap made 100 ms later (max difference %g)",
                        (double) largestDifference (out, reference, 0, out.size())));
        }

        {
            // Wait as a note value: 1/16 at 120 BPM is 125 ms.
            auto run = [&input] (float waitMs, float waitSync)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::wait, waitMs);
                h.set (ParamID::waitSync, waitSync);
                return play (h, input, { keyDown (24000, 60) }, 0.0, 120.0);
            };
            const auto synced = run (0.0f, 3.0f), typed = run (125.0f, 0.0f);
            check (juce::exactlyEqual (largestDifference (synced, typed, 0, synced.size()), 0.0f) && largestSample (synced, 31000, 60000) > 0.01f
                       && juce::exactlyEqual (largestSample (synced, 0, 30000), 0.0f),
                   fmt ("Wait Sync 1/16 at 120 BPM is Wait 125 ms (max difference %g)", (double) largestDifference (synced, typed, 0, synced.size())));
        }

        {
            // Refresh as a note value: a Live note re-grabs on its own quarter notes. At 125 BPM a beat
            // is 23040 samples, which is not a whole number of this note's loops (218.18 samples), so a
            // clock that restarted at each grab would slip by part of a loop every beat. The input is
            // silent until 2.25 beats into the note; the third re-grab is the first that can hear it,
            // and it must fall within one loop of beat 3. (With Refresh in ms the note picks the sound
            // up at once.)
            std::vector<float> late = noiseInput (144000, 24, 0.25f);
            for (size_t i = 0; i < 75840; ++i)
                late[i] = 0.0f;
            auto firstSoundAt = [&late] (float refreshSync)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::refreshSync, refreshSync);
                const auto out = play (h, late, { keyDown (24000, 57) }, 0.0, 125.0);
                size_t first = 0;
                while (first < out.size() && juce::exactlyEqual (out[first], 0.0f))
                    ++first;
                return (int) first;
            };
            const int beat3 = 24000 + 3 * 23040;
            const int synced = firstSoundAt (5.0f), unsynced = firstSoundAt (0.0f);
            check (synced >= beat3 - 2 && synced <= beat3 + 220 && unsynced >= 75840 && unsynced < 78840,
                   fmt ("Refresh Sync 1/4 at 125 BPM: sound that starts 2.25 beats into a note is picked up on the note's beat 3 (%+d samples; a loop is 218); with Refresh 25 ms, %d ms after it starts",
                        synced - beat3, (unsynced - 75840) / 48));
        }

        {
            // Nine taps inside a long wait: every one is heard later, and none is left hanging.
            Harness h (48000.0, 256);
            h.set (ParamID::wait, 500.0f);
            h.set (ParamID::release, 30.0f);
            std::vector<ScriptEvent> events;
            for (int k = 0; k < 9; ++k)
            {
                events.push_back (keyDown (12000 + k * 4800, 48 + 2 * k));
                events.push_back (keyUp (12000 + k * 4800 + 2880, 48 + 2 * k));
            }
            const auto longInput = noiseInput (240000, 23, 0.25f);
            const auto out = play (h, longInput, events);
            int heard = 0;
            for (int k = 0; k < 9; ++k)
                heard += largestDifference (out, longInput, (size_t) (36000 + k * 4800 + 480), (size_t) (36000 + k * 4800 + 2880)) > 0.02f ? 1 : 0;
            const float after = largestDifference (out, longInput, 36000 + 8 * 4800 + 2880 + 96000, out.size());
            check (heard == 9 && juce::exactlyEqual (after, 0.0f),
                   fmt ("nine 60 ms taps with Wait 500 ms: %d heard half a second later, and the dry is untouched two seconds on (%g)", heard, (double) after));
        }

        {
            // "All notes off" during the wait: the note never plays.
            Harness h (48000.0, 256);
            h.set (ParamID::wait, 100.0f);
            const auto out = play (h, input, { keyDown (24000, 60), controller (26000, 123, 0) });
            check (juce::exactlyEqual (largestDifference (out, input, 0, out.size()), 0.0f),
                   fmt ("all-notes-off during the wait: no note follows (max difference from the dry %g)", (double) largestDifference (out, input, 0, out.size())));
        }

        {
            // Mono with Wait 300 ms. A legato move keeps the old note, unchanged, until the new key's
            // grab is due; and a first note that never sounded is replaced by the next key.
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::mono, 1.0f);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::wait, 300.0f);
            const auto out = play (h, input, { keyDown (24000, 57), keyDown (60000, 64) });
            const double held = centsBetween (pitchAt (out, 62000), noteHz (57)), moved = centsBetween (pitchAt (out, 80000), noteHz (64));
            check (std::abs (held) <= 5.0 && std::abs (moved) <= 5.0,
                   fmt ("mono legato A2 to E3 with Wait 300 ms: still A2 during the wait (%+.1f cents), E3 after it (%+.1f cents)", held, moved));

            Harness tap (48000.0, 256);
            tap.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            tap.set (ParamID::mono, 1.0f);
            tap.set (ParamID::dryWhenIdle, 0.0f);
            tap.set (ParamID::wait, 100.0f);
            const auto replaced = play (tap, input, { keyDown (24000, 57), keyUp (25440, 57), keyDown (26880, 64) });
            const double pitch = centsBetween (pitchAt (replaced, 40000), noteHz (64));
            check (juce::exactlyEqual (largestSample (replaced, 0, 26880 + 4800), 0.0f) && std::abs (pitch) <= 5.0,
                   fmt ("mono: a tapped A2 replaced by E3 during its wait never sounds; E3 starts at its own time (%+.1f cents)", pitch));
        }

        {
            // A synced Offset lands on the beat: a click placed just inside the end of the slice is
            // frozen, one placed just after it is not. At 40 BPM a quarter note (1.5 s) is too long and
            // is halved to an eighth (0.75 s), still on the grid.
            auto frozenPeak = [] (double bpm, int offsetSamples, int clickAfterEnd)
            {
                std::vector<float> click (144000, 0.0f);
                const juce::int64 key = 96000;
                click[(size_t) (key - offsetSamples + clickAfterEnd)] = 0.8f;
                Harness h (48000.0, 256);
                h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
                h.set (ParamID::grainCycles, 1.0f);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::offsetSync, 7.0f);   // 1/4
                const auto out = play (h, click, { keyDown (key, 57) }, 0.0, bpm);
                return largestSample (out, (size_t) key + 4800, out.size());
            };
            const int quarterAt95 = (int) std::lround (60.0 / 95.0 * 48000.0), eighthAt40 = (int) std::lround (0.5 * 60.0 / 40.0 * 48000.0);
            const float inside = frozenPeak (95.0, quarterAt95, -20), outside = frozenPeak (95.0, quarterAt95, 20);
            const float insideHalved = frozenPeak (40.0, eighthAt40, -20), outsideHalved = frozenPeak (40.0, eighthAt40, 20);
            check (inside > 0.05f && juce::exactlyEqual (outside, 0.0f),
                   fmt ("Offset 1/4 at 95 BPM: the slice ends %d samples back (a click just inside is frozen: %.2f; just after: %g)", quarterAt95, (double) inside, (double) outside));
            check (insideHalved > 0.05f && juce::exactlyEqual (outsideHalved, 0.0f),
                   fmt ("Offset 1/4 at 40 BPM is halved to 1/8: the slice ends %d samples back (inside %.2f, after %g)", eighthAt40, (double) insideHalved, (double) outsideHalved));
        }
    }

    void testWaitingVoiceRules()
    {
        section ("0.3 notes that wait: all-notes-off, taps in Mono, and which key a Mono note ends up on");

        const auto input = noiseInput (144000, 41, 0.25f);

        {
            // "All notes off" ends a tapped note that is playing out its length.
            Harness h (48000.0, 256);
            h.set (ParamID::wait, 500.0f);
            h.set (ParamID::release, 30.0f);
            const auto out = play (h, input, { keyDown (24000, 60), keyUp (43200, 60), controller (52800, 123, 0) });
            const float sounding = largestDifference (out, input, 49000, 52000), after = largestDifference (out, input, 60000, 66000);
            check (sounding > 0.02f && juce::exactlyEqual (after, 0.0f),
                   fmt ("a 400 ms tap with Wait 500 ms, all-notes-off 100 ms into the note: it was sounding (%.2f) and the dry is untouched 150 ms later (%g), not 300 ms later",
                        (double) sounding, (double) after));
        }

        {
            // A key pressed and let go on the same sample.
            auto run = [&input] (juce::int64 key, float waitMs)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::wait, waitMs);
                h.set (ParamID::attack, 0.0f);
                return play (h, input, { keyDown (key, 60), keyUp (key, 60) });
            };
            const auto waited = run (24000, 100.0f), later = run (28800, 0.0f);
            check (juce::exactlyEqual (largestDifference (waited, later, 0, waited.size()), 0.0f),
                   fmt ("a zero-length tap with Wait 100 ms is the zero-length tap made 100 ms later (max difference %g)",
                        (double) largestDifference (waited, later, 0, waited.size())));
        }

        auto mono = [] (Harness& h, float waitMs)
        {
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::mono, 1.0f);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::wait, waitMs);
        };

        {
            // Mono: a second tap made while the first note's release tail still rings.
            Harness h (48000.0, 256);
            mono (h, 100.0f);
            const auto out = play (h, input, { keyDown (24000, 57), keyUp (26880, 57), keyDown (33600, 64), keyUp (36480, 64) });
            const double pitch = centsBetween (shortPitchHz (out, 39400, 1600, 250.0, 500.0), noteHz (64));
            check (largestSample (out, 39000, 41000) > 0.01f && std::abs (pitch) <= 15.0,
                   fmt ("mono, two 60 ms taps 200 ms apart with Wait 100 ms: the second one sounds too (peak %.2f, %+.1f cents from E3)",
                        (double) largestSample (out, 39000, 41000), pitch));
        }

        {
            // Mono: A held, C pressed, E pressed, E let go before its turn: C is the newest key still
            // down, and gets the note at once.
            Harness h (48000.0, 256);
            mono (h, 300.0f);
            const auto out = play (h, input, { keyDown (12000, 57), keyDown (48000, 60), keyDown (52800, 64), keyUp (57600, 64) });
            const double pitch = centsBetween (pitchAt (out, 60000), noteHz (60));
            check (std::abs (pitch) <= 5.0, fmt ("mono, A2 held, C3 then E3 pressed, E3 let go before its turn: the note goes to C3 (%+.1f cents)", pitch));
        }

        {
            // Mono: A pressed, E pressed while A still waits, E let go: A is still down and gets the note.
            Harness h (48000.0, 256);
            mono (h, 300.0f);
            const auto out = play (h, input, { keyDown (24000, 57), keyDown (28800, 64), keyUp (33600, 64) });
            const double pitch = centsBetween (pitchAt (out, 48000), noteHz (57));
            check (juce::exactlyEqual (largestSample (out, 0, 43200), 0.0f) && std::abs (pitch) <= 5.0 && largestSample (out, 90000, 96000) > 0.01f,
                   fmt ("mono, A2 waiting, E3 pressed and let go: A2 sounds (%+.1f cents) and is still held two seconds on (peak %.2f)",
                        pitch, (double) largestSample (out, 90000, 96000)));
        }

        {
            // Mono, At Key, Grain 16: a low note waits a long time for its loop, a high one hardly at
            // all. A low tap followed by a held high key: the high key must not be let go when the
            // low tap's length runs out.
            Harness h (48000.0, 256);
            mono (h, 0.0f);
            h.set (ParamID::grabAt, 1.0f);
            h.set (ParamID::grainCycles, 16.0f);
            const auto out = play (h, input, { keyDown (12000, 36), keyUp (21600, 36), keyDown (26400, 84) });
            const double pitch = centsBetween (pitchAt (out, 50000), noteHz (84));
            check (largestSample (out, 60000, 70000) > 0.01f && std::abs (pitch) <= 10.0,
                   fmt ("mono, At Key: a held C5 that follows a tapped C1 is still sounding a second later (peak %.2f, %+.1f cents)",
                        (double) largestSample (out, 60000, 70000), pitch));
        }

        {
            // Live, At Key with an Offset far longer than the loop: the first slice ends 290 ms before
            // the key, and every later grab keeps that distance from the input.
            auto late = noiseInput (144000, 42, 0.25f);
            for (size_t i = 0; i < 60000; ++i)
                late[i] = 0.0f;
            Harness h (48000.0, 256);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::grabAt, 1.0f);
            h.set (ParamID::offset, 300.0f);
            const auto out = play (h, late, { keyDown (48000, 57) });
            const int region = (int) std::ceil ((2.0 + (double) 0.1f) * 48000.0 / 220.0);
            const int expected = 60000 + 14400 - region;
            size_t first = 0;
            while (first < out.size() && juce::exactlyEqual (out[first], 0.0f))
                ++first;
            check ((int) first >= expected && (int) first <= expected + 1400,
                   fmt ("Live, At Key, Offset 300 ms: sound that starts in the input is heard %d samples later (the note's distance from the input is %d)",
                        (int) first - 60000, 14400 - region));
        }

        {
            // Mono: A2 is ringing out. C3 is pressed and held, E3 is pressed over it and let go before
            // its turn: C3 is still down, and must get the note.
            Harness h (48000.0, 256);
            mono (h, 500.0f);
            h.set (ParamID::release, 2000.0f);
            const auto out = play (h, input, { keyDown (12000, 57), keyUp (60000, 57), keyDown (69600, 60), keyDown (74400, 64), keyUp (79200, 64) });
            const double pitch = centsBetween (pitchAt (out, 110000), noteHz (60));
            check (std::abs (pitch) <= 5.0 && largestSample (out, 130000, 140000) > 0.01f,
                   fmt ("mono, over a release tail: C3 held, E3 pressed and taken back: C3 sounds (%+.1f cents) and is still held (peak %.2f)",
                        pitch, (double) largestSample (out, 130000, 140000)));
        }

        {
            // Mono, At Key, Grain 16: a waiting low note is taken over by a high key, which is let go
            // again. The low key gets the note back, and its long loop must start now, not reach back
            // into the silence before it.
            auto tone = harmonicInput (144000, 220.0, 8, 0.03);
            for (size_t i = 0; i < 48000; ++i)
                tone[i] = 0.0f;
            Harness h (48000.0, 256);
            mono (h, 0.0f);
            h.set (ParamID::grabAt, 1.0f);
            h.set (ParamID::grainCycles, 16.0f);
            const auto out = play (h, tone, { keyDown (48000, 36), keyDown (48480, 76), keyUp (48960, 76) });
            const int region = (int) std::ceil ((16.0 + (double) 0.1f) * 48000.0 / noteHz (36));
            size_t first = 0;
            while (first < out.size() && juce::exactlyEqual (out[first], 0.0f))
                ++first;
            check ((int) first >= 48960 + region && (int) first <= 48960 + region + 2,
                   fmt ("mono, At Key: a waiting C1 handed back by E5 starts one whole C1 loop after that (%d samples; its loop is %d)",
                        (int) first - 48960, region));
        }
    }

    //==========================================================================
    // 0.3 stage 3: what a note grabs

    size_t firstSoundIn (const std::vector<float>& x)
    {
        size_t first = 0;
        while (first < x.size() && juce::exactlyEqual (x[first], 0.0f))
            ++first;
        return first;
    }

    void testInputTracker()
    {
        section ("0.3 the input tracker: level, hiss and hits, as Snap and Threshold read them");

        InputTracker tracker;
        tracker.prepare (48000.0, 153664);

        // Silence, noise, then a 220 Hz tone with a 2 ms hole and a 5 ms hole in it.
        const int total = 81920;
        juce::Random rng (5);
        for (int i = 0; i < total; ++i)
        {
            float x = 0.0f;
            const bool hole = (i >= 72000 && i < 72096) || (i >= 76896 && i < 77136);
            if (i == 24000)                  x = 0.25f;
            else if (i > 24000 && i < 48000) x = (rng.nextFloat() * 2.0f - 1.0f) * 0.25f;
            else if (i >= 48000 && ! hole)   x = 0.2f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * (double) i / 48000.0);
            tracker.push (x, x);
        }

        juce::int64 onset = -1, none = -1;
        const bool found = tracker.nearestOnset (24100, 2400, onset);
        const bool foundFarAway = tracker.nearestOnset (12000, 2400, none);
        check (found && onset == 24000 && ! foundFarAway,
               fmt ("a burst after silence is logged as a hit at its first sample (found at %d, expected 24000)", (int) onset));

        // Is the stretch from..to above 0.05 all the way through?
        auto covered = [&tracker] (int from, int to) { return tracker.covered (total - 1 - to, to - from, 0.05f); };
        check (covered (32000, 40000) && covered (60000, 71000) && ! covered (22000, 26000) && covered (46000, 50000),
               fmt ("level: noise %d, tone %d, a stretch that starts in silence %d, noise running into the tone %d (expected 1 1 0 1)",
                    (int) covered (32000, 40000), (int) covered (60000, 71000), (int) covered (22000, 26000), (int) covered (46000, 50000)));
        check (covered (71000, 73000) && ! covered (76000, 78000) && ! covered (71000, 72050) && ! covered (72050, 73000),
               fmt ("holes: a 2 ms hole inside a stretch is bridged (%d), a 5 ms hole is not (%d), and a stretch may not end (%d) or start (%d) in one",
                    (int) covered (71000, 73000), (int) covered (76000, 78000), (int) covered (71000, 72050), (int) covered (72050, 73000)));

        const float hissInNoise = tracker.hissShare (total - 1 - 44000, 12000), hissInTone = tracker.hissShare (total - 1 - 71000, 11000);
        check (hissInNoise > 0.8f && hissInTone < 0.05f,
               fmt ("hiss: %.0f%% of the noise is flagged, %.0f%% of the tone", 100.0 * (double) hissInNoise, 100.0 * (double) hissInTone));
    }

    void testSnap()
    {
        section ("0.3 G02: Snap starts the loop on the hit, whether the key was early or late");

        // Silence, then a burst that starts on sample 30000.
        auto burst = noiseInput (96000, 31, 0.25f);
        for (size_t i = 0; i < 30000; ++i)
            burst[i] = 0.0f;
        burst[30000] = 0.25f;

        auto run = [&burst] (juce::int64 key, float snapMs)
        {
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::snap, snapMs);
            return play (h, burst, { keyDown (key, 60) });
        };

        // The loop of a C3 at Grain 2 and Smooth 10% reads this much audio.
        const int region = (int) std::ceil ((2.0 + (double) 0.1f) * 48000.0 / noteHz (60));
        const auto reference = run (30000 + region, 0.0f);   // a key pressed exactly when the hit's first loop is complete

        const auto early = run (30000 - 960, 50.0f);
        check (juce::exactlyEqual (largestDifference (early, reference, 0, early.size()), 0.0f) && largestSample (early, 32000, 60000) > 0.01f,
               fmt ("a key 20 ms early: the note is the one a key at the end of the hit's first loop plays (max difference %g; first sound %d samples after the hit, loop %d)",
                    (double) largestDifference (early, reference, 0, early.size()), (int) firstSoundIn (early) - 30000, region));

        const auto earlyNoSnap = run (30000 - 960, 0.0f);
        check (juce::exactlyEqual (largestSample (earlyNoSnap, 0, earlyNoSnap.size()), 0.0f),
               "the same early key without Snap freezes the silence before the hit");

        // A late key grabs at once, and plays the same loop the reference does, that much later.
        const auto late = run (30000 + 960, 50.0f);
        const size_t shift = (size_t) (960 - region);
        float worst = 0.0f;
        for (size_t i = (size_t) (30000 + region); i + shift < late.size(); ++i)
            worst = std::max (worst, std::abs (late[i + shift] - reference[i]));
        check (juce::exactlyEqual (worst, 0.0f) && (int) firstSoundIn (late) >= 30960 && (int) firstSoundIn (late) <= 30962,
               fmt ("a key 20 ms late: starts at the key (%+d samples) with the same loop (max difference %g)",
                    (int) firstSoundIn (late) - 30960, (double) worst));

        // No hit within reach: the note grabs as it would without Snap, once Snap's reach has passed.
        const auto steady = noiseInput (96000, 32, 0.25f);
        Harness h (48000.0, 256);
        h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
        h.set (ParamID::dryWhenIdle, 0.0f);
        h.set (ParamID::snap, 50.0f);
        const auto noHit = play (h, steady, { keyDown (48000, 60) });
        const int started = (int) firstSoundIn (noHit) - 48000;
        check (started >= 2400 && started <= 2402 && largestSample (noHit, 52000, 90000) > 0.01f,
               fmt ("steady input, no hit: the note starts %d samples after the key (Snap's reach is 2400)", started));
    }

    void testThreshold()
    {
        section ("0.3 G03: Threshold waits for sound, keeps a Live note's last good grain, and Gate follows the input");

        // A steady tone that starts one second in; before it, silence.
        auto tone = harmonicInput (192000, 220.0, 3, 0.05);
        for (size_t i = 0; i < 48000; ++i)
            tone[i] = 0.0f;

        {
            auto run = [&tone] (juce::int64 key, float thresholdDb)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::threshold, thresholdDb);
                h.set (ParamID::maxWait, 2000.0f);
                return play (h, tone, { keyDown (key, 57) });
            };
            const auto waited = run (36000, -40.0f), midTone = run (96000, -80.0f);
            const int region = (int) std::ceil ((2.0 + (double) 0.1f) * 48000.0 / 220.0);
            const int started = (int) firstSoundIn (waited) - 48000;
            const double level = rmsDb (waited, 60000, 84000) - rmsDb (midTone, 108000, 132000);
            check (started >= region && started <= region + 96 && std::abs (level) <= 1.0,
                   fmt ("a key pressed 250 ms before the sound: the note starts %d samples after the sound does (its loop is %d), %+.2f dB against a note pressed mid-tone",
                        started, region, level));
        }

        {
            // Input that never reaches the Threshold: the note starts when Max Wait runs out, unless
            // the key has come up by then.
            const auto quiet = noiseInput (96000, 33, 0.001f);
            auto run = [&quiet] (bool release)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::threshold, -40.0f);
                h.set (ParamID::maxWait, 500.0f);
                std::vector<ScriptEvent> events { keyDown (24000, 60) };
                if (release)
                    events.push_back (keyUp (36000, 60));
                return play (h, quiet, events);
            };
            const auto held = run (false), released = run (true);
            const int started = (int) firstSoundIn (held) - 24000;
            check (started >= 24000 && started <= 24002 && juce::exactlyEqual (largestSample (released, 0, released.size()), 0.0f),
                   fmt ("input below the Threshold: a held key starts %d samples later (Max Wait is 24000); a key let go before that never sounds", started));
        }

        // A tone for a second and a half, half a second of silence, then the tone again.
        auto gapped = harmonicInput (192000, 220.0, 3, 0.05);
        for (size_t i = 72000; i < 96000; ++i)
            gapped[i] = 0.0f;

        {
            auto run = [&gapped] (float thresholdDb, bool gate)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::threshold, thresholdDb);
                h.set (ParamID::gate, gate ? 1.0f : 0.0f);
                return play (h, gapped, { keyDown (24000, 57) });
            };
            const auto plain = run (-80.0f, false), kept = run (-40.0f, false), gated = run (-40.0f, true);
            const double keptInGap = rmsDb (kept, 80000, 94000) - rmsDb (kept, 48000, 70000);
            const float plainInGap = largestSample (plain, 80000, 94000), gatedInGap = largestSample (gated, 80000, 94000);
            const double gatedAfter = rmsDb (gated, 110000, 130000) - rmsDb (gated, 48000, 70000);
            check (std::abs (keptInGap) <= 1.0 && juce::exactlyEqual (plainInGap, 0.0f),
                   fmt ("Live through a gap in the input: with Threshold the note keeps its last good grain (%+.2f dB); without, it freezes the silence (peak %g)",
                        keptInGap, (double) plainInGap));
            check (juce::exactlyEqual (gatedInGap, 0.0f) && std::abs (gatedAfter) <= 1.0,
                   fmt ("Gate: silent in the gap (peak %g), back when the input is (%+.2f dB)", (double) gatedInGap, gatedAfter));
        }

        {
            // A tone, then hiss. Skip Hiss keeps the tone; without it the hiss is frozen (plenty of
            // energy near 2 kHz, where the tone has none).
            auto hissy = harmonicInput (144000, 220.0, 5, 0.05);
            const auto noise = noiseInput (144000, 34, 0.1f);
            for (size_t i = 60000; i < hissy.size(); ++i)
                hissy[i] = noise[i];
            auto run = [&hissy] (bool skipHiss)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::grainCycles, 1.0f);
                h.set (ParamID::skipHiss, skipHiss ? 1.0f : 0.0f);
                return play (h, hissy, { keyDown (24000, 57) });
            };
            const double skipped = toneBalanceDb (slice (run (true), 144000 - 32768, 32768), 220.0);
            const double frozen = toneBalanceDb (slice (run (false), 144000 - 32768, 32768), 220.0);
            check (skipped < -30.0 && frozen > -15.0,
                   fmt ("a tone, then hiss: with Skip Hiss the note stays on the tone (2 kHz against 1 kHz: %+.1f dB); without, it freezes the hiss (%+.1f dB)",
                        skipped, frozen));
        }

        {
            // Snap 25 ms, Threshold on, Max Wait 0, steady input well above the Threshold and no hit: a
            // 20 ms tap plays once Snap's reach has passed (the level is looked at on the last chance).
            const auto steady = noiseInput (96000, 44, 0.25f);
            Harness h (48000.0, 256);
            h.set (ParamID::snap, 25.0f);
            h.set (ParamID::threshold, -40.0f);
            h.set (ParamID::maxWait, 0.0f);
            h.set (ParamID::release, 30.0f);
            const auto out = play (h, steady, { keyDown (48000, 60), keyUp (48960, 60) });
            const float sounding = largestDifference (out, steady, 49300, 50100);
            check (sounding > 0.02f && juce::exactlyEqual (largestDifference (out, steady, 0, 49200), 0.0f),
                   fmt ("Snap 25 ms, Threshold on, Max Wait 0, no hit: a 20 ms tap over loud input still plays, 25 ms later (%.2f from the dry)", (double) sounding));
        }
    }

    void testGridAndSkip()
    {
        section ("0.3 G04: grid grabs land on the song's lines, and Skip leaves grabs out");

        // Silent until 1.25 s (2.5 beats at 120 BPM), then noise.
        auto late = noiseInput (144000, 35, 0.25f);
        for (size_t i = 0; i < 60000; ++i)
            late[i] = 0.0f;

        auto firstSoundAt = [&late] (bool grid, float skip, int block)
        {
            Harness h (48000.0, block);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::refreshSync, 5.0f);   // 1/4
            h.set (ParamID::gridGrabs, grid ? 1.0f : 0.0f);
            h.set (ParamID::skipChance, skip);
            const auto out = play (h, late, { keyDown (6000, 57) }, 0.0, 120.0);
            return (int) firstSoundIn (out);
        };

        // The key is a quarter of a beat late (sample 6000). Its own quarter notes fall on 30000,
        // 54000 (still silent) and 78000; the song's lines fall on 24000, 48000 and 72000.
        const int own = firstSoundAt (false, 0.0f, 256), grid = firstSoundAt (true, 0.0f, 256);
        check (grid >= 72000 && grid <= 72002 && own >= 78000 - 2 && own <= 78000 + 480,
               fmt ("Refresh Sync 1/4 with the key a sixteenth late: on the grid the sound is picked up on beat 4 of the bar (%+d samples); without, on the note's own beat (%+d samples from it)",
                    grid - 72000, own - 78000));
        check (firstSoundAt (true, 0.0f, 64) == grid && firstSoundAt (true, 0.0f, 4096) == grid,
               "the grid line lands on the same sample at blocks of 64, 256 and 4096");
        check (firstSoundAt (true, 100.0f, 256) == (int) late.size() && firstSoundAt (false, 100.0f, 256) == (int) late.size(),
               "Skip 100%: no re-grab is ever taken, on the grid or off it");

        // Skip 50% on a 1/16 grid: about half of the lines grab. The loop is 2400 / 11 samples long, so
        // the output 4800 samples on is the same again unless a grab came between.
        {
            const auto steady = noiseInput (144000, 36, 0.25f);
            auto grabs = [&steady] (float skip)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::refreshSync, 9.0f);   // 1/16 = 6000 samples at 120 BPM
                h.set (ParamID::gridGrabs, 1.0f);
                h.set (ParamID::skipChance, skip);
                h.set (ParamID::attack, 0.0f);
                const auto out = play (h, steady, { keyDown (3000, 57) }, 0.0, 120.0);

                int changed = 0;
                for (int line = 2; line < 22; ++line)
                {
                    const size_t at = (size_t) (line * 6000);
                    float difference = 0.0f;
                    for (size_t i = 0; i < 2000; ++i)
                        difference = std::max (difference, std::abs (out[at + 2300 + i] - out[at - 2500 + i]));
                    changed += difference > 0.01f ? 1 : 0;
                }
                return changed;
            };
            const int all = grabs (0.0f), half = grabs (50.0f);
            check (all == 20 && half >= 4 && half <= 16,
                   fmt ("20 lines of a 1/16 grid: %d grab with Skip 0%%, %d with Skip 50%%", all, half));
        }

        {
            // A key pressed exactly on a line: its own grab is the line's. A second grab a sample later
            // would crossfade two copies of the same slice.
            const auto steady = noiseInput (96000, 43, 0.25f);
            auto run = [&steady] (bool grid)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::attack, 0.0f);
                h.set (ParamID::refreshSync, 5.0f);
                h.set (ParamID::gridGrabs, grid ? 1.0f : 0.0f);
                return play (h, steady, { keyDown (24000, 57) }, 0.0, 120.0);
            };
            const auto onGrid = run (true), offGrid = run (false);
            check (juce::exactlyEqual (largestDifference (onGrid, offGrid, 24000, 24218), 0.0f) && largestSample (onGrid, 24000, 24218) > 0.01f,
                   fmt ("a key pressed on a grid line: its first loop is the one it plays with the grid off (max difference %g)",
                        (double) largestDifference (onGrid, offGrid, 24000, 24218)));
        }

        {
            // The host cycles on exactly one line (one beat, Refresh Sync 1/4): the song comes back to
            // the same line number every time, and every return is a line. Tried with the block
            // boundary on the wrap (240) and off it (256).
            auto cycled = noiseInput (96000, 45, 0.25f);
            for (size_t i = 0; i < 30000; ++i)
                cycled[i] = 0.0f;

            auto firstSoundAt = [&cycled] (int block)
            {
                Harness h (48000.0, block);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::refreshSync, 5.0f);
                h.set (ParamID::gridGrabs, 1.0f);

                FakePlayHead playHead;
                playHead.bpm = 120.0;
                h.proc.setPlayHead (&playHead);
                juce::AudioBuffer<float> buffer (2, block);
                juce::MidiBuffer midi;
                midi.ensureSize (64);

                int first = -1;
                const int total = (int) cycled.size();
                for (int pos = 0; pos < total; pos += block)
                {
                    const int n = std::min (block, total - pos);
                    buffer.setSize (2, n, false, false, true);
                    for (int i = 0; i < n; ++i)
                    {
                        buffer.setSample (0, i, cycled[(size_t) (pos + i)]);
                        buffer.setSample (1, i, cycled[(size_t) (pos + i)]);
                    }

                    midi.clear();
                    if (pos <= 6000 && 6000 < pos + n)
                    {
                        const juce::uint8 noteOn[3] = { 0x90, 57, 100 };
                        midi.addEvent (noteOn, 3, 6000 - pos);
                    }

                    playHead.ppq = 1.0 + std::fmod ((double) pos / 24000.0, 1.0);
                    h.proc.processBlock (buffer, midi);

                    for (int i = 0; i < n && first < 0; ++i)
                        if (! juce::exactlyEqual (buffer.getSample (0, i), 0.0f))
                            first = pos + i;
                }
                h.proc.setPlayHead (nullptr);
                return first;
            };
            const int split = firstSoundAt (240), unsplit = firstSoundAt (256);
            check (split >= 48000 && split <= 48002 && unsplit == split,
                   fmt ("a host cycle of one grid line: sound that starts a quarter of the way through is picked up at the next return (sample %d, and %d when the block does not end on the wrap; the return is at 48000)",
                        split, unsplit));
        }
    }

    void testFeedback()
    {
        section ("0.3 G19: Feedback puts the frozen sound back into what is grabbed next, and always dies away");

        // One second of noise, then silence.
        auto input = noiseInput (240000, 37, 0.25f);
        for (size_t i = 48000; i < input.size(); ++i)
            input[i] = 0.0f;

        auto run = [&input] (float feedbackPercent)
        {
            Harness h (48000.0, 256);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::feedback, feedbackPercent);
            return play (h, input, { keyDown (12000, 57) });
        };

        const auto dry = run (0.0f), half = run (50.0f);
        const float before = largestSample (half, 36000, 48000);
        const float dryTail = largestSample (dry, 52800, 60000), halfTail = largestSample (half, 52800, 60000);
        const float halfEnd = largestSample (half, 192000, 240000);
        check (juce::exactlyEqual (dryTail, 0.0f) && halfTail > 0.001f * before && halfEnd < 0.001f * before,
               fmt ("the input stops under a held Live note: without Feedback the note is silent 100 ms later (peak %g); at 50%% it rings on (%.1f dB) and has died away 3 s later (%.1f dB)",
                    (double) dryTail, 20.0 * std::log10 ((double) (halfTail / before) + 1.0e-12), 20.0 * std::log10 ((double) (halfEnd / before) + 1.0e-12)));

        // The worst case for a loop that feeds itself: every cycle alike (Grain 16), Auto Gain off,
        // a four-note chord, Feedback 100%, input that never stops. It must stay bounded BEFORE the limiter.
        {
            const auto steady = noiseInput (240000, 38, 0.25f);
            auto peakBeforeLimiter = [&steady] (float feedbackPercent)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::grainCycles, 16.0f);
                h.set (ParamID::autoGain, 0.0f);
                h.set (ParamID::feedback, feedbackPercent);
                const auto out = play (h, steady, { keyDown (12000, 45), keyDown (12000, 52), keyDown (12000, 57), keyDown (12000, 64) });
                const auto stats = h.proc.getLimiterStats();
                return stats.nonFiniteInputs == 0 && ! out.empty() ? stats.maxInputPeak : 1.0e9f;
            };
            const float without = peakBeforeLimiter (0.0f), with = peakBeforeLimiter (100.0f);
            check (with < 4.0f * without && with < 8.0f,
                   fmt ("Feedback 100%%, Grain 16, Auto Gain off, four notes, 5 s of input: peak before the limiter %.2f (%.2f without Feedback)",
                        (double) with, (double) without));
        }

        // One bad input sample must not stay in the loop.
        {
            auto bad = noiseInput (240000, 39, 0.25f);
            bad[30000] = std::numeric_limits<float>::quiet_NaN();
            Harness h (48000.0, 256);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::feedback, 50.0f);
            const std::vector<float> head (bad.begin(), bad.begin() + 96000), tail (bad.begin() + 96000, bad.end());
            play (h, head, { keyDown (12000, 57) });
            h.proc.resetLimiterStats();   // the bad sample itself (and the grains that held it) are behind us
            const auto out = play (h, tail, {});
            const int late = h.proc.getLimiterStats().nonFiniteInputs;
            const float peak = largestSample (out, 104000, 144000);
            check (late == 0 && peak > 0.01f,
                   fmt ("one NaN input sample at Feedback 50%%: from a second later nothing non-finite reaches the limiter (%d) and the note still sounds (peak %.2f)",
                        late, (double) peak));
        }

        {
            // The send always carries the gain of the summed cycles, whatever Auto Gain says. With it
            // off and sixteen alike cycles, a send without that gain would come back twice as loud each
            // time round at 50%.
            Harness h (48000.0, 256);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::grainCycles, 16.0f);
            h.set (ParamID::autoGain, 0.0f);
            h.set (ParamID::feedback, 50.0f);
            const auto out = play (h, input, { keyDown (12000, 57) });
            const float level = largestSample (out, 36000, 48000), end = largestSample (out, 192000, 240000);
            check (end < 0.001f * level,
                   fmt ("Feedback 50%%, Grain 16, Auto Gain off: 3 s after the input stops the note has died away (%.1f dB)",
                        20.0 * std::log10 ((double) (end / level) + 1.0e-12)));
        }

        {
            // A chord sends no more than one note does. Four notes at 100%: once the input stops the
            // level before the limiter must fall, not sit at the saturator's ceiling.
            Harness h (48000.0, 256);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::grainCycles, 16.0f);
            h.set (ParamID::autoGain, 0.0f);
            h.set (ParamID::feedback, 100.0f);
            play (h, noiseInput (48000, 38, 0.25f), { keyDown (12000, 45), keyDown (12000, 52), keyDown (12000, 57), keyDown (12000, 64) });
            const float loud = h.proc.getLimiterStats().maxInputPeak;
            play (h, std::vector<float> (144000, 0.0f), {});
            h.proc.resetLimiterStats();
            play (h, std::vector<float> (48000, 0.0f), {});
            const float later = h.proc.getLimiterStats().maxInputPeak;
            check (later < 0.25f * loud,
                   fmt ("Feedback 100%%, four notes, Grain 16, Auto Gain off: the peak before the limiter is %.2f with input and %.2f in the fourth second without", (double) loud, (double) later));
        }

        {
            // The Grain LFO changes the loop's shape sixty times a second. With Feedback up, each of
            // those crossfades is between two copies of the same sound, and must not bulge.
            Harness h (48000.0, 256);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::grainCycles, 6.0f);
            h.set (ParamID::grainLfoOn, 1.0f);
            h.set (ParamID::grainLfoShape, (float) (int) LfoShape::square);
            h.set (ParamID::grainLfoRate, 30.0f);
            h.set (ParamID::grainLfoDepth, 50.0f);
            h.set (ParamID::feedback, 90.0f);
            const auto out = play (h, input, { keyDown (12000, 57) });
            const float level = largestSample (out, 36000, 48000), end = largestSample (out, 192000, 240000);
            check (end < 0.003f * level,
                   fmt ("Feedback 90%% with the Grain LFO stepping at 30 Hz: 3 s after the input stops the note is at %.1f dB",
                        20.0 * std::log10 ((double) (end / level) + 1.0e-12)));
        }

        {
            // The Formant LFO changes how alike the cycles are between two grabs (Refresh 250 ms). The
            // send follows that, or the loop would feed itself for ever.
            auto longInput = noiseInput (480000, 46, 0.25f);
            for (size_t i = 48000; i < longInput.size(); ++i)
                longInput[i] = 0.0f;
            Harness h (48000.0, 256);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::refresh, 250.0f);
            h.set (ParamID::formantLfoOn, 1.0f);
            h.set (ParamID::formantLfoRate, 2.0f);
            h.set (ParamID::formantLfoDepth, 50.0f);
            h.set (ParamID::feedback, 90.0f);
            const auto out = play (h, longInput, { keyDown (12000, 57) });
            const float level = largestSample (out, 36000, 48000), end = largestSample (out, 432000, 480000);
            check (end < 0.1f * level,
                   fmt ("Feedback 90%% with the Formant LFO moving and Refresh 250 ms: 8 s after the input stops the note is at %.1f dB",
                        20.0 * std::log10 ((double) (end / level) + 1.0e-12)));
        }

        {
            // What rings on stays on the note: each time round, the copy that comes back lands in step.
            Harness h (48000.0, 256);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::refresh, 5.0f);
            h.set (ParamID::feedback, 100.0f);
            const auto out = play (h, input, { keyDown (12000, 69) });
            const double pitch = centsBetween (pitchAt (out, 84000, 15), noteHz (69));
            check (std::abs (pitch) <= 8.0 && largestSample (out, 84000, 116768) > 0.001f,
                   fmt ("Feedback 100%%, Refresh 5 ms, A3: a second after the input stops the ringing note is %+.1f cents from 440 Hz", pitch));
        }
    }

    //==========================================================================
    // 0.3 stage 4: how notes are held, and how many play

    /** How loud a note is in a mix: the power of its first six harmonics, in dB, over 2^15 samples from the given position. */
    double noteLevelDb (const std::vector<float>& x, size_t position, int midiNote)
    {
        const auto part = slice (x, position, 32768);
        double power = 0.0;
        for (int k = 1; k <= 6; ++k)
            power += std::pow (10.0, lineLevelDb (part, 48000.0, noteHz (midiNote) * k) / 10.0);
        return 10.0 * std::log10 (std::max (1.0e-20, power));
    }

    double noteAgainst (const std::vector<float>& x, size_t position, int midiNote, int referenceNote)
    {
        return noteLevelDb (x, position, midiNote) - noteLevelDb (x, position, referenceNote);
    }

    void testPedalAndHoldModes()
    {
        section ("0.3 G06: the sustain pedal, Latch, On Grid and Full decide when a note ends");

        const auto input = noiseInput (144000, 51, 0.25f);
        auto frozenOnly = [] (Harness& h)
        {
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::dryWhenIdle, 0.0f);
        };

        {
            // The pedal holds a note past its key; lifting it lets the note go.
            auto run = [&] (bool pedalOn)
            {
                Harness h (48000.0, 256);
                frozenOnly (h);
                h.set (ParamID::sustainPedal, pedalOn ? 1.0f : 0.0f);
                return play (h, input, { keyDown (24000, 60), controller (30000, 64, 127), keyUp (36000, 60), controller (72000, 64, 0) });
            };
            const auto with = run (true), without = run (false);
            check (largestSample (with, 60000, 70000) > 0.01f && juce::exactlyEqual (largestSample (with, 86000, 96000), 0.0f)
                       && juce::exactlyEqual (largestSample (without, 50000, 60000), 0.0f),
                   fmt ("pedal down, key up: the note sounds on (peak %.2f) until the pedal lifts (then %g); with Sustain Pedal off it ends at the key (%g)",
                        (double) largestSample (with, 60000, 70000), (double) largestSample (with, 86000, 96000), (double) largestSample (without, 50000, 60000)));
        }

        {
            // Mono under the pedal: two taps, the second takes the voice over, and both keys being up
            // does not end it until the pedal lifts.
            Harness h (48000.0, 256);
            frozenOnly (h);
            h.set (ParamID::mono, 1.0f);
            const auto out = play (h, input, { keyDown (24000, 57), controller (26000, 64, 127), keyUp (28800, 57),
                                               keyDown (33600, 64), keyUp (38400, 64), controller (60000, 64, 0) });
            const double pitch = centsBetween (pitchAt (out, 48000), noteHz (64));
            check (std::abs (pitch) <= 5.0 && juce::exactlyEqual (largestSample (out, 75000, 85000), 0.0f),
                   fmt ("mono under the pedal: the second tap is what sounds (%+.1f cents from E3), and it ends when the pedal lifts (%g)",
                        pitch, (double) largestSample (out, 75000, 85000)));
        }

        {
            // Latch: a chord stays after its keys are up; the next chord replaces it.
            Harness h (48000.0, 256);
            frozenOnly (h);
            h.set (ParamID::holdMode, (float) (int) HoldMode::latch);
            const auto out = play (h, input, { keyDown (24000, 60), keyDown (24000, 64), keyDown (24000, 67),
                                               keyUp (28800, 60), keyUp (28800, 64), keyUp (28800, 67),
                                               keyDown (72000, 66), keyUp (74400, 66) });
            const double chordE = noteAgainst (out, 36000, 64, 60);
            const double afterC = noteAgainst (out, 100000, 60, 66);
            check (std::abs (chordE) < 10.0 && afterC < -30.0 && largestSample (out, 130000, 140000) > 0.01f,
                   fmt ("Latch: C-E-G rings on with every key up (E against C: %+.1f dB); tapping F# replaces the chord (C against F#: %+.1f dB) and F# rings on",
                        chordE, afterC));
        }

        {
            // Latch: a note pressed again while another key is down is taken out of the chord.
            Harness h (48000.0, 256);
            frozenOnly (h);
            h.set (ParamID::holdMode, (float) (int) HoldMode::latch);
            const auto out = play (h, input, { keyDown (24000, 60), keyDown (26400, 64), keyUp (28800, 64),
                                               keyDown (31200, 64), keyUp (33600, 64), keyUp (36000, 60) });
            const double e = noteAgainst (out, 60000, 64, 60);
            check (e < -30.0 && largestSample (out, 130000, 140000) > 0.01f,
                   fmt ("Latch: E pressed again under a held C leaves the chord (E against C: %+.1f dB) and C rings on", e));
        }

        {
            // Latch is switched off, or the song stops: the chord ends, and the dry signal is back untouched.
            auto stopBy = [&] (bool switchOff)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::release, 30.0f);
                h.set (ParamID::holdMode, (float) (int) HoldMode::latch);
                const auto first = play (h, input, { keyDown (24000, 60), keyUp (28800, 60) }, 0.0, 120.0);
                const float ringing = largestDifference (first, input, 120000, 140000);
                if (switchOff)
                    h.set (ParamID::holdMode, (float) (int) HoldMode::normal);
                const auto second = switchOff ? play (h, input, {}, 6.0, 120.0) : play (h, input, {});   // no play head: the song has stopped
                return std::pair<float, float> (ringing, largestDifference (second, input, 24000, second.size()));
            };
            const auto off = stopBy (true), stopped = stopBy (false);
            check (off.first > 0.02f && juce::exactlyEqual (off.second, 0.0f) && stopped.first > 0.02f && juce::exactlyEqual (stopped.second, 0.0f),
                   fmt ("a latched note ends when Latch is switched off (dry untouched: %g) and when the song stops (%g)",
                        (double) off.second, (double) stopped.second));
        }

        {
            // Full: every note lasts one Hold Time (1/4 = 24000 samples at 120 BPM), whatever the key does.
            auto run = [&] (bool tap)
            {
                Harness h (48000.0, 256);
                frozenOnly (h);
                h.set (ParamID::holdMode, (float) (int) HoldMode::full);
                std::vector<ScriptEvent> events { keyDown (24000, 60) };
                if (tap)
                    events.push_back (keyUp (26400, 60));
                return play (h, input, events, 0.0, 120.0);
            };
            const auto tapped = run (true), held = run (false);
            check (largestSample (tapped, 42000, 47000) > 0.01f && juce::exactlyEqual (largestSample (tapped, 60000, 70000), 0.0f)
                       && juce::exactlyEqual (largestDifference (tapped, held, 0, tapped.size()), 0.0f),
                   fmt ("Full, Hold Time 1/4: a 50 ms tap sounds for the whole beat (peak %.2f near its end) and no longer (%g); a key held for seconds plays the same note (max difference %g)",
                        (double) largestSample (tapped, 42000, 47000), (double) largestSample (tapped, 60000, 70000),
                        (double) largestDifference (tapped, held, 0, tapped.size())));
        }

        {
            // On Grid: a key that comes up between lines is released on the next line; one that comes
            // up on a line is released there; with the song stopped it is released at once.
            auto run = [&] (juce::int64 up, bool playing)
            {
                Harness h (48000.0, 256);
                frozenOnly (h);
                h.set (ParamID::holdMode, (float) (int) HoldMode::onGrid);
                return play (h, input, { keyDown (30000, 60), keyUp (up, 60) }, playing ? 0.0 : -1.0, 120.0);
            };
            const auto between = run (40000, true), onLine = run (48000, true), stopped = run (40000, false);
            const double carried = rmsDb (between, 44000, 47500) - rmsDb (between, 34000, 38000);
            check (std::abs (carried) <= 1.0 && juce::exactlyEqual (largestSample (between, 60000, 70000), 0.0f),
                   fmt ("On Grid, 1/4: a key let go 170 ms before the beat sounds on to the beat (%+.2f dB) and ends there (%g)",
                        carried, (double) largestSample (between, 60000, 70000)));
            check (juce::exactlyEqual (largestSample (onLine, 60000, 70000), 0.0f) && juce::exactlyEqual (largestSample (stopped, 52000, 60000), 0.0f),
                   fmt ("On Grid: a key let go on the beat ends there, not a beat later (%g); with the song stopped it ends at the key (%g)",
                        (double) largestSample (onLine, 60000, 70000), (double) largestSample (stopped, 52000, 60000)));
        }

        {
            GrainLockProcessor p;
            check (std::isinf (p.getTailLengthSeconds()), "the host is told the sound can go on for ever (a held note needs no input)");
        }
    }

    void testVoicesAndGlide()
    {
        section ("0.3 G08: the voice limit takes release tails first; glide only when legato, per octave, and in poly");

        const auto input = noiseInput (144000, 52, 0.25f);

        {
            // Voices 2, three keys held: the oldest key goes.
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::voices, 2.0f);
            const auto out = play (h, input, { keyDown (24000, 50), keyDown (28800, 61), keyDown (33600, 72) });
            const double oldest = noteAgainst (out, 60000, 50, 61), newest = noteAgainst (out, 60000, 72, 61);
            check (oldest < -30.0 && std::abs (newest) < 10.0,
                   fmt ("Voices 2, three keys held: the oldest (D2) has gone (%+.1f dB against C#3); the other two play (C4 against C#3: %+.1f dB)", oldest, newest));
        }

        {
            // Voices 2, one key held and one long release tail: a third note takes the tail, not the held key.
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::voices, 2.0f);
            h.set (ParamID::release, 4000.0f);
            const auto out = play (h, input, { keyDown (24000, 50), keyDown (28800, 61), keyUp (31200, 61), keyDown (36000, 72) });
            const double held = noteAgainst (out, 60000, 50, 72), tail = noteAgainst (out, 60000, 61, 72);
            check (std::abs (held) < 10.0 && tail < -30.0,
                   fmt ("Voices 2, a held D2 and the 4 s tail of a C#3: a new C4 takes the tail (C#3 against C4: %+.1f dB), the held key stays (D2 against C4: %+.1f dB)",
                        tail, held));
        }

        // Mono, Glide 200 ms, C2 then another key 250 ms later. The first note's Release is long, so its
        // voice is still there for the second key to take over whether or not the keys overlap.
        auto monoGlide = [&input] (bool legatoOnly, bool perOctave, int to, bool overlap)
        {
            Harness h (48000.0, 256);
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::dryWhenIdle, 0.0f);
            h.set (ParamID::mono, 1.0f);
            h.set (ParamID::glide, 200.0f);
            h.set (ParamID::release, 2000.0f);
            h.set (ParamID::glideLegato, legatoOnly ? 1.0f : 0.0f);
            h.set (ParamID::glideRate, perOctave ? 1.0f : 0.0f);
            std::vector<ScriptEvent> events { keyDown (24000, 48) };
            if (! overlap)
                events.push_back (keyUp (34000, 48));
            events.push_back (keyDown (36000, to));
            return play (h, input, events);
        };

        // The pitch around a moment, looked for only where it is expected (a frozen noise loop repeats
        // just as well every two periods, so an open search can land an octave low).
        auto centsAt = [] (const std::vector<float>& out, size_t position, double minHz, double maxHz, int note)
        {
            return centsBetween (shortPitchHz (out, position, 1024, minHz, maxHz), noteHz (note));
        };

        {
            // 50 ms after the key a gliding note is still well below it.
            const double separate = centsAt (monoGlide (true, false, 60, false), 38400, 200.0, 400.0, 60);
            const double legato = centsAt (monoGlide (true, false, 60, true), 38400, 125.0, 240.0, 60);
            const double always = centsAt (monoGlide (false, false, 60, false), 38400, 125.0, 240.0, 60);
            check (std::abs (separate) <= 30.0 && legato < -500.0 && legato > -1000.0 && always < -500.0 && always > -1000.0,
                   fmt ("Glide Legato, 50 ms after the key: a note played after a gap is on pitch (%+.0f cents), one played legato is still gliding (%+.0f cents); with it off both glide (%+.0f cents)",
                        separate, legato, always));
        }

        {
            // Per octave: two octaves take twice the Glide time. 300 ms after the key the plain glide
            // has arrived; the per-octave one is about half an octave short.
            const double plain = centsAt (monoGlide (false, false, 72, true), 50400, 400.0, 800.0, 72);
            const double perOctave = centsAt (monoGlide (false, true, 72, true), 50400, 300.0, 500.0, 72);
            check (std::abs (plain) <= 30.0 && perOctave < -350.0 && perOctave > -850.0,
                   fmt ("Glide 200 ms over two octaves, 300 ms after the key: %+.0f cents from the note as a fixed time, %+.0f cents as a time per octave",
                        plain, perOctave));
        }

        {
            // Poly Glide: a new note slides in from the last key played.
            auto run = [&input] (bool polyGlide)
            {
                Harness h (48000.0, 256);
                h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
                h.set (ParamID::dryWhenIdle, 0.0f);
                h.set (ParamID::glide, 200.0f);
                h.set (ParamID::release, 30.0f);
                h.set (ParamID::polyGlide, polyGlide ? 1.0f : 0.0f);
                return play (h, input, { keyDown (24000, 48), keyUp (30000, 48), keyDown (36000, 60) });
            };
            const double gliding = centsAt (run (true), 38400, 125.0, 240.0, 60);
            const double plain = centsAt (run (false), 38400, 200.0, 400.0, 60);
            check (std::abs (plain) <= 30.0 && gliding < -500.0 && gliding > -1000.0,
                   fmt ("Poly Glide, 50 ms after the second key: %+.0f cents from the note with it on, %+.0f cents with it off", gliding, plain));
        }
    }

    //==========================================================================
    // 0.3 stage 5: tone and stereo, on the frozen sound only

    double correlation (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
    {
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t i = from; i < to; ++i)
        {
            ab += (double) a[i] * b[i];
            aa += (double) a[i] * a[i];
            bb += (double) b[i] * b[i];
        }
        return ab / std::sqrt (std::max (1.0e-30, aa * bb));
    }

    void testTone()
    {
        section ("0.3 G26 / G24: Low Cut, High Cut, Tilt, Drive and Hollow shape the frozen sound; Diffuse smears it");

        auto frozenOnly = [] (Harness& h)
        {
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::dryWhenIdle, 0.0f);
        };

        {
            // The same held A2 over the same noise, one control at a time. The note's 1st harmonic is
            // 220 Hz and its 20th is 4400 Hz.
            const auto noise = noiseInput (144000, 61, 0.25f);
            auto run = [&] (const char* id, float value)
            {
                Harness h (48000.0, 256);
                frozenOnly (h);
                if (id != nullptr)
                    h.set (id, value);
                return slice (play (h, noise, { keyDown (24000, 57) }), 100000, 32768);
            };
            auto line = [] (const std::vector<float>& part, double hz) { return lineLevelDb (part, 48000.0, hz); };

            const auto plain = run (nullptr, 0.0f), lowCut = run (ParamID::lowCut, 2000.0f), highCut = run (ParamID::highCut, 500.0f), tilted = run (ParamID::tilt, 6.0f);
            const double lowCutLow = line (lowCut, 220.0) - line (plain, 220.0), lowCutHigh = line (lowCut, 4400.0) - line (plain, 4400.0);
            const double highCutLow = line (highCut, 220.0) - line (plain, 220.0), highCutHigh = line (highCut, 4400.0) - line (plain, 4400.0);
            const double tiltLow = line (tilted, 220.0) - line (plain, 220.0), tiltHigh = line (tilted, 4400.0) - line (plain, 4400.0);
            check (lowCutLow < -20.0 && std::abs (lowCutHigh) < 1.5,
                   fmt ("Low Cut 2 kHz: the note's 220 Hz is %+.1f dB, its 4.4 kHz %+.1f dB", lowCutLow, lowCutHigh));
            check (highCutHigh < -20.0 && std::abs (highCutLow) < 1.5,
                   fmt ("High Cut 500 Hz: the note's 4.4 kHz is %+.1f dB, its 220 Hz %+.1f dB", highCutHigh, highCutLow));
            check (tiltHigh - tiltLow > 7.0 && tiltHigh > 3.0 && tiltLow < -1.5,
                   fmt ("Tilt +6 dB: 220 Hz %+.1f dB, 4.4 kHz %+.1f dB", tiltLow, tiltHigh));
        }

        {
            // Drive on the worst case for a clipper: a loud, pure, high note (C6, 2093 Hz). Without
            // oversampling its 21st and 23rd harmonics fold back to 4047 Hz and 139 Hz, notes that are
            // not in the sound. Output Gain is down so the limiter stays out of it.
            const double f0 = noteHz (96);
            std::vector<float> sine (144000);
            for (size_t i = 0; i < sine.size(); ++i)
                sine[i] = 0.9f * (float) std::sin (juce::MathConstants<double>::twoPi * f0 * (double) i / 48000.0);

            auto run = [&] (float driveDb)
            {
                Harness h (48000.0, 256);
                frozenOnly (h);
                h.set (ParamID::outGain, -12.0f);
                h.set (ParamID::drive, driveDb);
                return slice (play (h, sine, { keyDown (24000, 96) }), 100000, 32768);
            };
            auto folds = [f0] (const std::vector<float>& part)
            {
                const double fundamental = lineLevelDb (part, 48000.0, f0);
                return std::max (lineLevelDb (part, 48000.0, 48000.0 - 21.0 * f0), lineLevelDb (part, 48000.0, 23.0 * f0 - 48000.0)) - fundamental;
            };
            const auto clean = run (0.0f), driven = run (24.0f), barely = run (0.1f);
            const double third = lineLevelDb (driven, 48000.0, 3.0 * f0) - lineLevelDb (driven, 48000.0, f0);
            const double step = rmsDb (barely, 0, barely.size()) - rmsDb (clean, 0, clean.size());
            Harness latency (48000.0, 256);
            check (folds (driven) < -60.0 && third > -20.0,
                   fmt ("Drive 24 dB on a loud C6 sine: folded-back lines %.1f dB under the note (%.1f dB with Drive off); its 3rd harmonic is at %+.1f dB",
                        folds (driven), folds (clean), third));
            check (std::abs (step) < 0.2 && latency.proc.getLatencySamples() == 0,
                   fmt ("Drive 0.1 dB is %+.3f dB from Drive off (no jump on leaving 0), and no latency is reported (%d samples)",
                        step, latency.proc.getLatencySamples()));
        }

        {
            // Hollow: the even harmonics of the played note go, the odd ones grow by 3 dB, with Pitch
            // Lock on or off. The source is the key's own note (A3, 440 Hz) with four harmonics.
            const auto source = harmonicInput (144000, 440.0, 4, 0.05);
            auto run = [&] (float hollow, bool lock)
            {
                Harness h (48000.0, 256);
                frozenOnly (h);
                h.set (ParamID::pitchLock, lock ? 1.0f : 0.0f);
                h.set (ParamID::hollow, hollow);
                return slice (play (h, source, { keyDown (24000, 69) }), 100000, 32768);
            };
            for (const bool lock : { false, true })
            {
                const auto plain = run (0.0f, lock), hollow = run (100.0f, lock);
                auto change = [&] (int k) { return lineLevelDb (hollow, 48000.0, 440.0 * k) - lineLevelDb (plain, 48000.0, 440.0 * k); };
                check (std::abs (change (1) - 3.0) < 1.0 && std::abs (change (3) - 3.0) < 1.0 && change (2) < -20.0 && change (4) < -20.0,
                       fmt ("Hollow 100%%, Pitch Lock %s: harmonics 1 to 4 change by %+.1f, %+.1f, %+.1f, %+.1f dB",
                            lock ? "on" : "off", change (1), change (2), change (3), change (4)));
            }
        }

        {
            // Diffuse: the level and the harmonics of a held note stay where they were; left and right
            // stop being the same; and when the note is over, the smear dies away and the dry signal
            // is untouched again.
            const auto source = harmonicInput (144000, 220.0, 8, 0.03);
            auto run = [&] (float diffuse, std::vector<float>& right)
            {
                Harness h (48000.0, 256);
                frozenOnly (h);
                h.set (ParamID::diffuse, diffuse);
                return play (h, source, { keyDown (24000, 57) }, -1.0, 120.0, &right);
            };
            std::vector<float> plainRight, smearedRight;
            const auto plain = run (0.0f, plainRight), smeared = run (100.0f, smearedRight);
            const double level = rmsDb (smeared, 96000, 140000) - rmsDb (plain, 96000, 140000);
            double worst = 0.0;
            for (int k = 1; k <= 8; ++k)
                worst = std::max (worst, std::abs (lineLevelDb (slice (smeared, 100000, 32768), 48000.0, 220.0 * k)
                                                   - lineLevelDb (slice (plain, 100000, 32768), 48000.0, 220.0 * k)));
            const double alike = correlation (smeared, smearedRight, 96000, 140000);
            check (std::abs (level) < 0.7 && worst < 3.0 && alike < 0.9 && correlation (plain, plainRight, 96000, 140000) > 0.999,
                   fmt ("Diffuse 100%% on a held note: level %+.2f dB, its first eight harmonics within %.1f dB, left and right %.2f alike (1.00 without)",
                        level, worst, alike));

            const auto noise = noiseInput (144000, 62, 0.25f);
            Harness h (48000.0, 256);
            h.set (ParamID::diffuse, 100.0f);
            h.set (ParamID::release, 30.0f);
            const auto out = play (h, noise, { keyDown (24000, 60), keyUp (48000, 60) });
            check (largestDifference (out, noise, 30000, 46000) > 0.02f && juce::exactlyEqual (largestDifference (out, noise, 100000, out.size()), 0.0f),
                   fmt ("Diffuse 100%%, Dry When Idle: a second after the note the output is the input again, exactly (max difference %g)",
                        (double) largestDifference (out, noise, 100000, out.size())));
        }
    }

    void testStereo()
    {
        section ("0.3 G15: Spread places notes, Width opens a note up without changing its mono sum, Drift wanders");

        auto frozenOnly = [] (Harness& h)
        {
            h.set (ParamID::captureMode, (float) (int) CaptureMode::hold);
            h.set (ParamID::dryWhenIdle, 0.0f);
        };
        const auto noise = noiseInput (144000, 63, 0.2f);

        {
            // Spread 100%, Alternate: the first note is hard left (3 dB up there, nothing on the right),
            // the second hard right.
            auto run = [&] (float spread, std::vector<float>& right, bool second)
            {
                Harness h (48000.0, 256);
                frozenOnly (h);
                h.set (ParamID::release, 30.0f);
                h.set (ParamID::spread, spread);
                std::vector<ScriptEvent> events { keyDown (24000, 57) };
                if (second)
                {
                    events.push_back (keyUp (48000, 57));
                    events.push_back (keyDown (60000, 57));
                }
                return play (h, noise, events, -1.0, 120.0, &right);
            };
            std::vector<float> centreRight, firstRight, secondRight;
            const auto centre = run (0.0f, centreRight, false), first = run (100.0f, firstRight, false), second = run (100.0f, secondRight, true);
            const double lift = rmsDb (first, 60000, 100000) - rmsDb (centre, 60000, 100000);
            check (std::abs (lift - 3.01) < 0.1 && juce::exactlyEqual (largestSample (firstRight, 0, firstRight.size()), 0.0f)
                       && juce::exactlyEqual (largestSample (second, 80000, 120000), 0.0f) && largestSample (secondRight, 80000, 120000) > 0.01f,
                   fmt ("Spread 100%%: the first note is hard left (%+.2f dB there, right channel peak %g); the next one is hard right (left channel peak %g)",
                        lift, (double) largestSample (firstRight, 0, firstRight.size()), (double) largestSample (second, 80000, 120000)));
        }

        {
            // Width on a rich note at the key's own pitch.
            const auto source = harmonicInput (144000, 220.0, 8, 0.03);
            auto run = [&] (float width, bool lock, std::vector<float>& right)
            {
                Harness h (48000.0, 256);
                frozenOnly (h);
                h.set (ParamID::pitchLock, lock ? 1.0f : 0.0f);
                h.set (ParamID::width, width);
                return play (h, source, { keyDown (24000, 57) }, -1.0, 120.0, &right);
            };
            for (const bool lock : { true, false })
            {
                std::vector<float> narrowRight, wideRight;
                const auto narrow = run (0.0f, lock, narrowRight), wide = run (100.0f, lock, wideRight);
                float monoDifference = 0.0f;
                for (size_t i = 0; i < wide.size(); ++i)
                    monoDifference = std::max (monoDifference, std::abs ((wide[i] + wideRight[i]) - (narrow[i] + narrowRight[i])));
                const double balance = rmsDb (wide, 60000, 140000) - rmsDb (wideRight, 60000, 140000);
                const double alike = correlation (wide, wideRight, 60000, 140000);
                check (monoDifference < 1.0e-4f && std::abs (balance) < 0.3 && alike < 0.9,
                       fmt ("Width 100%%, Pitch Lock %s: left + right is what it was (max difference %g), left and right are %+.2f dB apart and %.2f alike",
                            lock ? "on" : "off", (double) monoDifference, balance, alike));
            }
        }

        {
            // Drift: the note wanders, but stays the note.
            auto run = [&] (float drift, std::vector<float>& right)
            {
                Harness h (48000.0, 256);
                frozenOnly (h);
                h.set (ParamID::drift, drift);
                return play (h, noise, { keyDown (24000, 57) }, -1.0, 120.0, &right);
            };
            std::vector<float> stillRight, driftRight;
            const auto still = run (0.0f, stillRight), drifting = run (100.0f, driftRight);
            const double pitch = centsBetween (pitchAt (drifting, 100000), noteHz (57));
            const double sides = rmsDb (drifting, 60000, 140000) - rmsDb (driftRight, 60000, 140000);
            check (largestDifference (still, drifting, 60000, 140000) > 0.01f && std::abs (pitch) <= 15.0 && std::abs (sides) < 6.0
                       && ! juce::exactlyEqual (largestDifference (drifting, driftRight, 60000, 140000), 0.0f),
                   fmt ("Drift 100%%: the sound moves (%.2f from the still one), %+.1f cents from the note, left and right %+.1f dB apart",
                        (double) largestDifference (still, drifting, 60000, 140000), pitch, sides));
        }
    }

    void testCpu()
    {
        section ("CPU: the heaviest patch, as a multiple of real time (a figure to watch, not a promise about any one computer)");

        // 96 kHz, 64-sample blocks, eight low notes at Grain 16 in Live with a 5 ms Refresh, every
        // LFO per voice, and everything stage 3 to 5 added switched on.
        const double rate = 96000.0;
        const int blockSize = 64;
        auto seconds = [&] ()
        {
            Harness h (rate, blockSize);
            h.set (ParamID::grainCycles, 16.0f);
            h.set (ParamID::refresh, 5.0f);
            for (const auto& ids : ParamID::lfo)
            {
                h.set (ids.on, 1.0f);
                h.set (ids.trig, (float) (int) LfoTrig::voice);
                h.set (ids.depth, 60.0f);
            }
            h.set (ParamID::feedback, 50.0f);
            h.set (ParamID::threshold, -60.0f);
            h.set (ParamID::lowCut, 80.0f);
            h.set (ParamID::highCut, 9000.0f);
            h.set (ParamID::tilt, 3.0f);
            h.set (ParamID::drive, 12.0f);
            h.set (ParamID::hollow, 50.0f);
            h.set (ParamID::diffuse, 60.0f);
            h.set (ParamID::spread, 70.0f);
            h.set (ParamID::width, 60.0f);
            h.set (ParamID::drift, 50.0f);

            std::vector<MidiEvent> events;
            for (int k = 0; k < 8; ++k)
                events.push_back (noteOnAt (4800 + k * 64, 36 + (k * 5) % 13, 100));

            juce::Random rng (77);
            const double start = juce::Time::getMillisecondCounterHiRes();
            h.run ((juce::int64) (4.0 * rate), events, 0.25f, nullptr, nullptr, rng);
            return (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
        };

        double best = 1.0e9;
        for (int attempt = 0; attempt < 3; ++attempt)
            best = std::min (best, seconds());
        const double timesRealTime = 4.0 / std::max (1.0e-6, best);
        check (timesRealTime > 1.0, fmt ("eight voices, Grain 16, everything on, 96 kHz: %.1f x real time on this machine (4 s of audio in %.2f s)", timesRealTime, best));
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
    if (wants ("v03c"))
    {
        testWaitAndAtKey();
        testWaitingVoiceRules();
    }
    if (wants ("v03f"))
    {
        testTone();
        testStereo();
    }
    if (wants ("cpu"))
    {
        testCpu();
    }
    if (wants ("v03e"))
    {
        testPedalAndHoldModes();
        testVoicesAndGlide();
    }
    if (wants ("v03d"))
    {
        testInputTracker();
        testSnap();
        testThreshold();
        testGridAndSkip();
        testFeedback();
    }
    if (wants ("reference"))
    {
        testFingerprints();
        testBlockSizes();
    }

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
