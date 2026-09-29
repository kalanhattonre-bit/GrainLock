#pragma once

#include "../Parameters.h"

#include <cmath>

namespace grainlock
{
    /** The single global LFO. Output is bipolar, -1..1.

        The sample-and-hold value is a hash of the cycle number, so re-locking to the host
        at a block boundary can never draw a second value for the same cycle. */
    class Lfo
    {
    public:
        void reset (juce::uint64 newSeed) noexcept
        {
            seed = newSeed;
            phase = 0.0;
            cycle = 0;
            held = valueForCycle (cycle);
        }

        /** Returns the value at the current phase, then advances by increment (cycles per sample). */
        float next (double increment, LfoShape shape) noexcept
        {
            if (! std::isfinite (phase))
                phase = 0.0;

            const float value = valueAt (phase, shape);

            phase += std::isfinite (increment) ? increment : 0.0;
            if (phase >= 1.0)
            {
                const double whole = std::floor (phase);
                phase -= whole;
                cycle += (juce::int64) whole;
                held = valueForCycle (cycle);
            }
            return value;
        }

        /** Locks to the host's musical position. cycleIndex counts whole LFO cycles since the
            song start; newPhase is the position within that cycle. */
        void syncTo (double newPhase, juce::int64 cycleIndex) noexcept
        {
            if (! std::isfinite (newPhase))
                return;

            const double fraction = newPhase - std::floor (newPhase);

            // The host can report a position a hair before a boundary this LFO already crossed.
            if (cycleIndex == cycle - 1 && fraction > 0.995)
                return;

            phase = fraction;
            if (cycleIndex != cycle)
            {
                cycle = cycleIndex;
                held = valueForCycle (cycle);
            }
        }

    private:
        float valueAt (double p, LfoShape shape) const noexcept
        {
            switch (shape)
            {
                case LfoShape::sine:       return (float) std::sin (juce::MathConstants<double>::twoPi * p);
                case LfoShape::triangle:   return (float) (p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0));
                case LfoShape::square:     return p < 0.5 ? 1.0f : -1.0f;
                case LfoShape::sampleHold: return held;
            }
            return 0.0f;
        }

        /** splitmix64 of (seed, cycle), mapped to -1..1. */
        float valueForCycle (juce::int64 c) const noexcept
        {
            juce::uint64 z = seed + (juce::uint64) c * 0x9e3779b97f4a7c15ull;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            z ^= z >> 31;
            return (float) ((double) (z >> 11) * (1.0 / 9007199254740992.0)) * 2.0f - 1.0f;
        }

        juce::uint64 seed = 0;
        double phase = 0.0;
        juce::int64 cycle = 0;
        float held = 0.0f;
    };
}
