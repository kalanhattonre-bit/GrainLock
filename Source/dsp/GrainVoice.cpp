#include "GrainVoice.h"

#include <cmath>

namespace grainlock
{
    namespace
    {
        constexpr double halfPi = juce::MathConstants<double>::halfPi;
        constexpr double pi = juce::MathConstants<double>::pi;
        constexpr double maxSeam = 0.5;
        constexpr double rootHz = 261.6255653005986;   // C3 (MIDI 60): where Formant Track leaves the formant alone
        constexpr int seamPoints = 64;
        constexpr int gainPointsFirst = 128, gainPointsRegrab = 48;

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
        gainSlew = (float) (1.0 - std::exp (-1.0 / (0.010 * sampleRate)));
        for (int i = 0; i < numLfos; ++i)
            ownLfos[(size_t) i].reset (lfoSeed (i));
        kill();
    }

    double GrainVoice::frequencyFor (double semitones) const noexcept
    {
        const double f = 440.0 * std::exp2 ((semitones - 69.0) / 12.0);
        return std::isfinite (f) ? juce::jlimit (2.0, sampleRate * 0.25, f) : 440.0;
    }

    void GrainVoice::restartNoteEnvelope() noexcept
    {
        noteEnv = noteEnvInstant ? 1.0f : 0.0f;
        noteEnvStage = noteEnvInstant ? EnvStage::decay : EnvStage::attack;
    }

