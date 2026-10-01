#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <vector>

namespace grainlock
{
    /** What the Tone section is asked to do. Each value has a setting that means "off". */
    struct ToneSettings
    {
        float lowCutHz = 20.0f;        // 20 = off
        float highCutHz = 20000.0f;    // 20000 = off
        float tiltDb = 0.0f;           // above 0: brighter
        float driveDb = 0.0f;
        float diffuse = 0.0f;          // 0..1
    };

    /** Shapes the frozen sound (and only it): Low Cut, Drive, Tilt, High Cut, Diffuse, in that order.

        A stage that is off is not in the path at all, so with everything off the sound is untouched
        to the last bit. A stage that is switched on or off crossfades over 10 ms instead of clicking.
        Settings are followed every 32 samples whatever the host's block size, so the result does not
        depend on it. Nothing here adds latency. */
    class ToneChain
    {
    public:
        static constexpr float lowCutOffHz = 20.0f;
        static constexpr float highCutOffHz = 20000.0f;

        void prepare (double newSampleRate)
        {
            sampleRate = newSampleRate;
            fadeStep = (float) (1.0 / (0.010 * sampleRate));
            followCoeff = (float) (1.0 - std::exp (-(double) tick / (0.030 * sampleRate)));
            tiltCoeff = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 800.0 / sampleRate));
            lengthSlew = (float) (1.0 - std::exp (-1.0 / (0.005 * sampleRate)));

            // 4x: what the clipper adds above half the sample rate is filtered off instead of folding
            // back as notes that are not in the sound. Polyphase IIR, so there is no latency to report.
            oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
                (size_t) 2, (size_t) 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
            oversampler->initProcessing ((size_t) 1);

            const int length = juce::nextPowerOfTwo ((int) std::ceil (0.021 * sampleRate) + 8);
            for (auto& channel : diffusers)
                for (auto& stage : channel)
                    stage.line.assign ((size_t) length, 0.0f);
            diffuseMask = length - 1;

            // Each stage of each side turns at its own slow rate, so no part of the sound sits still in a notch.
            static constexpr double rates[2][numDiffusers] = { { 0.13, 0.31, 0.19, 0.43 }, { 0.17, 0.23, 0.37, 0.11 } };
            for (size_t ch = 0; ch < 2; ++ch)
                for (size_t s = 0; s < (size_t) numDiffusers; ++s)
                {
                    diffusers[ch][s].phaseStep = rates[ch][s] / sampleRate;
                    diffusers[ch][s].phase = 0.21 * (double) (s + 1) + 0.37 * (double) ch;
                }

