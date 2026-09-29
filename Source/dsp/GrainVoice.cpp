#include "GrainVoice.h"

#include <cmath>

namespace grainlock
{
    namespace
    {
        constexpr double halfPi = juce::MathConstants<double>::halfPi;
        constexpr double pi = juce::MathConstants<double>::pi;
        constexpr double maxSeam = 0.5;

        // Built at load time, so the audio thread never runs a function-local static initialiser.
        const std::array<float, maxCycles + 1> invSqrtTable = []
        {
            std::array<float, maxCycles + 1> t {};
            t[0] = 1.0f;
            for (int i = 1; i <= maxCycles; ++i)
                t[(size_t) i] = 1.0f / std::sqrt ((float) i);
            return t;
        }();

        float invSqrtCycles (int n) noexcept
        {
            return invSqrtTable[(size_t) juce::jlimit (0, maxCycles, n)];
        }
    }

    void GrainVoice::prepare (double newSampleRate, int grainCapacity)
    {
        sampleRate = newSampleRate;
        for (auto& g : grains)
            g.allocate (grainCapacity);

        envelope.prepare (sampleRate);
        level.reset (sampleRate, 0.005);
        kill();
    }

    double GrainVoice::frequencyFor (double semitones) const noexcept
    {
        const double f = 440.0 * std::exp2 ((semitones - 69.0) / 12.0);
        return std::isfinite (f) ? juce::jlimit (2.0, sampleRate * 0.25, f) : 440.0;
    }

    double GrainVoice::loopLengthSamples (const PlayState& state, double frequency) const noexcept
    {
        const double period = sampleRate / frequency;
        return state.lockOn ? period : period * (double) state.taps;
    }

    int GrainVoice::tapsThatFit (const GrainBuffer& grain, int cycles, bool lockOn, float formantRatio) const noexcept
    {
        const int wanted = juce::jlimit (minCycles, maxCycles, cycles);

        // Pitch Lock off: the loop length IS the pitch (f / cycles), so it is never cut short.
        // If the grain is too small, renderState shortens the read per period instead.
        if (! lockOn)
            return wanted;

        // Sized for the widest seam, so moving Smooth never changes how many taps fit.
        const double cycleLength = grain.capturePeriod() * (double) formantRatio;
        if (grain.isEmpty() || ! (cycleLength > 0.0))
            return 1;

        return juce::jlimit (1, wanted, (int) std::floor (grain.span() / cycleLength - maxSeam));
    }

    bool GrainVoice::grainHolds (const GrainBuffer& grain, int cycles, bool lockOn, float formantRatio) const noexcept
    {
        const double n = (double) juce::jlimit (minCycles, maxCycles, cycles);
        const double cycleLength = grain.capturePeriod() * (double) formantRatio;
        const double need = lockOn ? (n + maxSeam) * cycleLength : n * (1.0 + maxSeam) * cycleLength;
        return ! grain.isEmpty() && need <= grain.span();
    }

    void GrainVoice::capture (GrainBuffer& grain, double frequency, const VoiceContext& ctx,
                              const CaptureSource& source) noexcept
    {
        const double period = sampleRate / frequency;
        const double n = (double) juce::jlimit (minCycles, maxCycles, ctx.captureCyclesMax);
        const double r = (double) ctx.captureRatioMax;

        // Pitch Lock on needs (N + seam) periods; off needs N * (1 + seam).
        const double lockOnNeed = (n + maxSeam) * period * r;
        const double lockOffNeed = n * (1.0 + maxSeam) * period * r;
        const double span = ctx.captureBothLayouts ? juce::jmax (lockOnNeed, lockOffNeed)
                                                   : (ctx.pitchLock ? lockOnNeed : lockOffNeed);

        const int ringLimit = source.ring->size() - source.offsetSamples - 8;
        const int limit = juce::jmin (grain.maxSpan(), ringLimit);
        const int spanSamples = (int) std::ceil (juce::jlimit (0.0, (double) limit, span));

        grain.capture (*source.ring, source.offsetSamples, spanSamples, period);
        samplesSinceCapture = 0.0;
        lastCaptureOffset = source.offsetSamples;
    }

