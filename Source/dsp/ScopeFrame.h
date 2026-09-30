#pragma once

#include <juce_core/juce_core.h>

#include <array>

namespace grainlock
{
    /** One display snapshot, built on the audio thread about 30 times a second. */
    struct ScopeFrame
    {
        static constexpr int numPoints = 256;
        static constexpr int maxNotes = 12;

        std::array<float, numPoints> wave {};   // one loop of the focused voice
        std::array<int, maxNotes> notes {};     // sounding voices, oldest first
        int numNotes = 0;
        int focusNote = -1;                     // the voice drawn in wave, -1 if none
        int loopCycles = 1;                     // note periods spanned by the drawn loop
        float seamFraction = 0.0f;              // shaded crossfade region at the end of the loop
        std::array<float, 3> lfoValues {};      // each LFO's output scaled by its depth, -1..1 (pitch, formant, grain)
        std::array<bool, 3> lfoActive {};       // which LFOs are on
        bool hasWave = false;
        bool live = true;
        std::array<juce::uint64, 2> heldNotes {};   // bit n set = MIDI note n is held

        bool isHeld (int note) const noexcept
        {
            return note >= 0 && note < 128 && ((heldNotes[(size_t) (note >> 6)] >> (note & 63)) & 1u) != 0;
        }

        void setHeld (int note) noexcept
        {
            if (note >= 0 && note < 128)
                heldNotes[(size_t) (note >> 6)] |= (juce::uint64) 1 << (note & 63);
        }
    };

    /** Lock-free single-producer, single-consumer hand-off from the audio thread to the UI. */
    class ScopeFifo
    {
    public:
        /** Audio thread. Drops the frame if the UI has fallen behind. */
        void push (const ScopeFrame& frame) noexcept
        {
            const auto scope = fifo.write (1);
            if (scope.blockSize1 > 0)
                frames[(size_t) scope.startIndex1] = frame;
        }

        /** UI thread. Keeps only the newest waiting frame. */
        bool pullLatest (ScopeFrame& dest) noexcept
        {
            bool got = false;
            while (fifo.getNumReady() > 0)
            {
                const auto scope = fifo.read (1);
                if (scope.blockSize1 > 0)
                {
                    dest = frames[(size_t) scope.startIndex1];
                    got = true;
                }
            }
            return got;
        }

    private:
        static constexpr int capacity = 8;
        juce::AbstractFifo fifo { capacity };
        std::array<ScopeFrame, capacity> frames {};
    };
}
