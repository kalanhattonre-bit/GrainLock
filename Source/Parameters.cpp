#include "Parameters.h"

namespace grainlock
{
    namespace
    {
        using Range = juce::NormalisableRange<float>;

        juce::String signedString (float v, int decimals)
        {
            const auto text = juce::String (v, decimals);
            return v > 0.0f ? "+" + text : text;
        }

        juce::String formatMs (float ms, int)
        {
            if (ms >= 1000.0f)
                return juce::String (ms / 1000.0f, 2) + " s";
            return juce::String (ms, ms < 10.0f ? 1 : 0) + " ms";
        }

        float parseMs (const juce::String& text)
        {
            const auto t = text.trim().toLowerCase();
            const float v = t.getFloatValue();
            return (t.endsWith ("s") && ! t.endsWith ("ms")) ? v * 1000.0f : v;
        }

        juce::String formatPercent (float v, int)    { return juce::String (juce::roundToInt (v)) + "%"; }
        juce::String formatSemitones (float v, int)  { return signedString (v, 1) + " st"; }
        juce::String formatCents (float v, int)      { return signedString ((float) juce::roundToInt (v), 0) + " ct"; }
        juce::String formatDb (float v, int)         { return signedString (v, 1) + " dB"; }

        juce::String formatHz (float v, int)
        {
            return juce::String (v, v < 1.0f ? 2 : (v < 10.0f ? 2 : 1)) + " Hz";
        }

        float parseNumber (const juce::String& text) { return text.trim().getFloatValue(); }

        Range skewedRange (float lo, float hi, float step, float centre)
        {
            Range r (lo, hi, step);
            r.setSkewForCentre (centre);
            return r;
        }

        auto floatParam (const char* id, const char* name, Range range, float def,
                         juce::AudioParameterFloatAttributes::StringFromValue toText,
                         juce::AudioParameterFloatAttributes::ValueFromString fromText = parseNumber)
        {
            return std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { id, 1 }, name, range, def,
                juce::AudioParameterFloatAttributes()
                    .withStringFromValueFunction (std::move (toText))
                    .withValueFromStringFunction (std::move (fromText)));
        }