    void GrainVoice::evaluate (const VoiceContext& ctx, bool advance) noexcept
    {
        const auto& mods = ctx.mods;
        const double glide = advance ? pitch.next() : pitch.value;

        // LFOs this voice adds itself: its own oscillator (Voice, Once), or the shared one faded in.
        std::array<float, numLfos> lfoValue { 0.0f, 0.0f, 0.0f };
        for (size_t i = 0; i < (size_t) numLfos; ++i)
        {
            const auto& feed = ctx.lfo[i];
            if (! feed.inVoice)
                continue;

            float value = feed.shared;
            if (feed.perVoice)
                value = feed.depth * (advance ? ownLfos[i].next (feed.increment, feed.shape, feed.offset, feed.once)
                                              : ownLfos[i].peek (feed.shape, feed.offset));

            if (advance)
                lfoFade[i] = juce::jmin (1.0f, lfoFade[i] + feed.fadeStep);
            value *= lfoFade[i];

            // The shared pitch and formant LFOs arrive smoothed; a voice's own are smoothed here
            // (about 2 ms, which takes the click off Square and S&H). Grain steps crossfade instead.
            if (feed.perVoice && i != (size_t) LfoTarget::grainCycles)
            {
                if (advance)
                    lfoSmoothed[i] += mods.lfoSmoothCoeff * (value - lfoSmoothed[i]);
                else
                    lfoSmoothed[i] = value;
                value = lfoSmoothed[i];
            }
            lfoValue[i] = std::isfinite (value) ? value : 0.0f;
        }

        // Note envelope.
        tapeStopEnabled = mods.tapeStop;
        noteEnvInstant = mods.envAttackStep >= 1.0f;
        if (advance)
        {
            if (noteEnvStage == EnvStage::attack)
            {
                noteEnv += mods.envAttackStep;
                if (noteEnv >= 1.0f)
                {
                    noteEnv = 1.0f;
                    noteEnvStage = EnvStage::decay;
                }
            }
            else if (noteEnvStage == EnvStage::decay)
            {
                noteEnv -= mods.envDecayStep;
                if (noteEnv <= 0.0f)
                {
                    noteEnv = 0.0f;
                    noteEnvStage = EnvStage::idle;
                }
            }

            if (tapeStopping)
                tapeSemitones = juce::jmax (-(double) tapeStopFallSemitones, tapeSemitones + (double) mods.tapeStep);
        }

        // Keyboard sources. d is how far a source is from rest: the wheel and aftertouch rest at 0,
        // the expression pedal rests at full.
        float vibratoDepth = 0.0f, sourceFormant = 0.0f, sourceCycles = 0.0f;
        levelMod = 1.0f;
        if (mods.anySource)
        {
            if (advance)
                polyPressureSmoothed += mods.pressureCoeff * (polyPressure - polyPressureSmoothed);
            else
                polyPressureSmoothed = polyPressure;

            const float distance[numModSources] = { ctx.wheel, juce::jmax (ctx.pressure, polyPressureSmoothed), 1.0f - ctx.expression };
            for (size_t s = 0; s < (size_t) numModSources; ++s)
            {
                const float d = juce::jlimit (0.0f, 1.0f, distance[s]);
                const float amount = mods.amount[s];
                switch (mods.dest[s])
                {
                    case ModDest::vibrato: vibratoDepth += amount * d; break;
                    case ModDest::formant: sourceFormant += amount * d * sourceFormantRangeSemitones; break;
                    case ModDest::grain:   sourceCycles += amount * d * sourceCyclesRange; break;
                    case ModDest::level:   levelMod *= amount >= 0.0f ? 1.0f - amount * d : 1.0f + amount * (1.0f - d); break;
                    case ModDest::off:     break;
                }
            }
        }

        // Base pitch: the note being played. Grabs are sized for it. (With nothing added here it is
        // exactly 0.2's sum.)
        const double shared = (double) ctx.globalSemitones + (double) lfoValue[(size_t) LfoTarget::pitch] * (double) lfoPitchRangeSemitones;
        basePitch = glide + shared;
        baseTargetPitch = pitch.target + shared;

        // Sounding pitch: only the loop rate follows it, so these move pitch and tone together.
        const double sounding = basePitch
                                + (double) (mods.envPitch * noteEnv)
                                + (double) (mods.vibratoDepthSemitones * vibratoDepth * ctx.vibrato)
                                + tapeSemitones;
        soundingFrequency = frequencyFor (sounding);

        // Formant and cycle count: the context's own numbers unless this voice adds something.
        const float formantExtra = lfoValue[(size_t) LfoTarget::formant] * lfoFormantRangeSemitones
                                   + mods.envFormant * noteEnv + sourceFormant;
        ratio = juce::exactlyEqual (formantExtra, 0.0f) ? ctx.formantRatio
                                                        : std::exp2 ((ctx.formantSemitones + formantExtra) / 12.0f);
        ratio = std::isfinite (ratio) ? juce::jlimit (0.25f, 4.0f, ratio) : 1.0f;

        const float cyclesExtra = lfoValue[(size_t) LfoTarget::grainCycles] * lfoCyclesRange
                                  + mods.envGrain * noteEnv + sourceCycles;
        cyclesNow = juce::exactlyEqual (cyclesExtra, 0.0f) ? ctx.targetCycles
                                                           : juce::roundToInt (ctx.cyclesBase + cyclesExtra);
        cyclesNow = juce::jlimit (minCycles, maxCycles, cyclesNow);

        // A grab is sized for the budget, and never for less than this voice wants right now.
        const float budget = std::isfinite (ctx.captureRatioMax) ? juce::jlimit (0.25f, 4.0f, ctx.captureRatioMax) : 1.0f;
        captureRatio = juce::jmax (budget, ratio);
        trackAmount = juce::jlimit (0.0f, 1.0f, ctx.trackAmount);
    }

    double GrainVoice::trackFactor (double capturePeriod) const noexcept
    {
        if (trackAmount <= 0.0f || ! (capturePeriod > 0.0))
            return 1.0;

        const double full = sampleRate / (capturePeriod * rootHz);   // the grab's pitch against C3
        return trackAmount >= 1.0f ? full : std::pow (full, (double) trackAmount);   // pow only while the switch fades
    }