            // Nothing is in the path until settle() or a fade puts it there.
            lowCutFade = highCutFade = tiltFade = driveFade = diffuseFade = 0.0f;
            current = ToneSettings {};
            target = ToneSettings {};
            reset();
        }

        /** Forgets everything that is ringing. The settings stay. */
        void reset() noexcept
        {
            for (auto& f : lowCut)  f = Biquad {};
            for (auto& f : highCut) f = Biquad {};
            tiltState = { 0.0f, 0.0f };
            if (oversampler != nullptr)
                oversampler->reset();
            clearDiffusers();
            countdown = 0;
        }

        /** Once per block. A stage that was fully out starts from the new value, not from wherever its
            control last was. */
        void setTargets (const ToneSettings& wanted) noexcept
        {
            target = wanted;
            target.lowCutHz = juce::jlimit (lowCutOffHz, 2000.0f, target.lowCutHz);
            target.highCutHz = juce::jlimit (500.0f, highCutOffHz, target.highCutHz);
            target.tiltDb = juce::jlimit (-6.0f, 6.0f, target.tiltDb);
            target.driveDb = juce::jlimit (0.0f, 24.0f, target.driveDb);
            target.diffuse = juce::jlimit (0.0f, 1.0f, target.diffuse);

            bool jumped = false;
            auto jump = [&jumped] (float fade, float& value, float to)
            {
                if (fade <= 0.0f && ! juce::exactlyEqual (value, to))
                {
                    value = to;
                    jumped = true;
                }
            };
            jump (lowCutFade, current.lowCutHz, target.lowCutHz);
            jump (highCutFade, current.highCutHz, target.highCutHz);
            jump (tiltFade, current.tiltDb, target.tiltDb);
            jump (driveFade, current.driveDb, target.driveDb);
            jump (diffuseFade, current.diffuse, target.diffuse);
            if (jumped)
                countdown = 0;   // worked out afresh on the next sample
        }

        /** Nothing is in the path and nothing is fading out: process() would change nothing. */
        bool isNeutral() const noexcept
        {
            return ! (lowCutOn() || highCutOn() || tiltOn() || driveOn() || diffuseOn())
                   && lowCutFade <= 0.0f && highCutFade <= 0.0f && tiltFade <= 0.0f && driveFade <= 0.0f && diffuseFade <= 0.0f;
        }

        /** A stage is part-way through switching on or off, or a setting in use is not yet the one
            asked for. (While nothing sounds, settle() puts that right at once.) */
        bool isUnsettled() const noexcept
        {
            auto midway = [] (float fade, bool on) { return on ? fade < 1.0f : fade > 0.0f; };
            auto behind = [] (float inUse, float asked) { return ! juce::exactlyEqual (inUse, asked); };
            return midway (lowCutFade, lowCutOn()) || midway (highCutFade, highCutOn()) || midway (tiltFade, tiltOn())
                   || midway (driveFade, driveOn()) || midway (diffuseFade, diffuseOn())
                   || behind (current.lowCutHz, target.lowCutHz) || behind (current.highCutHz, target.highCutHz)
                   || behind (current.tiltDb, target.tiltDb) || behind (current.driveDb, target.driveDb)
                   || behind (current.diffuse, target.diffuse);
        }

        /** For when nothing is sounding: every stage is simply on or off, at the settings asked for,
            and nothing rings. */
        void settle() noexcept
        {
            lowCutFade = lowCutOn() ? 1.0f : 0.0f;
            highCutFade = highCutOn() ? 1.0f : 0.0f;
            tiltFade = tiltOn() ? 1.0f : 0.0f;
            driveFade = driveOn() ? 1.0f : 0.0f;
            diffuseFade = diffuseOn() ? 1.0f : 0.0f;
            current = target;
            reset();
        }

        /** How long the section can go on sounding after its input has stopped, in seconds. */
        double tailSeconds() const noexcept { return (diffuseOn() || diffuseFade > 0.0f) ? 0.8 : 0.05; }

        void process (float& left, float& right) noexcept
        {
            if (--countdown <= 0)
            {
                countdown = tick;
                follow();
            }

            // A sample that is not a number (or absurdly large) must never get into a filter: it would
            // stay there for as long as a note sounds.
            auto clean = [] (float v) { return std::abs (v) < 1.0e6f ? v : 0.0f; };
            float io[2] = { clean (left), clean (right) };

            // Low Cut
            if (advance (lowCutFade, lowCutOn(), [this] { for (auto& f : lowCut) f = Biquad {}; }))
                for (size_t ch = 0; ch < 2; ++ch)
                    io[ch] += lowCutFade * (lowCut[ch].process (io[ch], lowCutCoeffs) - io[ch]);

            // Drive
            if (advance (driveFade, driveOn(), [this] { oversampler->reset(); }))
            {
                float driven[2] = { io[0], io[1] };
                float* channels[2] = { &driven[0], &driven[1] };
                juce::dsp::AudioBlock<float> block (channels, (size_t) 2, (size_t) 1);
                auto up = oversampler->processSamplesUp (block);
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    float* samples = up.getChannelPointer (ch);
                    for (size_t i = 0; i < up.getNumSamples(); ++i)
                    {
                        const float x = samples[i];
                        samples[i] = x + driveBlend * (driveMakeup * std::tanh (driveGain * x) - x);
                    }
                }
                oversampler->processSamplesDown (block);
                for (size_t ch = 0; ch < 2; ++ch)
                    io[ch] += driveFade * (driven[ch] - io[ch]);
            }

            // Tilt: lows one way, highs the other, turning about 800 Hz.
            if (advance (tiltFade, tiltOn(), [this] { tiltState = { 0.0f, 0.0f }; }))
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    tiltState[ch] += tiltCoeff * (io[ch] - tiltState[ch]);
                    const float tilted = tiltHigh * io[ch] + (tiltLow - tiltHigh) * tiltState[ch];
                    io[ch] += tiltFade * (tilted - io[ch]);
                }

            // High Cut
            if (advance (highCutFade, highCutOn(), [this] { for (auto& f : highCut) f = Biquad {}; }))
                for (size_t ch = 0; ch < 2; ++ch)
                    io[ch] += highCutFade * (highCut[ch].process (io[ch], highCutCoeffs) - io[ch]);

            // Diffuse: a chain of allpasses in series, never mixed with the sound that went in (a mix
            // would comb it). The amount sets how long they are.
            if (advance (diffuseFade, diffuseOn(), [this] { clearDiffusers(); }))
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    float x = io[ch];
                    for (size_t s = 0; s < (size_t) numDiffusers; ++s)
                        x = diffusers[ch][s].process (x, diffuseLength[ch][s], diffuseDepth, lengthSlew, diffuseMask);
                    io[ch] += diffuseFade * (x - io[ch]);
                }

            left = io[0];
            right = io[1];
        }

    private:
        static constexpr int tick = 32;
        static constexpr int numDiffusers = 4;

        struct Coeffs { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };

        struct Biquad
        {
            float s1 = 0.0f, s2 = 0.0f;

            float process (float x, const Coeffs& c) noexcept
            {
                const float y = c.b0 * x + s1;
                s1 = c.b1 * x - c.a1 * y + s2;
                s2 = c.b2 * x - c.a2 * y;
                return y;
            }
        };

        struct Diffuser
        {
            std::vector<float> line;
            int write = 0;
            double phase = 0.0, phaseStep = 0.0;
            float length = 0.0f;   // the length in use: glides to the one asked for, so a step in Diffuse does not crackle

            float process (float x, float lengthSamples, float depthSamples, float slew, int mask) noexcept
            {
                phase += phaseStep;
                if (phase >= 1.0)
                    phase -= 1.0;

                // A parabola through 0, 1, 0, -1, 0: near enough a sine, and cheap.
                const double p = phase < 0.5 ? phase : phase - 1.0;
                const float wobble = (float) (16.0 * p * (0.5 - std::abs (p)));

                length = length < 1.0f ? lengthSamples : length + slew * (lengthSamples - length);

                const float delay = juce::jmax (1.0f, length + depthSamples * wobble);
                const int whole = (int) delay;
                const float part = delay - (float) whole;
                const float a = line[(size_t) ((write - whole) & mask)];
                const float b = line[(size_t) ((write - whole - 1) & mask)];
                const float delayed = a + part * (b - a);

                const float stored = x + gain * delayed;
                line[(size_t) (write & mask)] = stored;
                write = (write + 1) & mask;
                return delayed - gain * stored;
            }

            static constexpr float gain = 0.6f;
        };

        bool lowCutOn() const noexcept  { return target.lowCutHz > lowCutOffHz; }
        bool highCutOn() const noexcept { return target.highCutHz < highCutOffHz; }
        bool tiltOn() const noexcept    { return ! juce::exactlyEqual (target.tiltDb, 0.0f); }
        bool driveOn() const noexcept   { return target.driveDb > 0.0f; }
        bool diffuseOn() const noexcept { return target.diffuse > 0.0f; }

        /** Moves a stage's fade towards on or off. Returns true while the stage has to run. */
        template <typename Fn>
        bool advance (float& fade, bool on, Fn&& whenSwitchedIn) noexcept
        {
            if (on)
            {
                if (fade <= 0.0f)
                    whenSwitchedIn();
                fade = juce::jmin (1.0f, fade + fadeStep);
            }
            else if (fade > 0.0f)
            {
                fade = juce::jmax (0.0f, fade - fadeStep);
            }
            return fade > 0.0f;
        }

        void clearDiffusers() noexcept
        {
            for (auto& channel : diffusers)
                for (auto& stage : channel)
                {
                    std::fill (stage.line.begin(), stage.line.end(), 0.0f);
                    stage.write = 0;
                    stage.length = 0.0f;   // starts at the length asked for
                }
        }

        /** Every 32 samples: the settings in use move towards the ones asked for. */
        void follow() noexcept
        {
            // Frequencies move in proportion (by ear), the rest in a straight line.
            current.lowCutHz *= std::pow (target.lowCutHz / current.lowCutHz, followCoeff);
            current.highCutHz *= std::pow (target.highCutHz / current.highCutHz, followCoeff);
            current.tiltDb += followCoeff * (target.tiltDb - current.tiltDb);
            current.driveDb += followCoeff * (target.driveDb - current.driveDb);

            // Diffuse on its way out keeps its lengths: it only fades. (Shrinking them under the fade
            // would be heard.)
            if (diffuseOn())
                current.diffuse += followCoeff * (target.diffuse - current.diffuse);

            lowCutCoeffs = design (current.lowCutHz, true);
            highCutCoeffs = design (current.highCutHz, false);

            tiltHigh = juce::Decibels::decibelsToGain (current.tiltDb);
            tiltLow = juce::Decibels::decibelsToGain (-current.tiltDb);

            // The clipper: gain in, a level that keeps a signal peaking at 0.5 where it was, and a
            // blend that eases it in over the first 3 dB so leaving 0 is not a jump.
            driveGain = juce::Decibels::decibelsToGain (current.driveDb);
            driveMakeup = 0.5f / std::tanh (0.5f * driveGain);
            driveBlend = juce::jlimit (0.0f, 1.0f, current.driveDb / 3.0f);

            static constexpr double fullMs[2][numDiffusers] = { { 4.31, 7.13, 11.27, 17.93 }, { 4.93, 7.67, 12.11, 19.07 } };
            for (size_t ch = 0; ch < 2; ++ch)
                for (size_t s = 0; s < (size_t) numDiffusers; ++s)
                    diffuseLength[ch][s] = 1.0f + current.diffuse * (float) (fullMs[ch][s] * 0.001 * sampleRate - 1.0);
            diffuseDepth = current.diffuse * (float) (0.00015 * sampleRate);
        }

        Coeffs design (float hz, bool highPass) const noexcept
        {
            const double w = juce::MathConstants<double>::twoPi * juce::jlimit (10.0, 0.45 * sampleRate, (double) hz) / sampleRate;
            const double cosW = std::cos (w), alpha = std::sin (w) / std::sqrt (2.0);   // Q = 0.707: 12 dB per octave, no bump
            const double a0 = 1.0 + alpha;
            const double edge = highPass ? (1.0 + cosW) * 0.5 : (1.0 - cosW) * 0.5;

            Coeffs c;
            c.b0 = (float) (edge / a0);
            c.b1 = (float) ((highPass ? -2.0 : 2.0) * edge / a0);
            c.b2 = (float) (edge / a0);
            c.a1 = (float) (-2.0 * cosW / a0);
            c.a2 = (float) ((1.0 - alpha) / a0);
            return c;
        }

        double sampleRate = 48000.0;
        ToneSettings target, current;
        int countdown = 0;
        float followCoeff = 0.02f, fadeStep = 0.002f, lengthSlew = 0.004f;

        float lowCutFade = 0.0f, highCutFade = 0.0f, tiltFade = 0.0f, driveFade = 0.0f, diffuseFade = 0.0f;

        std::array<Biquad, 2> lowCut, highCut;
        Coeffs lowCutCoeffs, highCutCoeffs;

        std::array<float, 2> tiltState { 0.0f, 0.0f };
        float tiltCoeff = 0.1f, tiltLow = 1.0f, tiltHigh = 1.0f;

        std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
        float driveGain = 1.0f, driveMakeup = 1.0f, driveBlend = 0.0f;

        std::array<std::array<Diffuser, numDiffusers>, 2> diffusers;
        std::array<std::array<float, numDiffusers>, 2> diffuseLength {};
        float diffuseDepth = 0.0f;
        int diffuseMask = 0;
    };
}
