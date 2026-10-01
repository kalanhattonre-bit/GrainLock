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
        tracker.prepare (sampleRate, ring.size());
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
        feedback.reset (sampleRate, 0.02);
        gateUp = (float) (1.0 / (0.005 * sampleRate));
        gateDown = (float) (1.0 / (0.050 * sampleRate));
        // The DC blocker sits at 1 Hz: any higher and its phase lead pulls the ringing part of a low note sharp.
        feedbackDcCoeff = (float) (1.0 - juce::MathConstants<double>::twoPi * 1.0 / sampleRate);
        feedbackLowCoeff = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 6000.0 / sampleRate));
        feedbackLag = (1.0f - feedbackLowCoeff) / feedbackLowCoeff;
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
        tracker.clear();
        sampleClock = 0;
        gateGain = 1.0f;
        gridHasSeen = false;
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
            feedback.setCurrentAndTargetValue (params.feedbackPercent / 100.0f);
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
            feedback.setTargetValue (params.feedbackPercent / 100.0f);
        }

        // Feedback at exactly 0 (and done fading) is not in the path at all: the memory holds the
        // input alone, as it always did.
        {
            const bool wanted = params.feedbackPercent > 0.0f || feedback.isSmoothing() || feedback.getCurrentValue() > 0.0f;
            if (feedbackActive && ! wanted)
                clearFeedback();
            feedbackActive = wanted;
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
            const double samplesPerBeat = 60.0 / (timing.bpm >= 1.0 ? timing.bpm : 120.0) * sampleRate;
            auto noteValue = [samplesPerBeat, this] (double beats, double ceilingSeconds)
            {
                double samples = beats * samplesPerBeat;
                if (! std::isfinite (samples))
                    return 0.0;
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
        // takes its own loop region off as well), so the whole note keeps one distance from the input.
        liveLagSamples = juce::jmax (0, offsetSamples - waitSamples);
        captureSource = CaptureSource { &ring, liveLagSamples };

        // What a note will and will not grab.
        const bool liveMode = params.captureMode == CaptureMode::live;
        snapSamples = (int) std::lround ((double) juce::jlimit (0.0f, 100.0f, params.snapMs) * sampleRate / 1000.0);
        maxWaitSamples = (int) std::lround ((double) juce::jlimit (0.0f, 2000.0f, params.maxWaitMs) * sampleRate / 1000.0);
        thresholdLevel = params.thresholdDb > thresholdOffDb ? juce::Decibels::decibelsToGain (params.thresholdDb) : 0.0f;
        stickyLive = liveMode && (thresholdLevel > 0.0f || params.skipHiss || params.skipChancePercent > 0.0f || refreshSynced);

        // The grid: Live re-grabs land on the song's own lines. It needs a note value and a running
        // song; without them a note keeps its own Refresh clock.
        {
            const double lineBeats = lfoSyncBeats (params.refreshSync);
            const bool wanted = liveMode && params.gridGrabs && lineBeats > 0.0 && timing.playing && timing.hasPpq;
            if (wanted)
            {
                const double bpm = timing.bpm >= 1.0 ? timing.bpm : 120.0;
                if (! gridActive || ! juce::exactlyEqual (lineBeats, gridBeats))
                    gridHasSeen = false;   // a new grid starts counting from where the song is now
                gridBeats = lineBeats;
                gridPpq = timing.ppq;
                gridPpqPerSample = bpm / 60.0 / sampleRate;
                gridHalfLine = juce::jmax (1, (int) (0.5 * lineBeats * 60.0 / bpm * sampleRate));
            }
            gridActive = wanted;
        }

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
        // A Live grain that can outlive its Refresh needs the same room.
        const bool hold = params.captureMode == CaptureMode::hold || stickyLive;
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
        ctx.tracker = &tracker;
        ctx.threshold = thresholdLevel;
        ctx.skipHiss = block.skipHiss;
        ctx.skipChance = block.skipChancePercent / 100.0f;
        ctx.gridActive = gridActive;
        ctx.stickyLive = stickyLive;
        ctx.feedbackOn = feedbackActive;
        ctx.feedbackLag = feedbackLag;
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
            if (gridActive)
                advanceGrid (ctx, i);

            const float inL = numInputChannels > 0 ? left[i] : 0.0f;
            const float inR = (numInputChannels > 1 && right != nullptr) ? right[i] : inL;
            pushInput (inL, inR);

            float wetL = 0.0f, wetR = 0.0f;
            FeedbackSend send;
            bool anyVoice = false;   // something is sounding: a voice still waiting to grab is not
            for (auto& voice : voices)
            {
                if (voice.isActive())
                {
                    anyVoice = anyVoice || ! voice.isWaiting();
                    voice.render (ctx, captureSource, wetL, wetR, send);
                }
            }

            // Gate: the frozen sound is heard only while the input itself is above the Threshold
            // (5 ms to open, 50 ms to close).
            {
                const bool open = ! (block.gate && thresholdLevel > 0.0f) || tracker.levelNow() >= thresholdLevel;
                gateGain = open ? juce::jmin (1.0f, gateGain + gateUp) : juce::jmax (0.0f, gateGain - gateDown);
                if (gateGain < 1.0f)
                {
                    wetL *= gateGain;
                    wetR *= gateGain;
                }
            }

            updateFeedback (send);

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
        // The detectors hear the input alone; only the memory also holds what Feedback sends back.
        tracker.push (left, right);
        if (feedbackActive)
            ring.push (left + feedbackLeft, right + feedbackRight);
        else
            ring.push (left, right);
        ++sampleClock;
    }

    void GrainEngine::clearFeedback() noexcept
    {
        feedbackLeft = feedbackRight = 0.0f;
        feedbackDcIn.fill (0.0f);
        feedbackDcOut.fill (0.0f);
        feedbackLow.fill (0.0f);
    }

    void GrainEngine::updateFeedback (const FeedbackSend& send) noexcept
    {
        const float amount = feedback.getNextValue();
        if (! feedbackActive)
            return;

        // A chord sends no more than one note does: the sum is divided by the voices' own levels.
        // The Gate closes the send as well.
        const float scale = gateGain / juce::jmax (1.0f, send.weight);
        const float in[2] = { send.left * scale, send.right * scale };
        float out[2] = { 0.0f, 0.0f };

        for (size_t ch = 0; ch < 2; ++ch)
        {
            // No DC, a little duller each time round, and never louder than -12 dB.
            const float blocked = in[ch] - feedbackDcIn[ch] + feedbackDcCoeff * feedbackDcOut[ch];
            feedbackDcIn[ch] = in[ch];
            feedbackDcOut[ch] = blocked;
            feedbackLow[ch] += feedbackLowCoeff * (blocked - feedbackLow[ch]);
            out[ch] = amount * 0.25f * std::tanh (4.0f * feedbackLow[ch]);
        }

        if (! (std::isfinite (out[0]) && std::isfinite (out[1])))
        {
            clearFeedback();   // a bad sample must not stay in the loop
            return;
        }

        feedbackLeft = out[0];
        feedbackRight = out[1];
    }

    void GrainEngine::advanceGrid (const VoiceContext& ctx, int sampleInBlock) noexcept
    {
        // The host gives the song position once per block; each sample's is worked out from it, so a
        // line lands on the same sample at any block size.
        const double ppq = gridPpq + (double) sampleInBlock * gridPpqPerSample;
        const auto index = (juce::int64) std::floor (ppq / gridBeats + 1.0e-9);

        if (! gridHasSeen)
        {
            gridHasSeen = true;
            gridLastSeen = index;
            gridPrevPpq = ppq;
            gridBeatsSinceFire = 0.5 * gridBeats;
            return;
        }

        gridBeatsSinceFire += gridPpqPerSample;

        // A cycle that is exactly one line long comes back to the same line number every time: the
        // jump back onto the line is the line.
        const bool jumpedBack = ppq < gridPrevPpq - 0.5 * gridBeats;
        const bool onLine = ppq - (double) index * gridBeats < 1.5 * gridPpqPerSample;
        gridPrevPpq = ppq;

        if (index == gridLastSeen && ! (jumpedBack && onLine))
            return;
        gridLastSeen = index;

        // A line counts once, however the position got here (a locate, a position reported a hair
        // early and then again): two lines are never less than half a line of song apart. Counted in
        // beats, so a tempo change inside a line does not lose the next one.
        if (gridBeatsSinceFire < 0.5 * gridBeats)
            return;
        gridBeatsSinceFire = 0.0;

        for (auto& voice : voices)
            voice.gridLine (ctx, captureSource, index, gridHalfLine);
    }

    //==============================================================================
    // Grabs that wait

    bool GrainEngine::planGrab (int slot, int note, const VoiceContext& ctx, PendingGrab& plan) noexcept
    {
        // The slice ends: at the key, plus the Wait, less the Offset; and for At Key one loop region
        // later still, so the region starts at the key instead of ending there. Snap and Threshold
        // then look at the input from that moment on (grabReady).
        const bool conditional = snapSamples > 0 || thresholdLevel > 0.0f;
        juce::int64 delay = (juce::int64) waitSamples - (juce::int64) offsetSamples;

        plan = PendingGrab {};
        plan.centre = sampleClock + delay;
        if (block.grabAtKey || conditional || delay > 0)   // every key that waits (Snap or Threshold may be switched on meanwhile)
            plan.region = (int) std::ceil (voices[(size_t) slot].plannedRegion (ctx, note));
        if (block.grabAtKey)
        {
            delay += (juce::int64) plan.region;
            plan.atKeyPart = plan.region;
        }

        plan.active = true;
        plan.placed = block.grabAtKey;
        plan.planEnd = sampleClock + delay;
        plan.dueTime = sampleClock + juce::jmax ((juce::int64) 0, delay);
        plan.giveUp = plan.dueTime + (juce::int64) maxWaitSamples;
        return delay <= 0 && ! conditional;
    }

    int GrainEngine::endDelayFor (juce::int64 sliceEnd) const noexcept
    {
        return (int) juce::jlimit ((juce::int64) 0, (juce::int64) (ring.size() - 16), sampleClock - sliceEnd);
    }

    bool GrainEngine::grabReady (PendingGrab& plan, bool keyDown, int& endDelay, bool& placed, bool& drop) noexcept
    {
        drop = false;
        if (sampleClock < plan.dueTime)
            return false;

        // Snap: the hit nearest the planned moment becomes the START of the loop. One that has
        // already happened is taken at once; otherwise the note waits out Snap's reach for one.
        if (snapSamples > 0 && ! plan.unsnapped)
        {
            if (! plan.anchored)
            {
                juce::int64 onset = 0;
                if (tracker.nearestOnset (plan.centre, (juce::int64) snapSamples, onset))
                {
                    plan.anchored = true;
                    plan.planEnd = onset + (juce::int64) plan.region;
                }
                else if (sampleClock < plan.centre + (juce::int64) snapSamples)
                    return false;
                else
                    plan.unsnapped = true;   // no hit: carry on as without Snap
            }

            if (plan.anchored)
            {
                if (sampleClock < plan.planEnd)
                    return false;   // the loop's worth of audio after the hit is still arriving

                endDelay = endDelayFor (plan.planEnd);
                placed = true;
                return true;
            }
        }

        // Threshold: the slice that would be grabbed now must be above it all the way through. It is
        // looked at every 32 samples; Max Wait later the note grabs whatever is there, so a key
        // never silently does nothing - unless it has already come up, and then it is forgotten.
        if (thresholdLevel > 0.0f)
        {
            const int lag = (int) juce::jlimit ((juce::int64) 0, (juce::int64) (ring.size() - 16), plan.dueTime - plan.planEnd);
            // The last chance always looks, whatever sample it falls on. A dip that failed the last
            // look is remembered: while it is still inside the slice the answer cannot change, so the
            // slice is not walked again.
            bool passes = false;
            if (sampleClock >= plan.giveUp || ((sampleClock - plan.dueTime) & 31) == 0)
            {
                const bool stillBlocked = plan.dipHop >= 0 && juce::exactlyEqual (plan.dipThreshold, thresholdLevel)
                                          && tracker.firstHopOf (lag, plan.region) <= plan.dipHop;
                if (! stillBlocked)
                {
                    plan.dipHop = -1;
                    plan.dipThreshold = thresholdLevel;
                    passes = tracker.covered (lag, plan.region, thresholdLevel, true, &plan.dipHop);
                }
            }

            if (! passes)
            {
                if (sampleClock < plan.giveUp)
                    return false;

                if (! keyDown)
                {
                    drop = true;
                    return false;
                }
            }

            endDelay = lag;
            placed = plan.placed;
            return true;
        }

        endDelay = endDelayFor (plan.planEnd);
        placed = plan.placed;
        return true;
    }

    void GrainEngine::fireGrab (int slot, const VoiceContext& ctx, int endDelay, bool placed, int atKeyPart) noexcept
    {
        voices[(size_t) slot].beginSounding (ctx, CaptureSource { &ring, endDelay }, placed, atKeyPart);
        pendingGrabs[(size_t) slot].active = false;
        restartNoteLfos();
        lastStartedVoice = slot;
    }

    void GrainEngine::startArmedVoice (int slot, const VoiceContext& ctx) noexcept
    {
        PendingGrab plan;
        if (planGrab (slot, voices[(size_t) slot].getNote(), ctx, plan))
            fireGrab (slot, ctx, endDelayFor (plan.planEnd), plan.placed, plan.atKeyPart);
        else
            pendingGrabs[(size_t) slot] = plan;
    }

    void GrainEngine::fireMonoKey (const VoiceContext& ctx, int endDelay, bool placed) noexcept
    {
        const auto key = monoKey;
        monoKey.active = false;

        const int glideSamples = (int) ((double) block.glideMs * sampleRate / 1000.0);

        if (monoVoice >= 0 && voices[(size_t) monoVoice].isActive() && ! voices[(size_t) monoVoice].isStealing()
            && ! voices[(size_t) monoVoice].isWaiting())
        {
            auto& voice = voices[(size_t) monoVoice];
            voice.retargetPlaced (key.note, key.level, glideSamples, ! voice.isHeld(), endDelay, placed, key.grab.atKeyPart);
            restartNoteLfos();
        }
        else
        {
            // The note it was to follow has gone: start afresh.
            monoVoice = findFreeSlot();
            voices[(size_t) monoVoice].arm (key.note, key.level, ++voiceCounter);
            fireGrab (monoVoice, ctx, endDelay, placed, key.grab.atKeyPart);
        }

        // A tap whose key came up while it waited its turn: it plays for as long as the key was down.
        if (key.released)
            voices[(size_t) monoVoice].releaseAfter (key.heldFor);

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
            {
                plan.active = false;   // the voice was stolen or cleared while it waited
                continue;
            }

            int endDelay = 0;
            bool placed = false, drop = false;
            if (grabReady (plan, voices[(size_t) slot].isHeld(), endDelay, placed, drop))
            {
                fireGrab (slot, ctx, endDelay, placed, plan.atKeyPart);
            }
            else if (drop)
            {
                voices[(size_t) slot].kill();
                plan.active = false;
            }
        }

        // A waiting voice always has a grab planned. Should one ever be left without, it must not sit
        // in its slot for good.
        for (int slot = 0; slot < numVoiceSlots; ++slot)
            if (voices[(size_t) slot].isWaiting() && ! pendingGrabs[(size_t) slot].active)
                voices[(size_t) slot].kill();

        if (monoKey.active)
        {
            int endDelay = 0;
            bool placed = false, drop = false;
            if (grabReady (monoKey.grab, ! monoKey.released, endDelay, placed, drop))
                fireMonoKey (ctx, endDelay, placed);
            else if (drop)
                monoKey.active = false;
        }
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
                    voice.retargetPlaced (note, level, glideSamples, ! legato, endDelayFor (plan.planEnd), true, plan.atKeyPart);
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
                monoKey.pressedAt = sampleClock;
                monoKey.released = false;
                monoKey.heldFor = 0;
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

        // A key that comes up before its turn. With another key still down it is a move taken back,
        // and never happens. With none it is a tap: it still plays its length when its turn comes.
        if (monoKey.active && monoKey.note == note)
        {
            if (stackSize > 0)
            {
                // Another key is still down. With a held note sounding, the move is simply taken back
                // (the fall-back below puts the note on the newest key). With nothing held to fall
                // back from, that key takes the turn instead, or it would never sound.
                if (monoVoice >= 0 && voices[(size_t) monoVoice].isHeld())
                {
                    monoKey.active = false;
                }
                else
                {
                    monoKey.note = stackTop();
                    monoKey.pressedAt = sampleClock;
                    if (block.grabAtKey || snapSamples > 0 || thresholdLevel > 0.0f)
                    {
                        PendingGrab plan;   // its loop is a different length: planned again, from now
                        planGrab (juce::jmax (0, monoVoice), monoKey.note, lastContext, plan);
                        monoKey.grab = plan;
                    }
                }
            }
            else if (! monoKey.released)
            {
                monoKey.released = true;
                monoKey.heldFor = (int) juce::jlimit ((juce::int64) 0, (juce::int64) (ring.size()), sampleClock - monoKey.pressedAt);
            }
        }

        if (monoVoice < 0)
            return;

        auto& voice = voices[(size_t) monoVoice];
        if (voice.isWaiting())
        {
            if (voice.getNote() == note)
            {
                if (stackSize > 0)
                {
                    voice.renote (stackTop());   // a key is still down under it: that key gets the note
                    if (block.grabAtKey || snapSamples > 0 || thresholdLevel > 0.0f)
                    {
                        PendingGrab plan;        // its loop is a different length: planned again, from now
                        planGrab (monoVoice, stackTop(), lastContext, plan);
                        pendingGrabs[(size_t) monoVoice] = plan;
                    }
                }
                else
                {
                    voice.release();             // a first note released before it grabbed still plays its length
                }
            }
            return;
        }

        if (! voice.isHeld())
            return;

        if (stackSize == 0)
        {
            voice.release();
        }
        else if (! monoKey.active && voice.getNote() != stackTop())
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
        clearFeedback();
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
