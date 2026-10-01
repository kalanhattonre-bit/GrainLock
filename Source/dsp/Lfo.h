#pragma once

#include "../Parameters.h"

#include <cmath>

namespace grainlock
{
    /** The seed of LFO number index, the same in the engine and in every voice. Different per LFO, so
        S&H on two LFOs never steps in lockstep. */
    inline juce::uint64 lfoSeed (int index) noexcept
    {
        return 0x6a1f5eedull + 0x9e3779b9ull * (juce::uint64) (index + 1);
    }

    /** One low-frequency oscillator. Output is bipolar, -1..1.

        The engine runs three of these for every note together; a voice runs its own three when an
        LFO is set to restart per voice. Both use the same seed, so every key hears the same
        sample-and-hold sequence.

        The sample-and-hold value is a hash of the cycle number, so re-locking to the host at a
        block boundary can never draw a second value for the same cycle, and a restart replays the
        sequence from its first value. */
    class Lfo
    {
    public:
        void reset (juce::uint64 newSeed) noexcept
        {
            seed = newSeed;
            restart();
        }

        /** Back to the start of the first cycle (a note-on in the Note, Voice and Once modes). */
        void restart() noexcept
        {
            phase = 0.0;
            cycle = 0;
            finished = false;
            held = valueForCycle (0);
            nextHeld = valueForCycle (1);
        }

        /** The value at the current phase, without advancing. */
        float peek (LfoShape shape, double offset = 0.0) const noexcept { return valueAt (shape, offset); }

        /** Returns the value at the current phase, then advances by increment (cycles per sample).
            offset (0..1) shifts where in the cycle the shape is read; it does not move the
            sample-and-hold steps. once: stop at the end of the first cycle and hold that value. */
        float next (double increment, LfoShape shape, double offset = 0.0, bool once = false) noexcept
        {
            if (! std::isfinite (phase))
                phase = 0.0;

            const float value = valueAt (shape, offset);

            if (once && finished)
                return value;

            phase += std::isfinite (increment) ? increment : 0.0;
            if (phase >= 1.0)
            {
                if (once)
                {
                    phase = 1.0 - 1.0e-9;   // the last point of the cycle, held from here on
                    finished = true;
                }
                else
                {
                    const double whole = std::floor (phase);
                    phase -= whole;
                    cycle += (juce::int64) whole;
                    held = valueForCycle (cycle);
                    nextHeld = valueForCycle (cycle + 1);
                }
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
            finished = false;
            if (cycleIndex != cycle)
            {
                cycle = cycleIndex;
                held = valueForCycle (cycle);
                nextHeld = valueForCycle (cycle + 1);
            }
        }

    private:
        float valueAt (LfoShape shape, double offset) const noexcept
        {
            double p = phase;
            if (! juce::exactlyEqual (offset, 0.0))   // exactly the old read when there is no offset
            {
                p += offset;
                p -= std::floor (p);
            }

            switch (shape)
            {
                case LfoShape::sine:       return (float) std::sin (juce::MathConstants<double>::twoPi * p);
                case LfoShape::triangle:   return (float) (p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0));
                case LfoShape::square:     return p < 0.5 ? 1.0f : -1.0f;
                case LfoShape::sampleHold: return held;
                case LfoShape::saw:        return (float) (1.0 - 2.0 * p);   // falls; Invert makes it rise
                case LfoShape::random:
                {
                    // A smooth walk from this cycle's random value to the next one's.
                    const double t = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * juce::jlimit (0.0, 1.0, phase));
                    return held + (float) t * (nextHeld - held);
                }
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
        float held = 0.0f, nextHeld = 0.0f;
        bool finished = false;
    };
}
