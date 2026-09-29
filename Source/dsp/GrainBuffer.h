#pragma once

#include "InputRing.h"

namespace grainlock
{
    /** 4-point, 3rd-order Hermite interpolation. Reads x[i-1] .. x[i+2] around idx. */
    inline float hermite4 (const float* x, double idx) noexcept
    {
        const int i = (int) idx;
        const float t = (float) (idx - (double) i);
        const float xm1 = x[i - 1], x0 = x[i], x1 = x[i + 1], x2 = x[i + 2];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    /** A captured slice of input. Positions are measured backwards from the slice's end:
        0 is the end, -span is the earliest readable point. */
    class GrainBuffer
    {
    public:
        void allocate (int capacity)
        {
            for (auto& channel : samples)
                channel.assign ((size_t) capacity, 0.0f);
            length = 0;
        }

        int capacity() const noexcept { return (int) samples[0].size(); }

        /** Largest span (in samples) that capture() can hold. */
        int maxSpan() const noexcept { return juce::jmax (0, capacity() - guardSamples); }

        /** How far back from the end a read may go. */
        double span() const noexcept { return (double) juce::jmax (0, length - guardSamples); }

        /** Period, in samples, of the note that was playing when this slice was taken. */
        double capturePeriod() const noexcept { return period; }

        /** Copies spanSamples (plus interpolation guards) ending endDelay samples before the newest input. */
        void capture (const InputRing& ring, int endDelay, int spanSamples, double notePeriod) noexcept
        {
            const int count = juce::jlimit (0, capacity(), spanSamples + guardSamples);
            if (count < minimumLength)
            {
                length = 0;
                return;
            }

            ring.copyEnding (endDelay, count, samples[0].data(), samples[1].data());
            length = count;
            period = notePeriod;
        }

        bool isEmpty() const noexcept { return length < minimumLength; }

        /** True when a capture filled the whole buffer, so capturing again cannot hold more. */
        bool isFull() const noexcept { return length >= capacity(); }

        /** Reads both channels at a position <= 0 (see class comment). */
        void read (double position, float& left, float& right) const noexcept
        {
            if (length < minimumLength)
            {
                left = right = 0.0f;
                return;
            }

            // One guard sample before the earliest point and two after the end, for the Hermite taps.
            // Written so a NaN position also lands on a valid index.
            const double last = (double) (length - 3);
            double idx = last + position;
            if (! (idx >= 1.0))
                idx = 1.0;
            else if (idx > last)
                idx = last;
            left = hermite4 (samples[0].data(), idx);
            right = hermite4 (samples[1].data(), idx);
        }

    private:
        static constexpr int guardSamples = 4;
        static constexpr int minimumLength = 8;

        std::array<std::vector<float>, 2> samples;
        int length = 0;
        double period = 100.0;
    };
}
