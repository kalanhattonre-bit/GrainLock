#pragma once

// The fixed script behind the sound fingerprints. It plays the same notes through the same
// settings every time and reduces the output to two numbers per 20 ms: the level, and the level of
// the sample-to-sample difference (which moves when the colour changes at the same level).
//
// A committed table of those numbers is a reference sound. Table A is version 0.2; table B is the
// default sound of 0.3. Neither is ever regenerated to make a test pass: if the sound was meant to
// change, that is a decision, and it gets a new table with a new name.
//
// This file must compile against 0.2 as well (that is how table A was made), so it sets parameters
// by text ID and skips the ones a version does not have.

#include "PluginProcessor.h"

#include <cmath>
#include <functional>
#include <vector>

namespace fingerprint
{
    constexpr double rate = 48000.0;
    constexpr double tempo = 117.3;           // grid lines fall between samples
    constexpr int totalSamples = 144000;      // 3 s
    constexpr int windowSamples = 960;        // 20 ms
    constexpr int numbersPerRun = 2 * (totalSamples / windowSamples);
    constexpr int numSources = 2;             // noise, and a 110 Hz buzz (alike cycles: the seam's hard case)

    struct ScriptPlayHead final : public juce::AudioPlayHead
    {
        double ppq = 0.0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (tempo);
            info.setPpqPosition (ppq);
            info.setIsPlaying (true);
            return info;
        }
    };

    struct Event
    {
        int time;
        juce::uint8 bytes[3];
    };

    /** A chord, four short notes, a note that starts under a held pitch bend, and a legato pair. */
    inline const std::vector<Event>& script()
    {
        static const std::vector<Event> events {
            { 24000, { 0x90, 57, 100 } }, { 24000, { 0x90, 60, 100 } }, { 24000, { 0x90, 64, 90 } },
            { 52800, { 0x80, 57, 0 } },   { 52800, { 0x80, 60, 0 } },   { 52800, { 0x80, 64, 0 } },
            { 62400, { 0x90, 72, 110 } }, { 66240, { 0x80, 72, 0 } },
            { 67200, { 0x90, 67, 80 } },  { 71040, { 0x80, 67, 0 } },
            { 72000, { 0x90, 72, 120 } }, { 75840, { 0x80, 72, 0 } },
            { 76800, { 0x90, 60, 60 } },  { 80640, { 0x80, 60, 0 } },
            { 86400, { 0xe0, 0x7f, 0x7f } },                               // bend fully up, then a note
            { 88800, { 0x90, 62, 100 } },
            { 105600, { 0xe0, 0x00, 0x40 } },                              // bend back to centre under it
            { 110400, { 0x80, 62, 0 } },
            { 115200, { 0x90, 60, 100 } }, { 120000, { 0x90, 67, 100 } },  // legato pair
            { 122400, { 0x80, 60, 0 } },   { 134400, { 0x80, 67, 0 } },
        };
        return events;
    }

    inline void set (GrainLockProcessor& p, const char* id, float plainValue)
    {
        if (auto* param = p.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    }

    /** 0: defaults. 1-5: the factory presets after Init. 6: mono glide in Live with all three LFOs.
        7 (0.3 only): defaults with Auto Gain on. Auto Gain is off everywhere else, as in 0.2. */
    inline int numConfigs (bool legacySeam) { return legacySeam ? 7 : 8; }

    inline void configure (GrainLockProcessor& p, int config)
    {
        if (config >= 1 && config <= 5)
            p.loadPreset (config);

        if (config == 6)
        {
            set (p, "captureMode", 1.0f);
            set (p, "mono", 1.0f);
            set (p, "glide", 120.0f);
            set (p, "pitchLfoOn", 1.0f);    set (p, "pitchLfoRate", 5.0f);    set (p, "pitchLfoDepth", 40.0f);
            set (p, "formantLfoOn", 1.0f);  set (p, "formantLfoShape", 1.0f); set (p, "formantLfoSync", 7.0f);  set (p, "formantLfoDepth", 50.0f);
            set (p, "grainLfoOn", 1.0f);    set (p, "grainLfoShape", 3.0f);   set (p, "grainLfoSync", 9.0f);    set (p, "grainLfoDepth", 60.0f);
        }

        set (p, "autoGain", config == 7 ? 1.0f : 0.0f);
    }

    /** Plays the script and returns the left output. blockSizeFor (position) gives each block's length. */
    inline std::vector<float> render (int config, int source, bool legacySeam, const std::function<int (int)>& blockSizeFor)
    {
        GrainLockProcessor proc;
        proc.setPlayConfigDetails (2, 2, rate, 4096);
        proc.prepareToPlay (rate, 4096);
        ScriptPlayHead playHead;
        proc.setPlayHead (&playHead);
        configure (proc, config);

       #ifdef GRAINLOCK_HAS_LEGACY_SEAM
        proc.setLegacySeamForTests (legacySeam);
       #else
        juce::ignoreUnused (legacySeam);
       #endif

        std::vector<float> inLeft ((size_t) totalSamples), inRight ((size_t) totalSamples);
        if (source == 0)
        {
            juce::Random rng (0x51d);
            for (size_t i = 0; i < inLeft.size(); ++i)
            {
                inLeft[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.3f;
                inRight[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.3f;
            }
        }
        else
        {
            for (size_t i = 0; i < inLeft.size(); ++i)
            {
                double s = 0.0;
                for (int k = 1; k <= 12; ++k)
                    s += std::sin (juce::MathConstants<double>::twoPi * 110.0 * k * (double) i / rate) / k;
                inLeft[i] = inRight[i] = (float) (0.12 * s);
            }
        }

        std::vector<float> out ((size_t) totalSamples);
        juce::AudioBuffer<float> buffer (2, 4096);
        juce::MidiBuffer midi;
        midi.ensureSize (1024);
        const auto& events = script();
        size_t next = 0;

        for (int pos = 0; pos < totalSamples;)
        {
            const int n = juce::jlimit (1, juce::jmin (4096, totalSamples - pos), blockSizeFor (pos));
            buffer.setSize (2, n, false, false, true);
            for (int i = 0; i < n; ++i)
            {
                buffer.setSample (0, i, inLeft[(size_t) (pos + i)]);
                buffer.setSample (1, i, inRight[(size_t) (pos + i)]);
            }

            midi.clear();
            while (next < events.size() && events[next].time < pos + n)
            {
                midi.addEvent (events[next].bytes, 3, juce::jmax (0, events[next].time - pos));
                ++next;
            }

            playHead.ppq = 3.17 + (double) pos * tempo / 60.0 / rate;   // from the position, never accumulated
            proc.processBlock (buffer, midi);

            for (int i = 0; i < n; ++i)
                out[(size_t) (pos + i)] = buffer.getSample (0, i);
            pos += n;
        }

        proc.setPlayHead (nullptr);
        return out;
    }

    inline void appendNumbers (const std::vector<float>& out, std::vector<float>& numbers)
    {
        float previous = 0.0f;
        for (int start = 0; start + windowSamples <= totalSamples; start += windowSamples)
        {
            double level = 0.0, difference = 0.0;
            for (int i = 0; i < windowSamples; ++i)
            {
                const float v = out[(size_t) (start + i)];
                level += (double) v * v;
                difference += (double) (v - previous) * (v - previous);
                previous = v;
            }
            numbers.push_back ((float) std::sqrt (level / windowSamples));
            numbers.push_back ((float) std::sqrt (difference / windowSamples));
        }
    }

    /** Every configuration over both sources, at 256-sample blocks. */
    inline std::vector<float> table (bool legacySeam)
    {
        std::vector<float> numbers;
        for (int config = 0; config < numConfigs (legacySeam); ++config)
            for (int source = 0; source < numSources; ++source)
                appendNumbers (render (config, source, legacySeam, [] (int) { return 256; }), numbers);
        return numbers;
    }

    inline bool write (const juce::File& file, const std::vector<float>& numbers)
    {
        juce::String text;
        text.preallocateBytes (numbers.size() * 16);
        for (const float v : numbers)
            text << juce::String ((double) v, 9, true) << "\n";
        return file.replaceWithText (text);
    }

    /** What a number in a table stands for, for a failure message. */
    inline juce::String describe (int index)
    {
        static const char* names[] = { "defaults", "Robot Voice", "Stutter Gate", "Drone Pad", "Glitch Drums",
                                       "Formant Choir", "mono glide + LFOs", "defaults + Auto Gain" };
        const int run = index / numbersPerRun, within = index % numbersPerRun;
        const int config = run / numSources, source = run % numSources;
        return juce::String (names[juce::jlimit (0, 7, config)]) + (source == 0 ? ", noise" : ", buzz")
               + ", " + juce::String ((within / 2) * 20) + " ms, " + ((within % 2) == 0 ? "level" : "colour");
    }
}
