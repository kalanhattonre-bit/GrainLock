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
        check (aParams.size() == (int) std::size (ParamID::all),
               fmt ("%d parameters exposed (expected %d)", aParams.size(), (int) std::size (ParamID::all)));

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

            if (b % blocksPerChange == 0)
            {
                // Sweep the settings that change code paths: modes, shapes, targets, sync.
                h.set (ParamID::captureMode, (float) rng.nextInt (2));
                h.set (ParamID::pitchLock, (float) rng.nextInt (2));
                h.set (ParamID::mono, rng.nextInt (4) == 0 ? 1.0f : 0.0f);
                h.set (ParamID::grainCycles, (float) (1 + rng.nextInt (16)));
                h.set (ParamID::smooth, rng.nextFloat() * 50.0f);
                h.set (ParamID::offset, rng.nextFloat() * 500.0f);
                h.set (ParamID::refresh, 5.0f + rng.nextFloat() * 495.0f);
                h.set (ParamID::formant, rng.nextFloat() * 24.0f - 12.0f);
                h.set (ParamID::tune, (float) (rng.nextInt (49) - 24));
                h.set (ParamID::fine, rng.nextFloat() * 200.0f - 100.0f);
                h.set (ParamID::glide, rng.nextFloat() * 500.0f);
                h.set (ParamID::attack, rng.nextFloat() * 200.0f);
                h.set (ParamID::sustain, rng.nextFloat() * 100.0f);
                h.set (ParamID::release, 1.0f + rng.nextFloat() * 800.0f);
                for (const auto& ids : ParamID::lfo)
                {
                    h.set (ids.on, (float) rng.nextInt (2));
                    h.set (ids.rate, 0.1f + rng.nextFloat() * 20.0f);
                    h.set (ids.sync, (float) rng.nextInt (13));
                    h.set (ids.shape, (float) rng.nextInt (4));
                    h.set (ids.depth, rng.nextFloat() * 100.0f);
                }
                h.set (ParamID::mix, rng.nextFloat() * 100.0f);
                h.set (ParamID::dryWhenIdle, (float) rng.nextInt (2));
                h.set (ParamID::outGain, rng.nextFloat() * 24.0f - 12.0f);
                playHead.playing = rng.nextInt (5) != 0;
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
        testLfoSyncedSampleHold();
        testHostGarbage();
        testThreeLfosTogether();
        testLegacyLfoMigration();
    }

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