    void GrainVoice::start (int midiNote, float velocityLevel, juce::uint64 order,
                            const VoiceContext& ctx, const CaptureSource& source)
    {
        note = midiNote;
        active = true;
        held = true;
        stealing = false;
        pendingRecapture = false;
        startOrder = order;

        pitch.jumpTo ((double) midiNote);
        level.setCurrentAndTargetValue (velocityLevel);

        current = 0;
        const int cycles = juce::jlimit (minCycles, maxCycles, ctx.targetCycles);
        capture (grains[0], frequencyFor (pitch.value + (double) ctx.globalSemitones), ctx, source);
        states[0] = PlayState { 0, cycles, tapsThatFit (grains[0], cycles, ctx.pitchLock, ctx.formantRatio), ctx.pitchLock, 0.0 };
        states[1] = states[0];
        transition.active = false;

        envelope.reset();
        envelope.noteOn();
    }

    void GrainVoice::retarget (int midiNote, float velocityLevel, int glideSamples, bool retriggerEnvelope)
    {
        note = midiNote;
        held = true;
        stealing = false;

        pitch.moveTo ((double) midiNote, glideSamples);
        if (velocityLevel >= 0.0f)
            level.setTargetValue (velocityLevel);

        // Every note grabs its own slice; the swap crossfades as soon as the voice is free to.
        pendingRecapture = true;

        if (retriggerEnvelope)
            envelope.noteOn();
    }

    void GrainVoice::release() noexcept
    {
        if (! active || stealing)
            return;

        held = false;
        envelope.noteOff();
    }

    void GrainVoice::steal (int fadeSamples) noexcept
    {
        if (! active)
            return;

        stealing = true;
        held = false;
        stealLength = juce::jmax (1, fadeSamples);
        stealRemaining = stealLength;
    }

    void GrainVoice::kill() noexcept
    {
        active = false;
        held = false;
        stealing = false;
        pendingRecapture = false;
        transition.active = false;
        envelope.reset();
    }

    void GrainVoice::renderState (const PlayState& state, const VoiceContext& ctx, float& left, float& right) const noexcept
    {
        const auto& grain = grains[(size_t) state.grain];
        left = right = 0.0f;

        if (grain.isEmpty())
            return;

        const double available = grain.span();
        const double seam = juce::jlimit (0.0, maxSeam, (double) ctx.smooth);
        const int n = state.taps;

        // Source samples read per note period. If the formant asks for more than the grain holds,
        // the read length is capped smoothly (the tap count never changes mid-note). The cap uses the
        // widest seam, like the capture sizing, so moving Smooth never changes the timbre.
        const double fitLimit = state.lockOn ? available / ((double) n + maxSeam)
                                             : available / ((double) n * (1.0 + maxSeam));
        const double cycleLength = juce::jmin (grain.capturePeriod() * (double) ctx.formantRatio, fitLimit);

        // Equal-power crossfade across the seam, into the audio just before the loop start.
        float seamOut = 1.0f, seamIn = 0.0f;
        if (seam > 0.0 && state.theta >= 1.0 - seam)
        {
            const double u = (state.theta - (1.0 - seam)) / seam;
            seamOut = (float) std::cos (u * halfPi);
            seamIn = (float) std::sin (u * halfPi);
        }

        float a = 0.0f, b = 0.0f;

        if (state.lockOn)
        {
            // Tap k reads note period k back from the newest one; all taps share the loop phase.
            const double base = (state.theta - 1.0) * cycleLength;
            float sumL = 0.0f, sumR = 0.0f;

            grain.read (base, a, b);
            sumL += seamOut * a;
            sumR += seamOut * b;

            for (int k = 1; k < n; ++k)
            {
                grain.read (base - (double) k * cycleLength, a, b);
                sumL += a;
                sumR += b;
            }

            if (seamIn > 0.0f)
            {
                grain.read (base - (double) n * cycleLength, a, b);
                sumL += seamIn * a;
                sumR += seamIn * b;
            }

            const float norm = invSqrtCycles (n);
            left = sumL * norm;
            right = sumR * norm;
        }
        else
        {
            const double loop = (double) n * cycleLength;
            const double position = (state.theta - 1.0) * loop;

            grain.read (position, a, b);
            left = seamOut * a;
            right = seamOut * b;

            if (seamIn > 0.0f)
            {
                grain.read (position - loop, a, b);
                left += seamIn * a;
                right += seamIn * b;
            }
        }
    }

    bool GrainVoice::advance (PlayState& state, double frequency) const noexcept
    {
        const double increment = frequency / sampleRate / (state.lockOn ? 1.0 : (double) state.taps);
        state.theta += increment;

        if (! std::isfinite (state.theta))
        {
            state.theta = 0.0;
            return false;
        }

        if (state.theta >= 1.0)
        {
            state.theta -= std::floor (state.theta);
            return true;
        }
        return false;
    }

