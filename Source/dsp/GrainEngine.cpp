#include "GrainEngine.h"

#include <cmath>

namespace grainlock
{
    void GrainEngine::prepare (double newSampleRate, int)
    {
        sampleRate = newSampleRate;

        // Input history: two seconds, plus the furthest a synced Offset can reach back. A grain may
        // hold up to 0.75 s (enough for 16 cycles of a low note).
        ring.prepare ((int) std::ceil ((2.0 + maxOffsetSeconds) * sampleRate) + 64);
        const int grainCapacity = (int) std::ceil (0.75 * sampleRate) + 16;
        for (auto& voice : voices)
            voice.prepare (sampleRate, grainCapacity);

        tuneSemis.reset (sampleRate, 0.02);
        bendSemis.reset (sampleRate, 0.03);
        formantSemis.reset (sampleRate, 0.03);
        smoothFraction.reset (sampleRate, 0.03);
        mix.reset (sampleRate, 0.02);
        for (int i = 0; i < numLfos; ++i)
        {
            lfoDepth[(size_t) i].reset (sampleRate, 0.02);
            lfoRate[(size_t) i].reset (sampleRate, 0.05);
        }
        sustain.reset (sampleRate, 0.02);
        outGain.reset (sampleRate, 0.02);
        bypassFade.reset (sampleRate, 0.02);
        trackAmount.reset (sampleRate, 0.03);
        for (auto* s : { &wheel, &pressure, &expression })
            s->reset (sampleRate, 0.025);   // takes the steps out of 7-bit controller values

        limiter.prepare (sampleRate);
        // Different seeds, so S&H on two LFOs never steps in lockstep.
        for (int i = 0; i < numLfos; ++i)
            lfos[(size_t) i].reset (lfoSeed (i));

        lfoSmoothCoeff = (float) (1.0 - std::exp (-1.0 / (0.002 * sampleRate)));
        activityUp = (float) (1.0 / (0.002 * sampleRate));
        activityDown = (float) (1.0 / (0.008 * sampleRate));
        limiterFadeStep = (float) (1.0 / (0.02 * sampleRate));
        stealFadeSamples = juce::jmax (1, (int) (0.005 * sampleRate));
        scopeInterval = juce::jmax (1, (int) (sampleRate / 30.0));

        lastContext = VoiceContext {};
        lastContext.sampleRate = sampleRate;
        captureSource = CaptureSource { &ring, 0 };

        prepared = true;
        firstBlock = true;
        reset();
    }

    void GrainEngine::reset()
    {
        killAll();
        ring.clear();
        sampleClock = 0;
        limiter.reset();

        // Controllers go back to rest here and on "reset controllers" only, never when notes are cleared.
        bendNorm = 0.0f;
        bendSemis.setCurrentAndTargetValue (0.0f);
        wheel.setCurrentAndTargetValue (0.0f);
        pressure.setCurrentAndTargetValue (0.0f);
        expression.setCurrentAndTargetValue (1.0f);
        vibratoPhase = 0.0;
        activity = 0.0f;
        limiterBlend = 0.0f;
        lfoSmoothed.fill (0.0f);
        lfoLastScaled.fill (0.0f);
        scopeCountdown = 0;
        lastStartedVoice = -1;
    }