    double GrainVoice::trackReach (double capturePeriod) const noexcept
    {
        if (trackAmount <= 0.0f || ! (capturePeriod > 0.0))
            return 1.0;

        const double full = sampleRate / (capturePeriod * rootHz);
        return trackAmount >= 1.0f ? full : juce::jmax (1.0, full);
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
        const double cycleLength = grain.capturePeriod() * (double) formantRatio * trackFactor (grain.capturePeriod());
        if (grain.isEmpty() || ! (cycleLength > 0.0))
            return 1;

        return juce::jlimit (1, wanted, (int) std::floor (grain.span() / cycleLength - maxSeam));
    }

    bool GrainVoice::grainHolds (const GrainBuffer& grain, int cycles, bool lockOn, float formantRatio) const noexcept
    {
        const double n = (double) juce::jlimit (minCycles, maxCycles, cycles);
        const double cycleLength = grain.capturePeriod() * (double) formantRatio * trackFactor (grain.capturePeriod());
        const double need = lockOn ? (n + maxSeam) * cycleLength : n * (1.0 + maxSeam) * cycleLength;
        return ! grain.isEmpty() && need <= grain.span();
    }

    int GrainVoice::nudgedEndDelay (const CaptureSource& source, const VoiceContext& ctx, double period) const noexcept
    {
        // With no seam fade to hide it, the loop jumps from the end of the slice to one loop length
        // before it. A little further back there is usually a point where those two places agree, and
        // ending the slice there takes most of the tick out. With a seam the fade already joins them
        // (and the place it joins is fixed by the note, not by where the slice ends), so the grab
        // stays exactly where the key put it.
        const auto& ring = *source.ring;
        const int base = source.offsetSamples;
        const double cycles = (double) cyclesNow;
        const double loopSource = cycles * period * (double) ratio * trackFactor (period);   // source samples back to the join
        const double loopPlayed = ctx.pitchLock ? period : cycles * period;                   // output samples per loop
        constexpr int window = 8;

        if (ctx.legacySeam || (double) ctx.smooth * loopPlayed >= (double) window)
            return base;

        const int loop = (int) std::lround (loopSource);
        const int reach = (int) std::ceil (juce::jmin (period, 0.010 * sampleRate));
        if (loop < 4 || reach < 2 || base + reach + loop + window + 8 >= ring.size())
            return base;

        // How badly the two places disagree, against how loud they are: 0 = identical, 2 = opposite.
        // (A plain difference would just prefer quiet audio and slide the grab off a hit.)
        auto mismatch = [&ring, loop] (int delay)
        {
            double difference = 0.0, energy = 1.0e-12;
            for (int j = 0; j < window; ++j)
            {
                const double a = ring.monoAt (delay + j), b = ring.monoAt (delay + j + loop);
                difference += (a - b) * (a - b);
                energy += a * a + b * b;
            }
            return difference / energy;
        };

        const int step = juce::jmax (1, reach / 32);
        int best = base;
        double bestMismatch = mismatch (base);
        for (int delay = base + step; delay <= base + reach; delay += step)
        {
            const double m = mismatch (delay);
            if (m < 0.7 * bestMismatch)   // only move for a clearly better join
            {
                bestMismatch = m;
                best = delay;
            }
        }

        if (best != base && step > 1)
        {
            const int coarse = best;
            for (int delay = juce::jmax (base, coarse - step + 1); delay < coarse + step && delay <= base + reach; ++delay)
            {
                const double m = mismatch (delay);
                if (m < bestMismatch)
                {
                    bestMismatch = m;
                    best = delay;
                }
            }
        }
        return best;
    }