    float GrainVoice::correlationBetween (const PlayState& a, const PlayState& b) noexcept
    {
        // Two Pitch-Lock-on states over the same audio share min(a, b) of their taps, so they are
        // correlated by min/sqrt(a*b). Anything involving Pitch Lock off reads different samples.
        if (a.lockOn && b.lockOn)
            return (float) juce::jmin (a.taps, b.taps) / std::sqrt ((float) (a.taps * b.taps));
        if (! a.lockOn && ! b.lockOn && a.taps == b.taps)
            return 1.0f;
        return 0.0f;
    }

    void GrainVoice::startTransition (int lengthSamples, float correlation) noexcept
    {
        transition.active = true;
        transition.correlation = juce::jlimit (0.0f, 1.0f, correlation);
        transition.position = 0;
        transition.length = juce::jmax (1, lengthSamples);
    }

    void GrainVoice::beginRecapture (double soundingFrequency, const VoiceContext& ctx, const CaptureSource& source) noexcept
    {
        const int previous = current;
        const int next = 1 - current;

        PlayState fresh = states[(size_t) previous];
        fresh.grain = 1 - states[(size_t) previous].grain;
        fresh.cycles = juce::jlimit (minCycles, maxCycles, ctx.targetCycles);
        fresh.lockOn = ctx.pitchLock;

        // Sized for the note being glided TO, so a slice is always its own note's wavelength.
        auto& grain = grains[(size_t) fresh.grain];
        capture (grain, frequencyFor (pitch.target + (double) ctx.globalSemitones), ctx, source);
        fresh.taps = tapsThatFit (grain, fresh.cycles, fresh.lockOn, ctx.formantRatio);

        states[(size_t) next] = fresh;
        current = next;
        pendingRecapture = false;

        // Fresh audio is unrelated to the old grain: equal-power, over one loop (1.5..50 ms).
        const double loop = loopLengthSamples (fresh, soundingFrequency);
        startTransition ((int) juce::jlimit (0.0015 * sampleRate, 0.05 * sampleRate, loop), 0.0f);
    }

    void GrainVoice::beginReshape (const VoiceContext& ctx) noexcept
    {
        const int previous = current;
        const int next = 1 - current;
        const auto& before = states[(size_t) previous];

        PlayState reshaped = before;
        reshaped.cycles = juce::jlimit (minCycles, maxCycles, ctx.targetCycles);
        reshaped.lockOn = ctx.pitchLock;
        reshaped.taps = tapsThatFit (grains[(size_t) reshaped.grain], reshaped.cycles, reshaped.lockOn, ctx.formantRatio);

        const float correlation = correlationBetween (before, reshaped);
        states[(size_t) next] = reshaped;
        current = next;
        startTransition ((int) (0.01 * sampleRate), correlation);
    }

    bool GrainVoice::beginExtend (int cycles, bool lockOn, const VoiceContext& ctx, const CaptureSource& source) noexcept
    {
        // Hold asked for more than its grain holds (Grain or Formant turned up mid-note). While the
        // input ring still has the audio, re-take the SAME frozen moment with more history behind
        // it, so pitch and timbre stay right and the new material is what really came before.
        const int previous = current;
        const int next = 1 - current;
        const auto& before = states[(size_t) previous];
        const auto& oldGrain = grains[(size_t) before.grain];
        auto& grain = grains[(size_t) (1 - before.grain)];

        const double period = oldGrain.capturePeriod();
        const double ratio = juce::jlimit (0.25, 4.0, juce::jmax ((double) ctx.formantRatio * 1.12, (double) ctx.captureRatioMax));
        const double n = (double) juce::jlimit (minCycles, maxCycles, cycles);
        const double span = juce::jmax ((n + maxSeam) * period * ratio, n * (1.0 + maxSeam) * period * ratio);

        // The ring has long since overwritten the moment of a note held longer than it holds.
        if (samplesSinceCapture + (double) lastCaptureOffset >= (double) source.ring->size())
            return false;

        const int spanSamples = (int) std::ceil (juce::jmin (span, (double) grain.maxSpan()));
        const int endDelay = lastCaptureOffset + (int) samplesSinceCapture;

        if (spanSamples <= (int) oldGrain.span() + 1 || endDelay + spanSamples + 16 > source.ring->size())
            return false;

        grain.capture (*source.ring, endDelay, spanSamples, period);

        PlayState extended = before;
        extended.grain = 1 - before.grain;
        extended.cycles = juce::jlimit (minCycles, maxCycles, cycles);
        extended.lockOn = lockOn;
        extended.taps = tapsThatFit (grain, extended.cycles, lockOn, ctx.formantRatio);

        const float correlation = correlationBetween (before, extended);
        states[(size_t) next] = extended;
        current = next;
        startTransition ((int) (0.01 * sampleRate), correlation);
        return true;
    }

