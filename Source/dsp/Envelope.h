#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>

namespace grainlock
{
    /** Linear ADSR. Times can change at any moment without jumps: attack and decay just change
        speed, and each release keeps the slope it had when the key went up (release time is
        measured from the level at note-off). Sustain is read every sample, so it can be smoothed. */
    class Envelope
    {
    public:
        void prepare (double newSampleRate) noexcept
        {
            sampleRate = newSampleRate;
            reset();
        }

        void setTimes (float attackMs, float decayMs, float releaseMs) noexcept
        {
            attackStep = 1.0f / (float) std::max (1.0, (double) attackMs * 0.001 * sampleRate);   // 0 ms = one sample
            decaySamples = (float) std::max (1.0, (double) decayMs * 0.001 * sampleRate);
            releaseSamples = (float) std::max (1.0, (double) releaseMs * 0.001 * sampleRate);
        }

        /** Starts (or restarts) the attack from the current level, so a retrigger never clicks. */
        void noteOn() noexcept { stage = Stage::attack; }

        void noteOff() noexcept
        {
            if (stage == Stage::idle)
                return;

            releaseStep = level / releaseSamples;
            stage = releaseStep > 0.0f ? Stage::release : Stage::idle;
            if (stage == Stage::idle)
                level = 0.0f;
        }

        void reset() noexcept
        {
            stage = Stage::idle;
            level = 0.0f;
        }

        bool isActive() const noexcept { return stage != Stage::idle; }

        float next (float sustain) noexcept
        {
            switch (stage)
            {
                case Stage::attack:
                    level += attackStep;
                    if (level >= 1.0f)
                    {
                        level = 1.0f;
                        stage = Stage::decay;
                    }
                    break;

                case Stage::decay:
                    level -= (1.0f - sustain) / decaySamples;
                    if (level <= sustain)
                    {
                        level = sustain;
                        stage = Stage::sustain;
                    }
                    break;

                case Stage::sustain:
                    level = sustain;
                    break;

                case Stage::release:
                    level -= releaseStep;
                    if (level <= 0.0f)
                    {
                        level = 0.0f;
                        stage = Stage::idle;
                    }
                    break;

                case Stage::idle:
                    level = 0.0f;
                    break;
            }
            return level;
        }

    private:
        enum class Stage { idle, attack, decay, sustain, release };

        double sampleRate = 48000.0;
        Stage stage = Stage::idle;
        float level = 0.0f;
        float attackStep = 1.0f;
        float decaySamples = 1.0f;
        float releaseSamples = 1.0f;
        float releaseStep = 0.0f;
    };
}