    void GrainVoice::capture (GrainBuffer& grain, double frequency, const VoiceContext& ctx,
                              const CaptureSource& source, bool nudge) noexcept
    {
        const double period = sampleRate / frequency;
        const double n = (double) juce::jlimit (minCycles, maxCycles, juce::jmax (ctx.captureCyclesMax, cyclesNow));
        const double r = (double) captureRatio * trackReach (period);

        // Pitch Lock on needs (N + seam) periods; off needs N * (1 + seam).
        const double lockOnNeed = (n + maxSeam) * period * r;
        const double lockOffNeed = n * (1.0 + maxSeam) * period * r;
        const double span = ctx.captureBothLayouts ? juce::jmax (lockOnNeed, lockOffNeed)
                                                   : (ctx.pitchLock ? lockOnNeed : lockOffNeed);

        const int endDelay = nudge ? nudgedEndDelay (source, ctx, period) : source.offsetSamples;
        const int ringLimit = source.ring->size() - endDelay - 8;
        const int limit = juce::jmin (grain.maxSpan(), ringLimit);
        const int spanSamples = (int) std::ceil (juce::jlimit (0.0, (double) limit, span));

        grain.capture (*source.ring, endDelay, spanSamples, period);
        samplesSinceCapture = 0.0;
        lastCaptureOffset = endDelay;
    }

    double GrainVoice::cycleLengthOf (const PlayState& state) const noexcept
    {
        // Source samples read per note period. If the formant asks for more than the grain holds,
        // the read length is capped smoothly (the tap count never changes mid-note). The cap uses the
        // widest seam, like the capture sizing, so moving Smooth never changes the timbre.
        const auto& grain = grains[(size_t) state.grain];
        const double available = grain.span();
        const int n = state.taps;
        const double fitLimit = state.lockOn ? available / ((double) n + maxSeam)
                                             : available / ((double) n * (1.0 + maxSeam));
        return juce::jmin (grain.capturePeriod() * (double) ratio * trackFactor (grain.capturePeriod()), fitLimit);
    }

    void GrainVoice::measure (PlayState& state, const VoiceContext& ctx, int points, float blend) const noexcept
    {
        const auto& grain = grains[(size_t) state.grain];
        const double c = cycleLengthOf (state);
        const double seam = juce::jlimit (0.0, maxSeam, (double) ctx.smooth);
        state.measuredCycle = c;
        state.measuredSeam = ctx.smooth;
        state.measuredAuto = ctx.autoGain;

        float rho = 0.0f, gain = 1.0f;

        if (! grain.isEmpty() && c > 0.0)
        {
            const int n = state.taps;
            const double loop = state.lockOn ? c : (double) n * c;
            const double back = (double) n * c;   // how far before the fading-out audio the fading-in audio lies

            if (seam > 0.0 && ! ctx.legacySeam)
            {
                double ab = 0.0, aa = 0.0, bb = 0.0;
                for (int i = 0; i < seamPoints; ++i)
                {
                    const double p = -seam * loop * ((double) i + 0.5) / (double) seamPoints;
                    float al = 0.0f, ar = 0.0f, bl = 0.0f, br = 0.0f;
                    grain.read (p, al, ar);
                    grain.read (p - back, bl, br);
                    ab += (double) al * bl + (double) ar * br;
                    aa += (double) al * al + (double) ar * ar;
                    bb += (double) bl * bl + (double) br * br;
                }

                // Small values are measurement noise (unrelated audio reads about +/-0.12 here): ignore
                // them, so noise-like material keeps the plain equal-power fade.
                const double r = aa > 1.0e-20 && bb > 1.0e-20 ? ab / std::sqrt (aa * bb) : 0.0;
                const double strength = (std::abs (r) - 0.15) / 0.85;
                if (strength > 0.0)
                    rho = (float) juce::jlimit (-0.5, 1.0, r < 0.0 ? -strength : strength);
            }

            if (ctx.autoGain && state.lockOn && n > 1)
            {
                double summed = 0.0, single = 0.0;
                for (int i = 0; i < points; ++i)
                {
                    const double base = -c * ((double) i + 0.5) / (double) points;
                    float sumL = 0.0f, sumR = 0.0f;
                    for (int k = 0; k < n; ++k)
                    {
                        float a = 0.0f, b = 0.0f;
                        grain.read (base - (double) k * c, a, b);
                        sumL += a;
                        sumR += b;
                        single += (double) a * a + (double) b * b;
                    }
                    summed += (double) sumL * sumL + (double) sumR * sumR;
                }

                // The cycles are scaled by 1/sqrt(n): right when they are unrelated, too loud by up to
                // sqrt(n) when they are alike (a source at the note's own pitch).
                if (single > 1.0e-20)
                    gain = (float) (1.0 / std::sqrt (juce::jlimit (1.0, (double) n, summed / single)));
            }
        }

        const float b = juce::jlimit (0.0f, 1.0f, blend);
        state.seamRho += b * (rho - state.seamRho);
        state.gainTarget += b * (gain - state.gainTarget);
    }