    void GrainEngine::applyBlockParams (const EngineParams& params, const HostTiming& timing, int) noexcept
    {
        if (! firstBlock && params.mono != monoMode)
            releaseAll();   // switching mode lets sounding notes ring out instead of cutting them

        monoMode = params.mono;
        block = params;

        const float tune = (float) params.tuneSemitones + params.fineCents / 100.0f;
        const float gain = juce::Decibels::decibelsToGain (params.outGainDb);

        if (firstBlock)
        {
            tuneSemis.setCurrentAndTargetValue (tune);
            formantSemis.setCurrentAndTargetValue (params.formantSemitones);
            smoothFraction.setCurrentAndTargetValue (params.smoothPercent / 100.0f);
            mix.setCurrentAndTargetValue (params.mixPercent / 100.0f);
            for (int i = 0; i < numLfos; ++i)
            {
                const auto& l = params.lfos[(size_t) i];
                lfoDepth[(size_t) i].setCurrentAndTargetValue (l.on ? l.depthPercent / 100.0f : 0.0f);
                lfoRate[(size_t) i].setCurrentAndTargetValue (l.rateHz);
            }
            sustain.setCurrentAndTargetValue (params.sustainPercent / 100.0f);
            outGain.setCurrentAndTargetValue (gain);
            bypassFade.setCurrentAndTargetValue (params.bypass ? 1.0f : 0.0f);
            trackAmount.setCurrentAndTargetValue (params.formantTrack ? 1.0f : 0.0f);
            firstBlock = false;
        }
        else
        {
            tuneSemis.setTargetValue (tune);
            formantSemis.setTargetValue (params.formantSemitones);
            smoothFraction.setTargetValue (params.smoothPercent / 100.0f);
            mix.setTargetValue (params.mixPercent / 100.0f);
            for (int i = 0; i < numLfos; ++i)
            {
                const auto& l = params.lfos[(size_t) i];
                lfoDepth[(size_t) i].setTargetValue (l.on ? l.depthPercent / 100.0f : 0.0f);
                lfoRate[(size_t) i].setTargetValue (l.rateHz);
            }
            sustain.setTargetValue (params.sustainPercent / 100.0f);
            outGain.setTargetValue (gain);
            bypassFade.setTargetValue (params.bypass ? 1.0f : 0.0f);
            trackAmount.setTargetValue (params.formantTrack ? 1.0f : 0.0f);
        }

        updateBendTarget();   // a new Bend range reaches a wheel that is already held

        // Note envelope, tape stop and keyboard sources, as every voice reads them.
        auto perSample = [this] (float ms) { return ms > 0.0f ? juce::jmin (1.0f, (float) (1000.0 / ((double) ms * sampleRate))) : 1.0f; };
        mods.envAttackStep = perSample (params.envAttackMs);
        mods.envDecayStep = perSample (params.envDecayMs);
        mods.envPitch = params.envPitch;
        mods.envFormant = params.envFormant;
        mods.envGrain = (float) params.envGrain;
        mods.tapeStop = params.tapeStop;
        mods.tapeStep = -tapeStopFallSemitones * perSample (params.releaseMs);
        mods.vibratoDepthSemitones = params.vibDepthCents / 100.0f;
        mods.pressureCoeff = (float) (1.0 - std::exp (-1.0 / (0.025 * sampleRate)));
        mods.lfoSmoothCoeff = lfoSmoothCoeff;
        mods.anySource = false;
        vibratoInUse = false;
        for (size_t s = 0; s < (size_t) numModSources; ++s)
        {
            mods.dest[s] = params.sources[s].dest;
            mods.amount[s] = juce::jlimit (-1.0f, 1.0f, params.sources[s].amountPercent / 100.0f);
            mods.anySource = mods.anySource || mods.dest[s] != ModDest::off;
            vibratoInUse = vibratoInUse || mods.dest[s] == ModDest::vibrato;
        }

        offsetSamples = juce::jlimit (0, (int) (0.5 * sampleRate), (int) std::lround (params.offsetMs * sampleRate / 1000.0));
        refreshSamples = juce::jmax (1.0, (double) params.refreshMs * sampleRate / 1000.0);
        waitSamples = juce::jlimit (0, (int) (maxWaitSeconds * sampleRate), (int) std::lround (params.waitMs * sampleRate / 1000.0));
        refreshSynced = false;

        // Note values for Offset, Wait and Refresh. One that is too long for its control is halved
        // until it fits, so the grab stays on the grid instead of landing at an arbitrary time.
        {
            const double samplesPerBeat = 60.0 / (timing.bpm > 0.0 ? timing.bpm : 120.0) * sampleRate;
            auto noteValue = [samplesPerBeat, this] (double beats, double ceilingSeconds)
            {
                double samples = beats * samplesPerBeat;
                while (samples > ceilingSeconds * sampleRate)
                    samples *= 0.5;
                return samples;
            };

            if (const double beats = grabSyncBeats (params.offsetSync); beats > 0.0)
                offsetSamples = (int) std::lround (noteValue (beats, maxOffsetSeconds));
            if (const double beats = grabSyncBeats (params.waitSync); beats > 0.0)
                waitSamples = (int) std::lround (noteValue (beats, maxWaitSeconds));
            if (const double beats = lfoSyncBeats (params.refreshSync); beats > 0.0)
            {
                refreshSamples = juce::jmax (1.0, beats * samplesPerBeat);
                refreshSynced = true;
            }
        }

        // A Live re-grab reads at the Offset less the Wait the note has already served (an At Key note
        // reads the newest audio), so the whole note keeps one distance from the input.
        liveLagSamples = params.grabAtKey ? 0 : juce::jmax (0, offsetSamples - waitSamples);
        captureSource = CaptureSource { &ring, liveLagSamples };

        // Times can change every block without side effects: a releasing voice keeps its own slope.
        for (auto& voice : voices)
            voice.setEnvelopeTimes (params.attackMs, params.decayMs, params.releaseMs);

        // LFO: free-running in Hz, or at the host's tempo. Sync sets the SPEED in every trigger mode;
        // only a Free LFO is also locked to the song position. In Note, Voice and Once the cycle
        // starts at the note and is never pulled back to the bar line.
        for (int i = 0; i < numLfos; ++i)
        {
            const auto& l = params.lfos[(size_t) i];
            lfoOffset[(size_t) i] = (double) juce::jlimit (0.0f, 360.0f, l.phaseDegrees) / 360.0;
            if (lfoOffset[(size_t) i] >= 1.0)
                lfoOffset[(size_t) i] = 0.0;
            lfoFadeStep[(size_t) i] = perSample (l.fadeMs);

            const double beats = lfoSyncBeats (l.sync);
            lfoSynced[(size_t) i] = beats > 0.0;
            if (! lfoSynced[(size_t) i])
                continue;

            const double bpm = timing.bpm > 0.0 ? timing.bpm : 120.0;
            lfoIncrement[(size_t) i] = bpm / 60.0 / beats / sampleRate;

            if (timing.playing && timing.hasPpq && l.trig == LfoTrig::free)
            {
                const double cycles = timing.ppq / beats;
                const double whole = std::floor (cycles);
                lfos[(size_t) i].syncTo (cycles - whole, (juce::int64) whole);
            }
        }

        // Capture budget: what the knobs and LFO can reach before the grain is replaced, counting
        // wherever the smoothers still are. Live replaces its grain every Refresh, so a small margin
        // will do; Hold keeps it for the whole note, so it gets room to turn Formant and Grain up.
        const bool hold = params.captureMode == CaptureMode::hold;
        auto reachOf = [this, &params] (LfoTarget target)
        {
            const auto i = (size_t) target;
            const float wanted = params.lfos[i].on ? params.lfos[i].depthPercent / 100.0f : 0.0f;
            return juce::jmax (lfoDepth[i].getCurrentValue(), wanted);   // counts a fade still in progress
        };

        // What the note envelope and the keyboard sources can add on top (upward only: a negative
        // amount never asks for more than the knob does).
        float sourceFormantReach = 0.0f, sourceCyclesReach = 0.0f;
        for (const auto& source : params.sources)
        {
            const float up = juce::jlimit (0.0f, 1.0f, source.amountPercent / 100.0f);
            if (source.dest == ModDest::formant) sourceFormantReach += up * sourceFormantRangeSemitones;
            if (source.dest == ModDest::grain)   sourceCyclesReach += up * sourceCyclesRange;
        }

        const float formantNow = juce::jmax (formantSemis.getCurrentValue(), params.formantSemitones);
        const float formantReach = formantNow + (hold ? 6.0f : 1.0f) + lfoFormantRangeSemitones * reachOf (LfoTarget::formant)
                                   + juce::jmax (0.0f, params.envFormant) + sourceFormantReach;
        captureRatioMax = juce::jlimit (0.25f, 4.0f, std::exp2 (formantReach / 12.0f));

        const int cyclesReach = params.grainCycles + (int) std::ceil (lfoCyclesRange * reachOf (LfoTarget::grainCycles))
                                + juce::jmax (0, params.envGrain) + (int) std::ceil (sourceCyclesReach);
        captureCyclesMax = juce::jlimit (minCycles, maxCycles, hold ? juce::jmax (cyclesReach, 2 * params.grainCycles)
                                                                    : cyclesReach);
        captureBothLayouts = hold;
    }

