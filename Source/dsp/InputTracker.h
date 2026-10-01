#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace grainlock
{
    /** What the engine remembers about the DRY input besides the audio itself: how loud it was and
        whether it was hiss (one entry every 32 samples, as far back as the input memory reaches), and
        when its last hits began. Snap, Threshold, Skip Hiss and Gate all read from here, so none of
        them ever hears the plugin's own Feedback.

        Times are counted in input samples since the last clear(): the sample pushed first is 0. */
    class InputTracker
    {
    public:
        static constexpr int hop = 32;
        static constexpr int numOnsets = 16;

        /** historyLength: how many input samples back the history must reach. */
        void prepare (double sampleRate, int historyLength)
        {
            int entries = 1;
            while (entries * hop < historyLength)
                entries <<= 1;

            peaks.assign ((size_t) entries, 0.0f);
            hiss.assign ((size_t) entries, (juce::uint8) 0);
            mask = entries - 1;

            followerRelease = (float) std::exp (-1.0 / (0.015 * sampleRate));
            fastCoeff = (float) (1.0 - std::exp (-1.0 / (0.003 * sampleRate)));
            slowCoeff = (float) (1.0 - std::exp (-1.0 / (0.100 * sampleRate)));
            highPass = (float) std::exp (-juce::MathConstants<double>::twoPi * 4000.0 / sampleRate);
            highGain = 0.5f * (1.0f + highPass);
            refractory = juce::jmax (1, (int) (0.030 * sampleRate));
            bridgeHops = juce::jmax (1, (int) std::lround (0.0015 * sampleRate / (double) hop));
            clear();
        }

        void clear() noexcept
        {
            std::fill (peaks.begin(), peaks.end(), 0.0f);
            std::fill (hiss.begin(), hiss.end(), (juce::uint8) 0);
            count = 0;
            follower = 0.0f;
            hopPeak = hopEnergy = hopHighEnergy = 0.0f;
            lastMono = highOut = 0.0f;
            fast = slow = 0.0f;
            armed = true;
            sinceOnset = refractory;
            onsets.fill (-1);
            nextOnset = 0;
        }

        /** One dry input sample. */
        void push (float left, float right) noexcept
        {
            if (! (std::isfinite (left) && std::isfinite (right)))
                left = right = 0.0f;

            const float magnitude = juce::jmax (std::abs (left), std::abs (right));
            hopPeak = juce::jmax (hopPeak, magnitude);

            // For the Gate: instant attack, 15 ms release.
            follower = magnitude > follower ? magnitude : follower * followerRelease;

            // Hiss: most of the energy lies above about 4 kHz.
            const float mono = 0.5f * (left + right);
            highOut = highPass * highOut + highGain * (mono - lastMono);
            lastMono = mono;
            hopEnergy += mono * mono;
            hopHighEnergy += highOut * highOut;

            // Hits: the last 3 ms are much louder than the last 100 ms.
            const float power = 0.5f * (left * left + right * right);
            fast += fastCoeff * (power - fast);
            slow += slowCoeff * (power - slow);
            ++sinceOnset;
            if (armed && sinceOnset >= refractory && fast > 4.0f * slow + onsetFloor)
            {
                onsets[(size_t) nextOnset] = count;
                nextOnset = (nextOnset + 1) % numOnsets;
                armed = false;
                sinceOnset = 0;
            }
            else if (! armed && fast < 2.0f * slow)
            {
                armed = true;
            }

            ++count;
            if ((count & (juce::int64) (hop - 1)) == 0)
            {
                const auto entry = (size_t) (((count / hop) - 1) & (juce::int64) mask);
                peaks[entry] = hopPeak;
                hiss[entry] = isHiss (hopEnergy, hopHighEnergy) ? (juce::uint8) 1 : (juce::uint8) 0;
                hopPeak = hopEnergy = hopHighEnergy = 0.0f;
            }
        }

        /** Input samples pushed since the last clear(). */
        juce::int64 clock() const noexcept { return count; }

        /** The level of the input right now (linear peak, 15 ms release). */
        float levelNow() const noexcept { return follower; }

        /** Is the stretch of regionSamples that ends endDelay samples before the newest input sample
            above threshold all the way through? It must be above at both ends; inside it, dips of up
            to 3 ms (the zero crossings of a low note) are ignored. Audio from before the start, or
            older than the history, counts as silence. */
        bool covered (int endDelay, int regionSamples, float threshold) const noexcept
        {
            const juce::int64 newest = count - 1 - (juce::int64) juce::jmax (0, endDelay);
            const juce::int64 oldest = newest - (juce::int64) juce::jmax (0, regionSamples);
            if (newest < 0)
                return false;

            const juce::int64 lastHop = newest / hop;
            const juce::int64 firstHop = oldest >= 0 ? oldest / hop : -1;

            int run = 0;
            bool anyAbove = false;
            for (juce::int64 h = firstHop; h <= lastHop; ++h)
            {
                if (peakOfHop (h) >= threshold)
                {
                    run = 0;
                    anyAbove = true;
                }
                else if (! anyAbove || ++run > 2 * bridgeHops)
                {
                    return false;   // a quiet start, or too long a dip
                }
            }
            return anyAbove && run == 0;   // and not a quiet end
        }

        /** The share (0..1) of that stretch that was hiss. */
        float hissShare (int endDelay, int regionSamples) const noexcept
        {
            const juce::int64 newest = count - 1 - (juce::int64) juce::jmax (0, endDelay);
            const juce::int64 oldest = newest - (juce::int64) juce::jmax (0, regionSamples);
            if (newest < 0)
                return 0.0f;

            const juce::int64 lastHop = newest / hop;
            const juce::int64 firstHop = juce::jmax ((juce::int64) 0, oldest) / hop;

            int flagged = 0;
            for (juce::int64 h = firstHop; h <= lastHop; ++h)
                flagged += hissOfHop (h) ? 1 : 0;
            return (float) flagged / (float) (lastHop - firstHop + 1);
        }

        /** The hit that began nearest to centre, no further than reach from it. */
        bool nearestOnset (juce::int64 centre, juce::int64 reach, juce::int64& onset) const noexcept
        {
            bool found = false;
            juce::int64 best = 0;
            for (const auto time : onsets)
            {
                if (time < 0)
                    continue;

                const juce::int64 distance = time > centre ? time - centre : centre - time;
                if (distance <= reach && (! found || distance < best))
                {
                    found = true;
                    best = distance;
                    onset = time;
                }
            }
            return found;
        }

    private:
        static bool isHiss (float energy, float highEnergy) noexcept
        {
            return energy > 1.0e-7f && highEnergy > 0.5f * energy;
        }

        /** Hop h holds input samples h * hop .. h * hop + hop - 1. */
        bool isStored (juce::int64 h) const noexcept
        {
            const juce::int64 stored = count / hop;
            return h >= 0 && h < stored && stored - h <= (juce::int64) mask;
        }

        float peakOfHop (juce::int64 h) const noexcept
        {
            if (h >= 0 && h == count / hop)
                return hopPeak;   // the hop still being filled
            return isStored (h) ? peaks[(size_t) (h & (juce::int64) mask)] : 0.0f;
        }

        bool hissOfHop (juce::int64 h) const noexcept
        {
            if (h >= 0 && h == count / hop)
                return isHiss (hopEnergy, hopHighEnergy);
            return isStored (h) && hiss[(size_t) (h & (juce::int64) mask)] != 0;
        }

        static constexpr float onsetFloor = 1.0e-7f;

        std::vector<float> peaks;        // the loudest sample of each hop
        std::vector<juce::uint8> hiss;   // 1 = that hop was hiss
        int mask = 0;
        juce::int64 count = 0;
        int bridgeHops = 2;

        float follower = 0.0f, followerRelease = 0.999f;
        float hopPeak = 0.0f, hopEnergy = 0.0f, hopHighEnergy = 0.0f;
        float lastMono = 0.0f, highOut = 0.0f, highPass = 0.6f, highGain = 0.8f;
        float fast = 0.0f, slow = 0.0f, fastCoeff = 0.007f, slowCoeff = 0.0002f;
        bool armed = true;
        int sinceOnset = 0, refractory = 1440;
        std::array<juce::int64, numOnsets> onsets {};
        int nextOnset = 0;
    };
}