    void GrainVoice::arm (int midiNote, float velocityLevel, juce::uint64 order) noexcept
    {
        note = midiNote;
        active = true;
        held = true;
        waiting = true;
        stealing = false;
        pendingRecapture = false;
        startOrder = order;

        pitch.jumpTo ((double) midiNote);
        level.setCurrentAndTargetValue (velocityLevel);

        polyPressure = polyPressureSmoothed = 0.0f;   // a reused slot never inherits the last note's pressure
        tapeStopping = false;
        tapeSemitones = 0.0;
        waitedSamples = gateSamples = gateRemaining = 0;
        placedDelay = -1;
        placedGrab = false;
        transition.active = false;
        envelope.reset();
    }

    double GrainVoice::plannedRegion (const VoiceContext& ctx, int midiNote) noexcept
    {
        // A voice that has not sounded yet is read as it will be at its first sample; a sounding one
        // (mono, a key that is waiting its turn) already has this sample's values.
        if (waiting)
        {
            restartNoteClocks (ctx);
            evaluate (ctx, false);
        }

        // The key's own period, with whatever tune, bend and pitch LFO are on the voice now. The region
        // is everything the loop reads: its cycles and the seam that fades in ahead of them.
        const double period = sampleRate / frequencyFor (basePitch - pitch.value + (double) midiNote);
        const double cycle = period * (double) ratio * trackFactor (period);
        const double seam = juce::jlimit (0.0, maxSeam, (double) ctx.smooth);
        const double n = (double) cyclesNow;
        const double region = ctx.pitchLock ? (n + seam) * cycle : n * (1.0 + seam) * cycle;
        return juce::jlimit (1.0, 0.8 * (double) grains[0].maxSpan(), region);   // never longer than a grain can use
    }

    void GrainVoice::restartNoteClocks (const VoiceContext& ctx) noexcept
    {
        for (size_t i = 0; i < (size_t) numLfos; ++i)
        {
            ownLfos[i].restart();
            lfoFade[i] = ctx.lfo[i].fadeStep >= 1.0f ? 1.0f : 0.0f;
            lfoSmoothed[i] = 0.0f;
        }
        noteEnvInstant = ctx.mods.envAttackStep >= 1.0f;
        restartNoteEnvelope();
    }

    void GrainVoice::beginSounding (const VoiceContext& ctx, const CaptureSource& source, bool placed) noexcept
    {
        waiting = false;
        // The key is already up: the note plays for as long as the key was down. (One more than the
        // count, so the release lands on the same sample it would for a key held that long.)
        gateRemaining = held ? 0 : juce::jmax (1, gateSamples) + 1;
        noteAge = 0.0;
        refreshDueAt = ctx.refreshSamples;

        // Every per-note clock starts here: the voice's own LFOs, their fade-in, the note envelope.
        restartNoteClocks (ctx);
        evaluate (ctx, false);

        current = 0;
        const int cycles = cyclesNow;
        capture (grains[0], frequencyFor (basePitch), ctx, source, ! placed);

        PlayState first;
        first.grain = 0;
        first.cycles = cycles;
        first.taps = tapsThatFit (grains[0], cycles, ctx.pitchLock, ratio);
        first.lockOn = ctx.pitchLock;
        measure (first, ctx, gainPointsFirst, 1.0f);
        first.gain = first.gainTarget;   // the attack hides it
        states[0] = first;
        states[1] = first;
        transition.active = false;
        sinceMeasure = 0;

        envelope.reset();
        envelope.noteOn();
    }