        auto boolParam (const char* id, const char* name, bool def)
        {
            return std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { id, 1 }, name, def,
                juce::AudioParameterBoolAttributes().withStringFromValueFunction (
                    [] (bool v, int) { return juce::String (v ? "On" : "Off"); }));
        }

        auto choiceParam (const char* id, const char* name, const juce::StringArray& choices, int def)
        {
            return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, choices, def);
        }
    }

    const juce::StringArray& lfoSyncChoices()
    {
        static const juce::StringArray choices { "Free", "1/1", "1/1T", "1/2", "1/2T", "1/4", "1/4T",
                                                 "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T" };
        return choices;
    }

    double lfoSyncBeats (int syncIndex) noexcept
    {
        // Quarter-note beats per LFO cycle; triplets are two thirds of the straight value.
        static constexpr double beats[] = { 0.0, 4.0, 8.0 / 3.0, 2.0, 4.0 / 3.0, 1.0, 2.0 / 3.0,
                                            0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0 };
        constexpr int count = (int) (sizeof (beats) / sizeof (beats[0]));
        return beats[juce::jlimit (0, count - 1, syncIndex)];
    }

    juce::String formatNoteName (int midiNote)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        midiNote = juce::jlimit (0, 127, midiNote);
        return juce::String (names[midiNote % 12]) + juce::String (midiNote / 12 - 2);
    }

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        // FREEZE
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { ParamID::grainCycles, 1 }, "Grain Cycles", minCycles, maxCycles, 2,
            juce::AudioParameterIntAttributes().withStringFromValueFunction (
                [] (int v, int) { return juce::String (v) + (v == 1 ? " cycle" : " cycles"); })));
        layout.add (floatParam (ParamID::smooth, "Smooth", Range (0.0f, 50.0f, 0.1f), 10.0f, formatPercent));
        layout.add (floatParam (ParamID::offset, "Offset", skewedRange (0.0f, 500.0f, 0.1f, 80.0f), 0.0f, formatMs, parseMs));
        layout.add (choiceParam (ParamID::captureMode, "Capture Mode", { "Hold", "Live" }, (int) CaptureMode::live));
        layout.add (floatParam (ParamID::refresh, "Refresh", skewedRange (5.0f, 500.0f, 0.1f, 50.0f), 25.0f, formatMs, parseMs));
        layout.add (boolParam (ParamID::pitchLock, "Pitch Lock", true));

        // VOICE
        layout.add (floatParam (ParamID::formant, "Formant", Range (-12.0f, 12.0f, 0.01f), 0.0f, formatSemitones));
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { ParamID::tune, 1 }, "Tune", -24, 24, 0,
            juce::AudioParameterIntAttributes().withStringFromValueFunction (
                [] (int v, int) { return signedString ((float) v, 0) + " st"; })));
        layout.add (floatParam (ParamID::fine, "Fine", Range (-100.0f, 100.0f, 0.1f), 0.0f, formatCents));
        layout.add (floatParam (ParamID::glide, "Glide", skewedRange (0.0f, 2000.0f, 0.1f, 200.0f), 0.0f, formatMs, parseMs));
        layout.add (boolParam (ParamID::mono, "Mono", false));
        layout.add (floatParam (ParamID::velSens, "Velocity Sens", Range (0.0f, 100.0f, 0.1f), 100.0f, formatPercent));

        // ENVELOPE
        layout.add (floatParam (ParamID::attack, "Attack", skewedRange (0.0f, 2000.0f, 0.1f, 100.0f), 5.0f, formatMs, parseMs));
        layout.add (floatParam (ParamID::decay, "Decay", skewedRange (1.0f, 5000.0f, 0.1f, 300.0f), 250.0f, formatMs, parseMs));
        layout.add (floatParam (ParamID::sustain, "Sustain", Range (0.0f, 100.0f, 0.1f), 100.0f, formatPercent));
        layout.add (floatParam (ParamID::release, "Release", skewedRange (1.0f, 5000.0f, 0.1f, 400.0f), 150.0f, formatMs, parseMs));

        // LFOs: one each for pitch, formant and grain cycles, all able to run at once.
        const char* lfoNames[] = { "Pitch LFO", "Formant LFO", "Grain LFO" };
        for (int i = 0; i < numLfos; ++i)
        {
            const auto& ids = ParamID::lfo[i];
            const juce::String name (lfoNames[i]);
            layout.add (boolParam (ids.on, (name + " On").toRawUTF8(), false));
            layout.add (floatParam (ids.rate, (name + " Rate").toRawUTF8(), skewedRange (0.01f, 30.0f, 0.001f, 2.0f), 1.0f, formatHz));
            layout.add (choiceParam (ids.sync, (name + " Sync").toRawUTF8(), lfoSyncChoices(), 0));
            layout.add (choiceParam (ids.shape, (name + " Shape").toRawUTF8(), { "Sine", "Triangle", "Square", "S&H" }, (int) LfoShape::sine));
            layout.add (floatParam (ids.depth, (name + " Depth").toRawUTF8(), Range (0.0f, 100.0f, 0.1f), 50.0f, formatPercent));
        }

        // OUTPUT
        layout.add (floatParam (ParamID::mix, "Mix", Range (0.0f, 100.0f, 0.1f), 100.0f, formatPercent));
        layout.add (boolParam (ParamID::dryWhenIdle, "Dry When Idle", true));
        layout.add (floatParam (ParamID::outGain, "Output Gain", Range (-24.0f, 24.0f, 0.1f), 0.0f, formatDb));

        // 0.3. Every default leaves a 0.2 project sounding the same, except Auto Gain, which is on
        // for new work and switched off when an older project is loaded (migratePre03State).
        layout.add (boolParam (ParamID::formantTrack, "Formant Track", false));
        layout.add (boolParam (ParamID::autoGain, "Auto Gain", true));

        // Handed to the host as its bypass switch, so bypassing crossfades instead of cutting.
        layout.add (boolParam (ParamID::bypass, "Bypass", false));

        return layout;
    }

    ParameterRefs::ParameterRefs (juce::AudioProcessorValueTreeState& state)
    {
        auto get = [&state] (const char* id)
        {
            auto* p = state.getRawParameterValue (id);
            jassert (p != nullptr);
            return p;
        };

        grainCycles = get (ParamID::grainCycles);
        smooth      = get (ParamID::smooth);
        offset      = get (ParamID::offset);
        captureMode = get (ParamID::captureMode);
        refresh     = get (ParamID::refresh);
        pitchLock   = get (ParamID::pitchLock);
        formant     = get (ParamID::formant);
        tune        = get (ParamID::tune);
        fine        = get (ParamID::fine);
        glide       = get (ParamID::glide);
        mono        = get (ParamID::mono);
        velSens     = get (ParamID::velSens);
        attack      = get (ParamID::attack);
        decay       = get (ParamID::decay);
        sustain     = get (ParamID::sustain);
        release     = get (ParamID::release);
        for (int i = 0; i < numLfos; ++i)
        {
            const auto& ids = ParamID::lfo[i];
            lfo[(size_t) i] = Lfo { get (ids.on), get (ids.rate), get (ids.sync), get (ids.shape), get (ids.depth) };
        }
        mix         = get (ParamID::mix);
        dryWhenIdle = get (ParamID::dryWhenIdle);
        outGain     = get (ParamID::outGain);
        bypass      = get (ParamID::bypass);

        formantTrack = get (ParamID::formantTrack);
        autoGain     = get (ParamID::autoGain);
    }

    void fillMissingParameters (juce::XmlElement& state, const juce::AudioProcessorValueTreeState& apvts)
    {
        // Parameters whose default is not what a version without them did.
        struct Legacy { const char* id; float value; };
        static constexpr Legacy legacy[] = {
            { ParamID::autoGain, 0.0f },   // on for new work; older projects keep their level
        };

        for (const char* id : ParamID::all)
        {
            bool present = false;
            for (auto* child : state.getChildWithTagNameIterator ("PARAM"))
                if (child->getStringAttribute ("id") == id)
                {
                    present = true;
                    break;
                }
            if (present)
                continue;

            const auto* parameter = apvts.getParameter (id);
            if (parameter == nullptr)
                continue;

            float value = parameter->convertFrom0to1 (parameter->getDefaultValue());
            for (const auto& entry : legacy)
                if (juce::String (entry.id) == id)
                    value = entry.value;

            auto* node = state.createNewChildElement ("PARAM");
            node->setAttribute ("id", id);
            node->setAttribute ("value", (double) value);
        }
    }

    void migrateLegacyLfoState (juce::XmlElement& state)
    {
        auto findParam = [&state] (const char* id) -> juce::XmlElement*
        {
            for (auto* child : state.getChildWithTagNameIterator ("PARAM"))
                if (child->getStringAttribute ("id") == id)
                    return child;
            return nullptr;
        };

        auto* targetNode = findParam ("lfoTarget");
        if (targetNode == nullptr)
            return;   // already the three-LFO layout

        auto valueOf = [&findParam] (const char* id, double fallback)
        {
            const auto* node = findParam (id);
            return node != nullptr ? node->getDoubleAttribute ("value", fallback) : fallback;
        };

        const int target = juce::jlimit (0, numLfos - 1, juce::roundToInt (targetNode->getDoubleAttribute ("value")));
        const double rate = valueOf ("lfoRate", 1.0);
        const double sync = valueOf ("lfoSync", 0.0);
        const double shape = valueOf ("lfoShape", 0.0);
        const double depth = valueOf ("lfoDepth", 0.0);

        for (const char* oldId : { "lfoRate", "lfoSync", "lfoShape", "lfoDepth", "lfoTarget" })
            if (auto* node = findParam (oldId))
                state.removeChildElement (node, true);

        // An old LFO at zero depth was effectively off: leave the new LFOs at their defaults.
        if (depth <= 0.0)
            return;

        auto setParam = [&state, &findParam] (const char* id, double value)
        {
            auto* node = findParam (id);
            if (node == nullptr)
            {
                node = state.createNewChildElement ("PARAM");
                node->setAttribute ("id", id);
            }
            node->setAttribute ("value", value);
        };

        const auto& ids = ParamID::lfo[target];
        setParam (ids.on, 1.0);
        setParam (ids.rate, rate);
        setParam (ids.sync, sync);
        setParam (ids.shape, shape);
        setParam (ids.depth, depth);
    }
}
