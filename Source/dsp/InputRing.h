#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace grainlock
{
    /** The most recent stretch of stereo input, kept so a note can grab audio from just before it. */
    class InputRing
    {
    public:
        /** Allocates at least minimumLength samples per channel (rounded up to a power of two). */
        void prepare (int minimumLength)
        {
            int length = 1;
            while (length < minimumLength)
                length <<= 1;

            for (auto& channel : data)
                channel.assign ((size_t) length, 0.0f);

            mask = length - 1;
            writePos = 0;
        }

        void clear() noexcept
        {
            for (auto& channel : data)
                std::fill (channel.begin(), channel.end(), 0.0f);
            writePos = 0;
        }

        int size() const noexcept { return mask + 1; }

        void push (float left, float right) noexcept
        {
            data[0][(size_t) writePos] = left;
            data[1][(size_t) writePos] = right;
            writePos = (writePos + 1) & mask;
        }

        /** Copies count samples per channel. The last one copied lies endDelay samples
            before the newest pushed sample (endDelay 0 means it IS the newest). */
        void copyEnding (int endDelay, int count, float* destLeft, float* destRight) const noexcept
        {
            jassert (count > 0 && endDelay >= 0 && count + endDelay <= size());

            const int newest = (writePos - 1) & mask;
            const int start = (newest - endDelay - count + 1) & mask;
            const int firstPart = std::min (count, size() - start);
            const int secondPart = count - firstPart;

            float* dest[2] = { destLeft, destRight };
            for (size_t ch = 0; ch < 2; ++ch)
            {
                std::memcpy (dest[ch], data[ch].data() + start, sizeof (float) * (size_t) firstPart);
                if (secondPart > 0)
                    std::memcpy (dest[ch] + firstPart, data[ch].data(), sizeof (float) * (size_t) secondPart);
            }
        }

    private:
        std::array<std::vector<float>, 2> data;
        int mask = 0;
        int writePos = 0;
    };
}