    VoiceContext GrainEngine::nextContext() noexcept
    {
        VoiceContext ctx;
        ctx.sampleRate = sampleRate;

        // All three shared LFOs run every sample; an LFO that is off just has its depth faded to zero.
        // A shared LFO with no fade-in is added to the sums below, as in 0.2. One that each voice runs
        // itself (Voice, Once) or fades in per note (Fade) is left out of them and handed to the voices.
        for (size_t i = 0; i < (size_t) numLfos; ++i)
        {
            const auto& l = block.lfos[i];
            const double increment = lfoSynced[i] ? lfoIncrement[i] : (double) lfoRate[i].getNextValue() / sampleRate;
            const float depth = lfoDepth[i].getNextValue() * (l.invert ? -1.0f : 1.0f);
            const float scaled = lfos[i].next (increment, l.shape, lfoOffset[i]) * depth;
            lfoLastScaled[i] = scaled;
            lfoSmoothed[i] += lfoSmoothCoeff * (scaled - lfoSmoothed[i]);   // ~2 ms, takes the click off square and S&H
            if (! std::isfinite (lfoSmoothed[i]))
                lfoSmoothed[i] = 0.0f;

            auto& feed = ctx.lfo[i];
            feed.perVoice = l.trig == LfoTrig::voice || l.trig == LfoTrig::once;
            feed.once = l.trig == LfoTrig::once;
            feed.inVoice = feed.perVoice || lfoFadeStep[i] < 1.0f;
            feed.shape = l.shape;
            feed.increment = increment;
            feed.offset = lfoOffset[i];
            feed.depth = depth;
            feed.shared = i == (size_t) LfoTarget::grainCycles ? scaled : lfoSmoothed[i];
            feed.fadeStep = lfoFadeStep[i];
        }

        auto sharedPart = [&ctx, this] (LfoTarget target, bool smoothed)
        {
            const auto i = (size_t) target;
            return ctx.lfo[i].inVoice ? 0.0f : (smoothed ? lfoSmoothed[i] : lfoLastScaled[i]);
        };
        const float lfoPitch = sharedPart (LfoTarget::pitch, true) * lfoPitchRangeSemitones;
        const float lfoFormant = sharedPart (LfoTarget::formant, true) * lfoFormantRangeSemitones;
        const float lfoCycles = sharedPart (LfoTarget::grainCycles, false) * lfoCyclesRange;   // steps crossfade in the voice

        // Keyboard sources, and the one vibrato wave every voice shares.
        ctx.mods = mods;
        ctx.wheel = wheel.getNextValue();
        ctx.pressure = pressure.getNextValue();
        ctx.expression = expression.getNextValue();
        if (vibratoInUse)
        {
            vibratoPhase += (double) block.vibRateHz / sampleRate;
            vibratoPhase -= std::floor (vibratoPhase);
            ctx.vibrato = (float) std::sin (juce::MathConstants<double>::twoPi * vibratoPhase);
        }

        ctx.globalSemitones = tuneSemis.getNextValue() + bendSemis.getNextValue() + lfoPitch;
        ctx.formantSemitones = formantSemis.getNextValue() + lfoFormant;
        ctx.formantRatio = juce::jlimit (0.25f, 4.0f, std::exp2 (ctx.formantSemitones / 12.0f));
        ctx.cyclesBase = (float) block.grainCycles + lfoCycles;
        ctx.trackAmount = trackAmount.getNextValue();
        ctx.autoGain = block.autoGain;
        ctx.legacySeam = legacySeam;
        ctx.smooth = smoothFraction.getNextValue();
        ctx.sustain = sustain.getNextValue();
        ctx.targetCycles = juce::jlimit (minCycles, maxCycles, juce::roundToInt (ctx.cyclesBase));
        ctx.pitchLock = block.pitchLock;
        ctx.live = block.captureMode == CaptureMode::live;
        ctx.refreshSamples = refreshSamples;
        ctx.refreshSynced = refreshSynced;
        ctx.captureRatioMax = captureRatioMax;
        ctx.captureCyclesMax = captureCyclesMax;
        ctx.captureBothLayouts = captureBothLayouts;
        return ctx;
    }