    void GrainVoice::render (const VoiceContext& ctx, const CaptureSource& source, float& outLeft, float& outRight) noexcept
    {
        if (! active)
            return;

        const double frequency = frequencyFor (pitch.next() + (double) ctx.globalSemitones);

        if (! transition.active)
        {
            const auto& now = states[(size_t) current];
            const int wanted = juce::jlimit (minCycles, maxCycles, ctx.targetCycles);

            const auto& grain = grains[(size_t) now.grain];

            if (pendingRecapture)
            {
                beginRecapture (frequency, ctx, source);
            }
            else if (now.cycles != wanted || now.lockOn != ctx.pitchLock)
            {
                // A shape the grain cannot hold: Live grabs a fresh, bigger grain; Hold extends the
                // frozen one backwards. Only when neither is possible does the shape squeeze in.
                const bool holds = grainHolds (grain, wanted, ctx.pitchLock, ctx.formantRatio);

                if (holds)
                    beginReshape (ctx);
                else if (ctx.live && ! grain.isFull())
                    beginRecapture (frequency, ctx, source);
                else if (ctx.live || ! beginExtend (wanted, ctx.pitchLock, ctx, source))
                    beginReshape (ctx);
            }
            else if (! ctx.live && ! grainHolds (grain, now.taps, now.lockOn, ctx.formantRatio))
            {
                // Formant turned up past what the Hold grain holds.
                beginExtend (now.cycles, now.lockOn, ctx, source);
            }
        }

        float left = 0.0f, right = 0.0f;
        renderState (states[(size_t) current], ctx, left, right);

        if (transition.active)
        {
            float oldLeft = 0.0f, oldRight = 0.0f;
            auto& fading = states[(size_t) (1 - current)];
            renderState (fading, ctx, oldLeft, oldRight);
            advance (fading, frequency);

            // sin/cos fade scaled so the summed power stays flat for the expected correlation:
            // equal-power when unrelated (rho 0), linear-like when identical (rho 1).
            const double u = (double) transition.position / (double) transition.length;
            const double k = 1.0 / std::sqrt (1.0 + (double) transition.correlation * std::sin (u * pi));
            const float gainIn = (float) (std::sin (u * halfPi) * k);
            const float gainOut = (float) (std::cos (u * halfPi) * k);

            left = gainIn * left + gainOut * oldLeft;
            right = gainIn * right + gainOut * oldRight;

            if (++transition.position >= transition.length)
                transition.active = false;
        }

        const bool wrapped = advance (states[(size_t) current], frequency);
        samplesSinceCapture += 1.0;

        // Live: grab fresh audio at a loop boundary once Refresh has elapsed and the loop has
        // repeated at least twice (a loop that refreshes every pass is just a delay, not a pitch).
        if (wrapped && ctx.live && ! transition.active && ! pendingRecapture)
        {
            const double loop = loopLengthSamples (states[(size_t) current], frequency);
            if (samplesSinceCapture >= juce::jmax (ctx.refreshSamples, 2.0 * loop))
                beginRecapture (frequency, ctx, source);
        }

        float gain = envelope.next (ctx.sustain) * level.getNextValue();

        if (stealing)
        {
            gain *= (float) stealRemaining / (float) stealLength;
            if (--stealRemaining <= 0)
                kill();
        }
        else if (! envelope.isActive())
        {
            kill();
        }

        outLeft += left * gain;
        outRight += right * gain;
    }

    int GrainVoice::renderLoopShape (const VoiceContext& ctx, float* dest, int numPoints) const noexcept
    {
        PlayState shape = states[(size_t) current];
        for (int i = 0; i < numPoints; ++i)
        {
            shape.theta = (double) i / (double) numPoints;
            float left = 0.0f, right = 0.0f;
            renderState (shape, ctx, left, right);
            dest[i] = 0.5f * (left + right);
        }
        return shape.lockOn ? 1 : shape.taps;
    }
}