    void GrainVoice::start (int midiNote, float velocityLevel, juce::uint64 order,
                            const VoiceContext& ctx, const CaptureSource& source)
    {
        arm (midiNote, velocityLevel, order);
        beginSounding (ctx, source);
    }

    void GrainVoice::retarget (int midiNote, float velocityLevel, int glideSamples, bool retriggerEnvelope)
    {
        note = midiNote;
        held = true;
        stealing = false;

        // A new key takes back a note that was slowing to a stop.
        tapeStopping = false;
        tapeSemitones = 0.0;
        if (retriggerEnvelope)
            restartNoteEnvelope();

        pitch.moveTo ((double) midiNote, glideSamples);
        if (velocityLevel >= 0.0f)
            level.setTargetValue (velocityLevel);

        // Every note grabs its own slice; the swap crossfades as soon as the voice is free to.
        pendingRecapture = true;
        placedDelay = -1;
        placedGrab = false;

        if (retriggerEnvelope)
            envelope.noteOn();
    }

    void GrainVoice::retargetPlaced (int midiNote, float velocityLevel, int glideSamples, bool retriggerEnvelope,
                                     int grabDelay, bool placed)
    {
        retarget (midiNote, velocityLevel, glideSamples, retriggerEnvelope);
        placedDelay = juce::jmax (0, grabDelay);
        placedGrab = placed;
    }

    void GrainVoice::release() noexcept
    {
        // A second release would restart the Release time from the level the first has reached.
        if (! active || stealing || ! held)
            return;

        held = false;
        if (waiting)
        {
            gateSamples = juce::jmax (1, waitedSamples);   // it will sound this long once it has grabbed
            return;
        }

        envelope.noteOff();
        tapeStopping = tapeStopEnabled;   // slows to a halt over the Release time, and stops re-grabbing
    }

    void GrainVoice::steal (int fadeSamples) noexcept
    {
        if (! active)
            return;

        if (waiting)
        {
            kill();   // nothing is sounding yet, so there is nothing to fade
            return;
        }

        stealing = true;
        held = false;
        stealLength = juce::jmax (1, fadeSamples);
        stealRemaining = stealLength;
    }

    void GrainVoice::kill() noexcept
    {
        active = false;
        held = false;
        waiting = false;
        stealing = false;
        pendingRecapture = false;
        tapeStopping = false;
        tapeSemitones = 0.0;
        waitedSamples = gateSamples = gateRemaining = 0;
        placedDelay = -1;
        placedGrab = false;
        transition.active = false;
        envelope.reset();
    }