    void GrainEngine::process (float* const* channels, int numChannels, int numInputChannels, int numSamples,
                               const juce::MidiBuffer& midi, const EngineParams& params, const HostTiming& timing) noexcept
    {
        if (! prepared || numChannels <= 0 || channels == nullptr)
            return;

        wasBypassed = false;
        applyBlockParams (params, timing, numSamples);

        float* left = channels[0];
        float* right = numChannels > 1 ? channels[1] : nullptr;

        auto midiIt = midi.cbegin();
        const auto midiEnd = midi.cend();

        for (int i = 0; i < numSamples; ++i)
        {
            const VoiceContext ctx = nextContext();
            lastContext = ctx;

            // Notes grab audio up to (not including) the sample they land on.
            while (midiIt != midiEnd && (*midiIt).samplePosition <= i)
            {
                const auto event = *midiIt;
                handleMidiEvent (event.data, event.numBytes, ctx);
                ++midiIt;
            }

            // Keys whose grab was put off (Wait, At Key) take it here, at the same point of the sample
            // a key landing now would, so a waited note is the note a later key would have played.
            servePendingGrabs (ctx);

            const float inL = numInputChannels > 0 ? left[i] : 0.0f;
            const float inR = (numInputChannels > 1 && right != nullptr) ? right[i] : inL;
            pushInput (inL, inR);

            float wetL = 0.0f, wetR = 0.0f;
            bool anyVoice = false;   // something is sounding: a voice still waiting to grab is not
            for (auto& voice : voices)
            {
                if (voice.isActive())
                {
                    anyVoice = anyVoice || ! voice.isWaiting();
                    voice.render (ctx, captureSource, wetL, wetR);
                }
            }

            // Dry When Idle: full dry while no key is down; Mix applies while keys are held.
            const float m = mix.getNextValue();
            if (anyVoiceEngaged())
                activity = juce::jmin (1.0f, activity + activityUp);
            else if (activity > 0.0f)
                activity = juce::jmax (0.0f, activity - activityDown);

            const float dryGain = block.dryWhenIdle ? 1.0f - m * activity : 1.0f - m;
            float outL = inL * dryGain + wetL * m;
            float outR = inR * dryGain + wetR * m;

            const float g = outGain.getNextValue();
            outL *= g;
            outR *= g;

            // The limiter always listens, but at rest (no voices, unity gain) it is faded out of the
            // path, so idle audio is exactly the input even above -1 dBFS. It cuts back in instantly.
            float limL = outL, limR = outR;
            limiter.process (limL, limR);

            const bool needLimiter = anyVoice || activity > 0.0f || ! juce::exactlyEqual (g, 1.0f) || outGain.isSmoothing();
            limiterBlend = needLimiter ? 1.0f : juce::jmax (0.0f, limiterBlend - limiterFadeStep);

            if (limiterBlend >= 1.0f || ! std::isfinite (outL) || ! std::isfinite (outR))
            {
                outL = limL;
                outR = limR;
            }
            else if (limiterBlend > 0.0f)
            {
                outL += (limL - outL) * limiterBlend;
                outR += (limR - outR) * limiterBlend;
            }

            // Host bypass: fade to the untouched input (everything above keeps running underneath).
            const float b = bypassFade.getNextValue();
            if (b >= 1.0f)
            {
                outL = inL;
                outR = inR;
            }
            else if (b > 0.0f)
            {
                outL += (inL - outL) * b;
                outR += (inR - outR) * b;
            }

            left[i] = outL;
            if (right != nullptr)
                right[i] = outR;

            if (--scopeCountdown <= 0)
            {
                scopeCountdown = scopeInterval;
                publishScope (ctx);
            }
        }

        // Anything stamped past the end of the block (hosts should not send these).
        for (; midiIt != midiEnd; ++midiIt)
        {
            const auto event = *midiIt;
            handleMidiEvent (event.data, event.numBytes, lastContext);
        }
    }

