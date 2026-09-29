#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace grainlock
{
    /** What reached the limiter since the last reset. Lets tests see problems the limiter hides. */
    struct LimiterStats
    {
        int nonFiniteInputs = 0;
        float maxInputPeak = 0.0f;
    };

    /** Zero-latency stereo-linked peak limiter. Instant attack and a smooth release keep every
        output sample at or below the ceiling; below it the signal passes bit-for-bit untouched. */
    class SoftLimiter
    {
    public:
        static constexpr float ceiling = 0.8912509f;   // -1 dBFS

        void prepare (double sampleRate) noexcept
        {
            releaseCoeff = (float) std::exp (-1.0 / (0.08 * sampleRate));
            envelope = 0.0f;
        }

        void reset() noexcept { envelope = 0.0f; }

        void process (float& left, float& right) noexcept
        {
            if (! std::isfinite (left) || ! std::isfinite (right))
            {
                ++stats.nonFiniteInputs;
                left = right = 0.0f;
                envelope = 0.0f;
                return;
            }

            const float peak = std::max (std::abs (left), std::abs (right));
            stats.maxInputPeak = std::max (stats.maxInputPeak, peak);
            envelope = peak > envelope ? peak : releaseCoeff * envelope + (1.0f - releaseCoeff) * peak;

            if (envelope > ceiling)
            {
                const float gain = ceiling / envelope;
                left *= gain;
                right *= gain;
            }
        }

        const LimiterStats& getStats() const noexcept { return stats; }
        void resetStats() noexcept { stats = {}; }

    private:
        float releaseCoeff = 0.999f;
        float envelope = 0.0f;
        LimiterStats stats;
    };
}