    void GrainVoice::renderState (const PlayState& state, const VoiceContext& ctx, float& left, float& right) const noexcept
    {
        const auto& grain = grains[(size_t) state.grain];
        left = right = 0.0f;

        if (grain.isEmpty())
            return;

        const double seam = juce::jlimit (0.0, maxSeam, (double) ctx.smooth);
        const int n = state.taps;
        const double cycleLength = cycleLengthOf (state);

        // Crossfade across the seam, into the audio just before the loop start. Equal-power when the
        // two sides are unrelated; when they are alike (seamRho towards 1) the pair is scaled so the
        // power stays flat instead of bulging by up to 3 dB in the middle of the fade.
        float seamOut = 1.0f, seamIn = 0.0f;
        if (seam > 0.0 && state.theta >= 1.0 - seam)
        {
            const double u = (state.theta - (1.0 - seam)) / seam;
            seamOut = (float) std::cos (u * halfPi);
            seamIn = (float) std::sin (u * halfPi);

            // A correlation measured for a different cycle length (Formant moving fast) says nothing
            // about this one: fall back to the plain fade until the next measurement.
            if (! juce::exactlyEqual (state.seamRho, 0.0f)
                && std::abs (cycleLength - state.measuredCycle) <= 0.03 * state.measuredCycle)
            {
                // sin(pi u) = 2 sin(pi u / 2) cos(pi u / 2)
                const float k = 1.0f / std::sqrt (1.0f + 2.0f * state.seamRho * seamOut * seamIn);
                seamOut *= k;
                seamIn *= k;
            }
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

            const float norm = invSqrtCycles (n) * state.gain;
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

    void GrainVoice::beginRecapture (double loopFrequency, const VoiceContext& ctx, const CaptureSource& source) noexcept
    {
        const int previous = current;
        const int next = 1 - current;

        PlayState fresh = states[(size_t) previous];
        fresh.grain = 1 - states[(size_t) previous].grain;
        fresh.cycles = cyclesNow;
        fresh.lockOn = ctx.pitchLock;

        // Where the new slice ends: a re-grab that waited ends where its key asked; otherwise at the
        // usual lag, but never further back than the grain it replaces.
        CaptureSource from = source;
        bool nudge = true;
        if (placedDelay >= 0)
        {
            from.offsetSamples = placedDelay;
            nudge = ! placedGrab;
            placedDelay = -1;
            placedGrab = false;
        }
        else
        {
            const double previousEnd = (double) lastCaptureOffset + samplesSinceCapture;
            if (previousEnd < (double) source.offsetSamples)
                from.offsetSamples = (int) previousEnd;
        }

        // Sized for the note being glided TO, so a slice is always its own note's wavelength.
        auto& grain = grains[(size_t) fresh.grain];
        capture (grain, frequencyFor (baseTargetPitch), ctx, from, nudge);
        fresh.taps = tapsThatFit (grain, fresh.cycles, fresh.lockOn, ratio);

        // A re-grab of the same shape moves towards its new measurements, so noise in them does not
        // become a level flutter at the Refresh rate; a different shape starts afresh.
        const auto& before = states[(size_t) previous];
        const bool sameShape = fresh.taps == before.taps && fresh.lockOn == before.lockOn;
        measure (fresh, ctx, gainPointsRegrab, sameShape ? 0.3f : 1.0f);
        fresh.gain = fresh.gainTarget;   // the crossfade into the new state hides it
        sinceMeasure = 0;

        states[(size_t) next] = fresh;
        current = next;
        pendingRecapture = false;

        // Fresh audio is unrelated to the old grain: equal-power, over one loop (1.5..50 ms).
        const double loop = loopLengthSamples (fresh, loopFrequency);
        startTransition ((int) juce::jlimit (0.0015 * sampleRate, 0.05 * sampleRate, loop), 0.0f);
    }

    void GrainVoice::beginReshape (const VoiceContext& ctx) noexcept
    {
        const int previous = current;
        const int next = 1 - current;
        const auto& before = states[(size_t) previous];

        PlayState reshaped = before;
        reshaped.cycles = cyclesNow;
        reshaped.lockOn = ctx.pitchLock;
        reshaped.taps = tapsThatFit (grains[(size_t) reshaped.grain], reshaped.cycles, reshaped.lockOn, ratio);
        measure (reshaped, ctx, gainPointsFirst, 1.0f);
        reshaped.gain = reshaped.gainTarget;
        sinceMeasure = 0;

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
        const double reach = juce::jlimit (0.25, 4.0, juce::jmax ((double) ratio * 1.12, (double) captureRatio)) * trackReach (period);
        const double n = (double) juce::jlimit (minCycles, maxCycles, cycles);
        const double span = juce::jmax ((n + maxSeam) * period * reach, n * (1.0 + maxSeam) * period * reach);

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
        extended.taps = tapsThatFit (grain, extended.cycles, lockOn, ratio);
        measure (extended, ctx, gainPointsFirst, 1.0f);
        extended.gain = extended.gainTarget;
        sinceMeasure = 0;

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

        if (waiting)
        {
            ++waitedSamples;
            return;
        }

        // A key that came up during the wait: the note has now played for as long as the key was down.
        // (Before anything else this sample, which is where a key coming up now would land.)
        if (gateRemaining > 0 && --gateRemaining == 0)
        {
            envelope.noteOff();
            tapeStopping = tapeStopEnabled;
        }

        evaluate (ctx, true);
        const double frequency = soundingFrequency;
        const bool live = ctx.live && ! tapeStopping;   // a note slowing to a stop keeps the audio it has
        noteAge += 1.0;

        if (pendingRecapture && placedDelay >= 0 && transition.active)
            ++placedDelay;   // the audio it asked for moves further back while the voice is busy

        if (! transition.active)
        {
            const auto& now = states[(size_t) current];
            const int wanted = cyclesNow;

            const auto& grain = grains[(size_t) now.grain];

            if (pendingRecapture)
            {
                beginRecapture (frequency, ctx, source);
            }
            else if (now.cycles != wanted || now.lockOn != ctx.pitchLock)
            {
                // A shape the grain cannot hold: Live grabs a fresh, bigger grain; Hold extends the
                // frozen one backwards. Only when neither is possible does the shape squeeze in.
                const bool holds = grainHolds (grain, wanted, ctx.pitchLock, ratio);

                if (holds)
                    beginReshape (ctx);
                else if (live && ! grain.isFull())
                    beginRecapture (frequency, ctx, source);
                else if (live || ! beginExtend (wanted, ctx.pitchLock, ctx, source))
                    beginReshape (ctx);
            }
            else if (! live && ! grainHolds (grain, now.taps, now.lockOn, ratio))
            {
                // Formant turned up past what the Hold grain holds.
                beginExtend (now.cycles, now.lockOn, ctx, source);
            }
        }

        float left = 0.0f, right = 0.0f;
        {
            auto& now = states[(size_t) current];
            now.gain += gainSlew * (now.gainTarget - now.gain);
            renderState (now, ctx, left, right);
        }

        if (transition.active)
        {
            float oldLeft = 0.0f, oldRight = 0.0f;
            auto& fading = states[(size_t) (1 - current)];
            fading.gain += gainSlew * (fading.gainTarget - fading.gain);
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
        ++sinceMeasure;

        // The measurements follow Formant, Smooth and the Auto Gain switch as they move: at a loop
        // boundary, at most every 10 ms, and only when one of them has changed.
        if (wrapped && ! transition.active && sinceMeasure >= (int) (0.01 * sampleRate))
        {
            auto& now = states[(size_t) current];
            const double c = cycleLengthOf (now);
            if (std::abs (c - now.measuredCycle) > 0.01 * now.measuredCycle
                || std::abs (ctx.smooth - now.measuredSeam) > 0.01f || now.measuredAuto != ctx.autoGain)
            {
                measure (now, ctx, gainPointsRegrab, 1.0f);   // the applied gain glides to it
                sinceMeasure = 0;
            }
        }

        // Live: grab fresh audio at a loop boundary once Refresh has elapsed and the loop has
        // repeated at least twice (a loop that refreshes every pass is just a delay, not a pitch).
        if (wrapped && live && ! transition.active && ! pendingRecapture)
        {
            const double loop = loopLengthSamples (states[(size_t) current], frequency);
            if (! ctx.refreshSynced)
            {
                if (samplesSinceCapture >= juce::jmax (ctx.refreshSamples, 2.0 * loop))
                    beginRecapture (frequency, ctx, source);
            }
            else if (noteAge >= refreshDueAt && samplesSinceCapture >= 2.0 * loop)
            {
                // A note value: the next grab falls due one interval after this one was DUE, so the
                // rhythm holds its tempo instead of drifting by a loop each time.
                beginRecapture (frequency, ctx, source);
                refreshDueAt += ctx.refreshSamples;
                if (refreshDueAt <= noteAge)
                    refreshDueAt = noteAge + ctx.refreshSamples;
            }
        }

        float gain = envelope.next (ctx.sustain) * level.getNextValue() * levelMod;

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