    void GrainEngine::processBypassed (const float* const* channels, int numChannels, int numInputChannels,
                                       int numSamples) noexcept
    {
        if (! prepared || numChannels <= 0 || channels == nullptr)
            return;

        if (! wasBypassed)
        {
            // Note-offs that arrive during a bypass never reach the voices, so end every note now.
            killAll();
            activity = 0.0f;
            limiterBlend = 0.0f;
            limiter.reset();
            wasBypassed = true;
            scopeCountdown = 0;
        }

        // Keep the display honest while bypassed: no notes, no grain.
        scopeCountdown -= numSamples;
        if (scopeCountdown <= 0)
        {
            scopeCountdown = scopeInterval;
            publishScope (lastContext);
        }

        const float* left = channels[0];
        const float* right = numChannels > 1 ? channels[1] : nullptr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = numInputChannels > 0 ? left[i] : 0.0f;
            const float inR = (numInputChannels > 1 && right != nullptr) ? right[i] : inL;
            pushInput (inL, inR);
        }
    }

    void GrainEngine::pushInput (float left, float right) noexcept
    {
        ring.push (left, right);
        ++sampleClock;
    }

    //==============================================================================
    // Grabs that wait

    bool GrainEngine::planGrab (int slot, int note, const VoiceContext& ctx, PendingGrab& plan) noexcept
    {
        // The slice ends: at the key, plus the Wait, less the Offset; and for At Key one loop region
        // later still, so the region starts at the key instead of ending there.
        juce::int64 delay = (juce::int64) waitSamples - (juce::int64) offsetSamples;
        if (block.grabAtKey)
            delay += (juce::int64) std::ceil (voices[(size_t) slot].plannedRegion (ctx, note));

        plan.active = true;
        plan.placed = block.grabAtKey;
        plan.planEnd = sampleClock + delay;
        plan.dueTime = sampleClock + juce::jmax ((juce::int64) 0, delay);
        return delay <= 0;
    }

    void GrainEngine::fireGrab (int slot, const VoiceContext& ctx, const PendingGrab& plan) noexcept
    {
        const int endDelay = (int) juce::jlimit ((juce::int64) 0, (juce::int64) (ring.size() - 16), sampleClock - plan.planEnd);
        voices[(size_t) slot].beginSounding (ctx, CaptureSource { &ring, endDelay }, plan.placed);
        pendingGrabs[(size_t) slot].active = false;
        restartNoteLfos();
        lastStartedVoice = slot;
    }

    void GrainEngine::startArmedVoice (int slot, const VoiceContext& ctx) noexcept
    {
        PendingGrab plan;
        if (planGrab (slot, voices[(size_t) slot].getNote(), ctx, plan))
            fireGrab (slot, ctx, plan);
        else
            pendingGrabs[(size_t) slot] = plan;
    }

    void GrainEngine::fireMonoKey (const VoiceContext& ctx) noexcept
    {
        const auto key = monoKey;
        monoKey.active = false;

        const int glideSamples = (int) ((double) block.glideMs * sampleRate / 1000.0);
        const int endDelay = (int) juce::jlimit ((juce::int64) 0, (juce::int64) (ring.size() - 16), sampleClock - key.grab.planEnd);

        if (monoVoice >= 0 && voices[(size_t) monoVoice].isActive() && ! voices[(size_t) monoVoice].isStealing()
            && ! voices[(size_t) monoVoice].isWaiting())
        {
            auto& voice = voices[(size_t) monoVoice];
            voice.retargetPlaced (key.note, key.level, glideSamples, ! voice.isHeld(), endDelay, key.grab.placed);
            restartNoteLfos();
        }
        else
        {
            // The note it was to follow has gone: start afresh.
            monoVoice = findFreeSlot();
            voices[(size_t) monoVoice].arm (key.note, key.level, ++voiceCounter);
            fireGrab (monoVoice, ctx, key.grab);
        }
        lastStartedVoice = monoVoice;
    }

    void GrainEngine::servePendingGrabs (const VoiceContext& ctx) noexcept
    {
        for (int slot = 0; slot < numVoiceSlots; ++slot)
        {
            auto& plan = pendingGrabs[(size_t) slot];
            if (! plan.active)
                continue;

            if (! voices[(size_t) slot].isWaiting())
                plan.active = false;   // the voice was stolen or cleared while it waited
            else if (sampleClock >= plan.dueTime)
                fireGrab (slot, ctx, plan);
        }

        // A waiting voice always has a grab planned. Should one ever be left without, it must not sit
        // in its slot for good.
        for (int slot = 0; slot < numVoiceSlots; ++slot)
            if (voices[(size_t) slot].isWaiting() && ! pendingGrabs[(size_t) slot].active)
                voices[(size_t) slot].kill();

        if (monoKey.active && sampleClock >= monoKey.grab.dueTime)
            fireMonoKey (ctx);
    }

    void GrainEngine::clearPendingGrabs() noexcept
    {
        for (auto& plan : pendingGrabs)
            plan.active = false;
        monoKey.active = false;
    }

    void GrainEngine::handleMidiEvent (const juce::uint8* data, int numBytes, const VoiceContext& ctx) noexcept
    {
        if (data == nullptr || numBytes < 2)
            return;

        const int status = data[0] & 0xf0;
        const int d1 = data[1] & 0x7f;

        if (status == 0xd0)   // channel pressure: the one two-byte message that matters here
        {
            pressure.setTargetValue ((float) d1 / 127.0f);
            return;
        }
        if (numBytes < 3)
            return;

        const int d2 = data[2] & 0x7f;

        if (status == 0xa0)   // poly pressure: that key's voices only
        {
            for (auto& voice : voices)
                if (voice.isActive() && voice.getNote() == d1)
                    voice.setPressure ((float) d2 / 127.0f);
        }
        else if (status == 0x90 && d2 > 0)
        {
            if (monoMode) monoNoteOn (d1, d2, ctx);
            else          noteOn (d1, d2, ctx);
        }
        else if (status == 0x80 || status == 0x90)
        {
            if (monoMode) monoNoteOff (d1, ctx);
            else          noteOff (d1, ctx);
        }
        else if (status == 0xe0)
        {
            const int value = d1 | (d2 << 7);
            bendNorm = (float) (value - 8192) / 8192.0f;
            updateBendTarget();
        }
        else if (status == 0xb0)
        {
            if (d1 == 1)        wheel.setTargetValue ((float) d2 / 127.0f);
            else if (d1 == 11)  expression.setTargetValue ((float) d2 / 127.0f);
            else if (d1 == 120) killAll();                          // all sound off
            else if (d1 == 123) releaseAll();                       // all notes off
            else if (d1 == 121)                                     // reset controllers
            {
                bendNorm = 0.0f;
                updateBendTarget();
                wheel.setTargetValue (0.0f);
                pressure.setTargetValue (0.0f);
                expression.setTargetValue (1.0f);
            }
        }
    }

    float GrainEngine::velocityLevel (int velocity) const noexcept
    {
        const float v = (float) juce::jlimit (1, 127, velocity) / 127.0f;
        const float sens = block.velSensPercent / 100.0f;
        return 1.0f - sens * (1.0f - v);
    }

    int GrainEngine::findFreeSlot() noexcept
    {
        for (int i = 0; i < numVoiceSlots; ++i)
            if (! voices[(size_t) i].isActive())
                return i;

        // Every slot busy (a burst of steals): cut the oldest voice outright.
        int oldest = 0;
        for (int i = 1; i < numVoiceSlots; ++i)
            if (voices[(size_t) i].getStartOrder() < voices[(size_t) oldest].getStartOrder())
                oldest = i;

        voices[(size_t) oldest].kill();
        return oldest;
    }

    void GrainEngine::noteOn (int note, int velocity, const VoiceContext& ctx) noexcept
    {
        // Re-striking a held key starts a fresh voice; the old one releases.
        for (auto& voice : voices)
            if (voice.isHeld() && voice.getNote() == note)
                voice.release();

        int sounding = 0;
        int oldest = -1;
        for (int i = 0; i < numVoiceSlots; ++i)
        {
            const auto& voice = voices[(size_t) i];
            if (voice.isActive() && ! voice.isStealing())
            {
                ++sounding;
                if (oldest < 0 || voice.getStartOrder() < voices[(size_t) oldest].getStartOrder())
                    oldest = i;
            }
        }

        if (sounding >= maxPolyphony && oldest >= 0)
            voices[(size_t) oldest].steal (stealFadeSamples);

        const int slot = findFreeSlot();
        voices[(size_t) slot].arm (note, velocityLevel (velocity), ++voiceCounter);
        startArmedVoice (slot, ctx);
    }

    void GrainEngine::noteOff (int note, const VoiceContext&) noexcept
    {
        for (auto& voice : voices)
            if (voice.isHeld() && voice.getNote() == note)
                voice.release();
    }

    void GrainEngine::monoNoteOn (int note, int velocity, const VoiceContext& ctx) noexcept
    {
        stackRemove (note);
        stackPush (note);

        const float level = velocityLevel (velocity);
        const int glideSamples = (int) ((double) block.glideMs * sampleRate / 1000.0);

        const bool usable = monoVoice >= 0 && voices[(size_t) monoVoice].isActive() && ! voices[(size_t) monoVoice].isStealing();

        if (usable && ! voices[(size_t) monoVoice].isWaiting())
        {
            // A note is sounding. If this key's grab is due now, move to it now (as 0.2 does); if it
            // lies ahead, the sounding note carries on unchanged and the move happens then.
            PendingGrab plan;
            if (planGrab (monoVoice, note, ctx, plan))
            {
                auto& voice = voices[(size_t) monoVoice];
                const bool legato = voice.isHeld();
                if (plan.placed)   // At Key with an Offset longer than the region: the slice ends where the key put it
                    voice.retargetPlaced (note, level, glideSamples, ! legato,
                                          (int) juce::jlimit ((juce::int64) 0, (juce::int64) (ring.size() - 16), sampleClock - plan.planEnd), true);
                else
                    voice.retarget (note, level, glideSamples, ! legato);
                monoKey.active = false;
                restartNoteLfos();   // in mono every key restarts a Note LFO, legato or not
            }
            else
            {
                monoKey.active = true;
                monoKey.note = note;
                monoKey.level = level;
                monoKey.grab = plan;
            }
        }
        else
        {
            // Nothing is sounding (or only a first note that has not grabbed yet, which this replaces).
            if (! usable)
                monoVoice = findFreeSlot();
            monoKey.active = false;
            voices[(size_t) monoVoice].arm (note, level, ++voiceCounter);
            startArmedVoice (monoVoice, ctx);
        }

        lastStartedVoice = monoVoice;
    }

    void GrainEngine::monoNoteOff (int note, const VoiceContext&) noexcept
    {
        stackRemove (note);

        // A key let go before its move took effect never happens.
        if (monoKey.active && monoKey.note == note)
            monoKey.active = false;

        if (monoVoice < 0)
            return;

        auto& voice = voices[(size_t) monoVoice];
        if (voice.isWaiting())
        {
            if (voice.getNote() == note)
                voice.release();   // a first note released before it grabbed still plays its length
            return;
        }

        if (! voice.isHeld())
            return;

        if (stackSize == 0)
        {
            voice.release();
        }
        else if (voice.getNote() == note && ! monoKey.active)
        {
            // Last-note priority: fall back to the most recent key still held. No key was pressed, so
            // this does not wait.
            const int glideSamples = (int) ((double) block.glideMs * sampleRate / 1000.0);
            voice.retarget (stackTop(), -1.0f, glideSamples, false);
        }
    }

    void GrainEngine::releaseAll() noexcept
    {
        // A note that has not sounded yet is dropped: "all notes off" must never be followed by a late note.
        for (auto& voice : voices)
        {
            if (voice.isWaiting())
                voice.kill();
            else
                voice.release();
        }
        clearPendingGrabs();
        stackSize = 0;
        monoVoice = -1;
    }

    void GrainEngine::killAll() noexcept
    {
        for (auto& voice : voices)
            voice.kill();
        clearPendingGrabs();
        stackSize = 0;
        monoVoice = -1;
    }

    bool GrainEngine::anyVoiceEngaged() const noexcept
    {
        for (const auto& voice : voices)
            if (voice.isEngaged())
                return true;
        return false;
    }

    void GrainEngine::restartNoteLfos() noexcept
    {
        for (size_t i = 0; i < (size_t) numLfos; ++i)
            if (block.lfos[i].trig == LfoTrig::note)
                lfos[i].restart();
    }

    void GrainEngine::updateBendTarget() noexcept
    {
        bendSemis.setTargetValue (bendNorm * (float) (bendNorm >= 0.0f ? block.bendUp : block.bendDown));
    }

    void GrainEngine::stackRemove (int note) noexcept
    {
        int write = 0;
        for (int read = 0; read < stackSize; ++read)
            if (noteStack[(size_t) read] != note)
                noteStack[(size_t) write++] = noteStack[(size_t) read];
        stackSize = write;
    }

    void GrainEngine::stackPush (int note) noexcept
    {
        if (stackSize < (int) noteStack.size())
            noteStack[(size_t) stackSize++] = note;
    }

    void GrainEngine::publishScope (const VoiceContext& ctx) noexcept
    {
        ScopeFrame frame;

        // Sounding voices, oldest first.
        std::array<int, numVoiceSlots> order {};
        int count = 0;
        for (int i = 0; i < numVoiceSlots; ++i)
        {
            const auto& voice = voices[(size_t) i];
            if (voice.isWaiting())
                frame.setWaiting (voice.getNote());
            else if (voice.isActive() && ! voice.isStealing())
                order[(size_t) count++] = i;
        }
        if (monoKey.active)
            frame.setWaiting (monoKey.note);
        frame.offsetMs = (float) (1000.0 * offsetSamples / sampleRate);
        frame.waitMs = (float) (1000.0 * waitSamples / sampleRate);

        for (int a = 1; a < count; ++a)
            for (int b = a; b > 0 && voices[(size_t) order[(size_t) b]].getStartOrder()
                                         < voices[(size_t) order[(size_t) (b - 1)]].getStartOrder(); --b)
                std::swap (order[(size_t) b], order[(size_t) (b - 1)]);

        frame.numNotes = juce::jmin (count, ScopeFrame::maxNotes);
        for (int k = 0; k < frame.numNotes; ++k)
            frame.notes[(size_t) k] = voices[(size_t) order[(size_t) k]].getNote();

        if (monoMode)
        {
            for (int s = 0; s < stackSize; ++s)
                frame.setHeld (noteStack[(size_t) s]);
        }
        else
        {
            for (const auto& voice : voices)
                if (voice.isHeld())
                    frame.setHeld (voice.getNote());
        }

        int focus = -1;
        if (lastStartedVoice >= 0 && voices[(size_t) lastStartedVoice].isActive()
            && ! voices[(size_t) lastStartedVoice].isStealing() && ! voices[(size_t) lastStartedVoice].isWaiting())
            focus = lastStartedVoice;
        else if (count > 0)
            focus = order[(size_t) (count - 1)];

        if (focus >= 0)
        {
            frame.loopCycles = voices[(size_t) focus].renderLoopShape (ctx, frame.wave.data(), ScopeFrame::numPoints);
            frame.focusNote = voices[(size_t) focus].getNote();
            frame.hasWave = true;
        }

        frame.seamFraction = ctx.smooth;
        for (size_t i = 0; i < (size_t) numLfos; ++i)
        {
            frame.lfoValues[i] = lfoLastScaled[i];
            frame.lfoActive[i] = block.lfos[i].on && block.lfos[i].depthPercent > 0.0f;
        }
        frame.live = ctx.live;

        scopeFifo.push (frame);
    }
}
