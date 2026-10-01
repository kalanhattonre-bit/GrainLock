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

        /** For whole-number parameters: getIntValue stops at a leading '+', so "+3 st" would read as 0. */
        int parseWhole (const juce::String& text, int) { return juce::roundToInt (text.trim().getFloatValue()); }

        juce::String formatDegrees (float v, int)    { return juce::String (juce::roundToInt (v)) + " deg"; }
        juce::String formatSemitonesWhole (int v, int) { return signedString ((float) v, 0) + " st"; }

        auto intParam (const char* id, const char* name, int lo, int hi, int def, std::function<juce::String (int, int)> toText)
        {
            return std::make_unique<juce::AudioParameterInt> (
                juce::ParameterID { id, 1 }, name, lo, hi, def,
                juce::AudioParameterIntAttributes()
                    .withStringFromValueFunction (std::move (toText))
                    .withValueFromStringFunction ([] (const juce::String& text) { return parseWhole (text, 0); }));
        }

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

    const juce::StringArray& lfoShapeChoices()
    {
        static const juce::StringArray choices { "Sine", "Triangle", "Square", "S&H", "Saw", "Random" };
        return choices;
    }

    const juce::StringArray& grabSyncChoices()
    {
        static const juce::StringArray choices { "Free", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1/1" };
        return choices;
    }

    double grabSyncBeats (int syncIndex) noexcept
    {
        static constexpr double beats[] = { 0.0, 0.125, 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0, 2.0, 4.0 };
        constexpr int count = (int) (sizeof (beats) / sizeof (beats[0]));
        return beats[juce::jlimit (0, count - 1, syncIndex)];
    }

    const juce::StringArray& holdTimeChoices()
    {
        static const juce::StringArray choices { "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1/1" };
        return choices;
    }

    double holdTimeBeats (int index) noexcept
    {
        return grabSyncBeats (juce::jlimit (0, 8, index) + 1);
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
        layout.add (intParam (ParamID::tune, "Tune", -24, 24, 0, formatSemitonesWhole));
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
            layout.add (choiceParam (ids.shape, (name + " Shape").toRawUTF8(), lfoShapeChoices(), (int) LfoShape::sine));
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

        // How each LFO starts. Free with no fade, no offset and not inverted is 0.2's LFO.
        for (int i = 0; i < numLfos; ++i)
        {
            const auto& ids = ParamID::lfo[i];
            const juce::String name (lfoNames[i]);
            layout.add (choiceParam (ids.trig, (name + " Trigger").toRawUTF8(), { "Free", "Note", "Voice", "Once" }, (int) LfoTrig::free));
            layout.add (floatParam (ids.fade, (name + " Fade").toRawUTF8(), skewedRange (0.0f, 5000.0f, 0.1f, 500.0f), 0.0f, formatMs, parseMs));
            layout.add (floatParam (ids.phase, (name + " Phase").toRawUTF8(), Range (0.0f, 360.0f, 1.0f), 0.0f, formatDegrees));
            layout.add (boolParam (ids.invert, (name + " Invert").toRawUTF8(), false));
        }

        // NOTE ENVELOPE: a second envelope per note (rises over Attack, falls back over Decay) that
        // bends pitch, formant and grain by these amounts. All amounts 0 = nothing.
        layout.add (floatParam (ParamID::envAttack, "Note Env Attack", skewedRange (0.0f, 2000.0f, 0.1f, 100.0f), 0.0f, formatMs, parseMs));
        layout.add (floatParam (ParamID::envDecay, "Note Env Decay", skewedRange (1.0f, 5000.0f, 0.1f, 300.0f), 300.0f, formatMs, parseMs));
        layout.add (floatParam (ParamID::envPitch, "Note Env Pitch", Range (-24.0f, 24.0f, 0.01f), 0.0f, formatSemitones));
        layout.add (floatParam (ParamID::envFormant, "Note Env Formant", Range (-12.0f, 12.0f, 0.01f), 0.0f, formatSemitones));
        layout.add (intParam (ParamID::envGrain, "Note Env Grain", -15, 15, 0,
                              [] (int v, int) { return signedString ((float) v, 0) + (std::abs (v) == 1 ? " cycle" : " cycles"); }));
        layout.add (boolParam (ParamID::tapeStop, "Tape Stop", false));

        // KEYBOARD: bend range, and what the mod wheel, aftertouch and the expression pedal move.
        layout.add (intParam (ParamID::bendUp, "Bend Up", 0, 24, 2, [] (int v, int) { return juce::String (v) + " st"; }));
        layout.add (intParam (ParamID::bendDown, "Bend Down", 0, 24, 2, [] (int v, int) { return juce::String (v) + " st"; }));
        layout.add (floatParam (ParamID::vibRate, "Vibrato Rate", skewedRange (0.1f, 12.0f, 0.01f, 5.0f), 5.5f, formatHz));
        layout.add (floatParam (ParamID::vibDepth, "Vibrato Depth", Range (0.0f, 200.0f, 1.0f), 50.0f,
                                [] (float v, int) { return juce::String (juce::roundToInt (v)) + " ct"; }));

        const juce::StringArray destinations { "Off", "Vibrato", "Formant", "Grain", "Level" };
        const char* sourceNames[] = { "Wheel", "Aftertouch", "Pedal" };
        const int sourceDefaults[] = { (int) ModDest::vibrato, (int) ModDest::off, (int) ModDest::off };
        for (int i = 0; i < numModSources; ++i)
        {
            const juce::String name (sourceNames[i]);
            layout.add (choiceParam (ParamID::source[i].dest, (name + " Destination").toRawUTF8(), destinations, sourceDefaults[i]));
            layout.add (floatParam (ParamID::source[i].amount, (name + " Amount").toRawUTF8(), Range (-100.0f, 100.0f, 1.0f), 100.0f,
                                    [] (float v, int) { return signedString ((float) juce::roundToInt (v), 0) + "%"; }));
        }

        // WHEN A NOTE GRABS. Before Key with no Wait is 0.2: the slice ends Offset before the key.
        layout.add (choiceParam (ParamID::grabAt, "Grab At", { "Before Key", "At Key" }, 0));
        layout.add (floatParam (ParamID::wait, "Wait", skewedRange (0.0f, 2000.0f, 0.1f, 250.0f), 0.0f, formatMs, parseMs));
        layout.add (choiceParam (ParamID::waitSync, "Wait Sync", grabSyncChoices(), 0));
        layout.add (choiceParam (ParamID::offsetSync, "Offset Sync", grabSyncChoices(), 0));
        layout.add (choiceParam (ParamID::refreshSync, "Refresh Sync", lfoSyncChoices(), 0));

        // WHAT A NOTE GRABS. Everything here is off by default.
        layout.add (floatParam (ParamID::snap, "Snap", Range (0.0f, 100.0f, 0.1f), 0.0f,
                                [] (float v, int) { return v <= 0.0f ? juce::String ("Off") : formatMs (v, 0); },
                                [] (const juce::String& text) { return text.trim().equalsIgnoreCase ("off") ? 0.0f : parseMs (text); }));
        layout.add (floatParam (ParamID::threshold, "Threshold", Range (thresholdOffDb, -10.0f, 0.1f), thresholdOffDb,
                                [] (float v, int) { return v <= thresholdOffDb ? juce::String ("Off") : juce::String (v, 1) + " dB"; },
                                [] (const juce::String& text) { return text.trim().equalsIgnoreCase ("off") ? thresholdOffDb : text.trim().getFloatValue(); }));
        layout.add (floatParam (ParamID::maxWait, "Max Wait", skewedRange (0.0f, 2000.0f, 0.1f, 500.0f), 500.0f, formatMs, parseMs));
        layout.add (boolParam (ParamID::skipHiss, "Skip Hiss", false));
        layout.add (boolParam (ParamID::gate, "Gate", false));
        layout.add (boolParam (ParamID::gridGrabs, "Grid Grabs", false));
        layout.add (floatParam (ParamID::skipChance, "Skip", Range (0.0f, 100.0f, 1.0f), 0.0f, formatPercent));
        layout.add (floatParam (ParamID::feedback, "Feedback", Range (0.0f, 100.0f, 0.1f), 0.0f, formatPercent));

        // HOW NOTES ARE HELD, AND HOW MANY PLAY. The sustain pedal is on for new work and switched off
        // when an older project is loaded (0.2 ignored it).
        layout.add (boolParam (ParamID::sustainPedal, "Sustain Pedal", true));
        layout.add (choiceParam (ParamID::holdMode, "Hold Mode", { "Normal", "Latch", "On Grid", "Full" }, (int) HoldMode::normal));
        layout.add (choiceParam (ParamID::holdTime, "Hold Time", holdTimeChoices(), 6));
        layout.add (boolParam (ParamID::glideLegato, "Glide Legato", false));
        layout.add (boolParam (ParamID::glideRate, "Glide Per Octave", false));
        layout.add (boolParam (ParamID::polyGlide, "Poly Glide", false));
        layout.add (intParam (ParamID::voices, "Voices", 1, 8, 8, [] (int v, int) { return juce::String (v); }));

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
            lfo[(size_t) i] = Lfo { get (ids.on), get (ids.rate), get (ids.sync), get (ids.shape), get (ids.depth),
                                    get (ids.trig), get (ids.fade), get (ids.phase), get (ids.invert) };
        }
        for (int i = 0; i < numModSources; ++i)
            source[(size_t) i] = Source { get (ParamID::source[i].dest), get (ParamID::source[i].amount) };

        envAttack  = get (ParamID::envAttack);
        envDecay   = get (ParamID::envDecay);
        envPitch   = get (ParamID::envPitch);
        envFormant = get (ParamID::envFormant);
        envGrain   = get (ParamID::envGrain);
        tapeStop   = get (ParamID::tapeStop);
        bendUp     = get (ParamID::bendUp);
        bendDown   = get (ParamID::bendDown);
        vibRate    = get (ParamID::vibRate);
        vibDepth   = get (ParamID::vibDepth);

        grabAt      = get (ParamID::grabAt);
        wait        = get (ParamID::wait);
        waitSync    = get (ParamID::waitSync);
        offsetSync  = get (ParamID::offsetSync);
        refreshSync = get (ParamID::refreshSync);
        snap        = get (ParamID::snap);
        threshold   = get (ParamID::threshold);
        maxWait     = get (ParamID::maxWait);
        skipHiss    = get (ParamID::skipHiss);
        gate        = get (ParamID::gate);
        gridGrabs   = get (ParamID::gridGrabs);
        skipChance  = get (ParamID::skipChance);
        feedback    = get (ParamID::feedback);

        sustainPedal = get (ParamID::sustainPedal);
        holdMode     = get (ParamID::holdMode);
        holdTime     = get (ParamID::holdTime);
        glideLegato  = get (ParamID::glideLegato);
        glideRate    = get (ParamID::glideRate);
        polyGlide    = get (ParamID::polyGlide);
        voices       = get (ParamID::voices);
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
            { ParamID::autoGain, 0.0f },    // on for new work; older projects keep their level
            { ParamID::wheelDest, 0.0f },   // 0.2 ignored the mod wheel; it must not start a vibrato in an old project
            { ParamID::sustainPedal, 0.0f },   // 0.2 ignored the sustain pedal; an old project's notes must still end at the key
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
