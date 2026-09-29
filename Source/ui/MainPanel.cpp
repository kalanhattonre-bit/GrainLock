#include "MainPanel.h"
#include "../PluginProcessor.h"

namespace grainlock::ui
{
    namespace
    {
        juce::RangedAudioParameter& param (juce::AudioProcessorValueTreeState& state, const char* id)
        {
            return *state.getParameter (id);
        }

        void paintLogo (juce::Graphics& g, juce::Rectangle<float> area)
        {
            // A loop (ring) holding one grain (dot): the loop the plugin locks onto.
            const float d = juce::jmin (area.getWidth(), area.getHeight());
            const auto ring = juce::Rectangle<float> (d, d).withCentre (area.getCentre()).reduced (2.0f);
            g.setColour (Theme::accent);
            g.drawEllipse (ring, 2.2f);

            const float angle = juce::MathConstants<float>::pi * 0.28f;
            const float r = ring.getWidth() * 0.5f;
            const auto grain = ring.getCentre() + juce::Point<float> (std::sin (angle), -std::cos (angle)) * r;
            g.setColour (Theme::background);
            g.fillEllipse (juce::Rectangle<float> (d * 0.42f, d * 0.42f).withCentre (grain));
            g.setColour (Theme::text);
            g.fillEllipse (juce::Rectangle<float> (d * 0.26f, d * 0.26f).withCentre (grain));
        }
    }

    MainPanel::MainPanel (GrainLockProcessor& p)
        : processor (p),
          state (p.apvts),
          captureMode (param (state, ParamID::captureMode), { "HOLD", "LIVE" }),
          grain       (state, ParamID::grainCycles, "Grain"),
          smooth      (state, ParamID::smooth, "Smooth"),
          offset      (state, ParamID::offset, "Offset"),
          refresh     (state, ParamID::refresh, "Refresh"),
          pitchLock   (state, ParamID::pitchLock, "Lock"),
          tune        (state, ParamID::tune, "Tune", true),
          fine        (state, ParamID::fine, "Fine", true),
          formant     (state, ParamID::formant, "Formant", true),
          glide       (state, ParamID::glide, "Glide"),
          mono        (state, ParamID::mono, "Mono"),
          mix         (state, ParamID::mix, "Mix"),
          gain        (state, ParamID::outGain, "Gain", true),
          dryWhenIdle (state, ParamID::dryWhenIdle, "Dry Idle"),
          attack      (state, ParamID::attack, "A"),
          decay       (state, ParamID::decay, "D"),
          sustain     (state, ParamID::sustain, "S"),
          release     (state, ParamID::release, "R"),
          velSens     (state, ParamID::velSens, "Vel"),
          lfoRate     (state, ParamID::lfoRate, "Rate"),
          lfoDepth    (state, ParamID::lfoDepth, "Depth"),
          lfoShape    (param (state, ParamID::lfoShape), { "Sine", "Triangle", "Square", "S&H" }, paintLfoShapeIcon),
          lfoTarget   (param (state, ParamID::lfoTarget), { "PITCH", "FORMANT", "GRAIN" })
    {
        for (auto* c : std::initializer_list<juce::Component*> {
                 &previousPreset, &nextPreset, &presetBox, &captureMode, &display,
                 &grain, &smooth, &offset, &refresh, &pitchLock,
                 &tune, &fine, &formant, &glide, &mono,
                 &mix, &gain, &dryWhenIdle,
                 &attack, &decay, &sustain, &release, &velSens,
                 &lfoRate, &lfoDepth, &lfoSync, &lfoShape, &lfoTarget, &keys })
            addAndMakeVisible (c);

        captureMode.textHeight = 12.5f;
        lfoTarget.textHeight = 11.0f;

        lfoSync.addItemList (lfoSyncChoices(), 1);
        lfoSyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, ParamID::lfoSync, lfoSync);

        presetBox.getNames = [this]
        {
            juce::StringArray names;
            for (int i = 0; i < processor.getNumPresets(); ++i)
                names.add (processor.getPresetName (i));
            return names;
        };
        presetBox.getCurrentIndex = [this] { return processor.getPresetIndex (processor.getCurrentPresetName()); };
        presetBox.onPick = [this] (int index) { processor.loadPreset (index); };
        previousPreset.onClick = [this] { stepPreset (-1); };
        nextPreset.onClick = [this] { stepPreset (1); };
        refreshPresetBox();

        setSize (Theme::baseWidth, Theme::baseHeight);
    }

    //==============================================================================
    void MainPanel::layoutRow (juce::Rectangle<int> area, std::initializer_list<juce::Component*> cells)
    {
        const int n = (int) cells.size();
        const float w = (float) area.getWidth() / (float) n;
        int i = 0;
        for (auto* c : cells)
        {
            const int x0 = area.getX() + juce::roundToInt (w * (float) i);
            const int x1 = area.getX() + juce::roundToInt (w * (float) (i + 1));
            c->setBounds (juce::Rectangle<int> (x0, area.getY(), x1 - x0, area.getHeight()).reduced (2, 0));
            ++i;
        }
    }

    void MainPanel::resized()
    {
        // Top bar
        previousPreset.setBounds (262, 10, 24, 24);
        presetBox.setBounds (290, 10, 200, 24);
        nextPreset.setBounds (494, 10, 24, 24);
        captureCaption = { 560, 10, 62, 24 };
        captureMode.setBounds (628, 10, 140, 24);

        display.setBounds (12, 50, 756, 150);

        sections = { Section { "FREEZE",   { 12, 206, 286, 104 } },
                     Section { "VOICE",    { 304, 206, 286, 104 } },
                     Section { "OUTPUT",   { 596, 206, 172, 104 } },
                     Section { "ENVELOPE", { 12, 314, 300, 94 } },
                     Section { "LFO",      { 318, 314, 450, 94 } } };

        auto body = [] (const Section& s) { return s.bounds.reduced (6, 4).withTrimmedTop (18); };

        layoutRow (body (sections[0]), { &grain, &smooth, &offset, &refresh, &pitchLock });
        layoutRow (body (sections[1]), { &tune, &fine, &formant, &glide, &mono });
        layoutRow (body (sections[2]), { &mix, &gain, &dryWhenIdle });
        layoutRow (body (sections[3]), { &attack, &decay, &sustain, &release, &velSens });

        auto lfoArea = body (sections[4]);
        layoutRow (lfoArea.removeFromLeft (132), { &lfoRate, &lfoDepth });
        lfoArea.removeFromLeft (10);
        lfoArea.removeFromRight (4);

        auto topRow = lfoArea.removeFromTop (lfoArea.getHeight() / 2);
        syncCaption = topRow.removeFromTop (12).withWidth (84);
        shapeCaption = syncCaption.withX (syncCaption.getRight() + 10).withWidth (topRow.getWidth() - 94);
        lfoSync.setBounds (topRow.removeFromLeft (84).withHeight (22));
        topRow.removeFromLeft (10);
        lfoShape.setBounds (topRow.withHeight (22));

        targetCaption = lfoArea.removeFromTop (12);
        lfoTarget.setBounds (lfoArea.withHeight (22));

        keys.setBounds (12, 414, 738, 20);   // stops short of the window's resize corner
    }

    void MainPanel::paint (juce::Graphics& g)
    {
        g.fillAll (Theme::background);

        // Wordmark
        paintLogo (g, { 16.0f, 11.0f, 22.0f, 22.0f });
        g.setColour (Theme::text);
        g.setFont (Theme::font (17.0f, true, 0.2f));
        g.drawText ("GRAINLOCK", juce::Rectangle<int> (46, 10, 200, 24), juce::Justification::centredLeft, false);

        g.setColour (Theme::textFaint);
        g.setFont (Theme::font (11.0f, true, 0.15f));
        g.drawText ("CAPTURE", captureCaption, juce::Justification::centredRight, false);

        for (const auto& s : sections)
        {
            const auto r = s.bounds.toFloat();
            g.setColour (Theme::panel);
            g.fillRoundedRectangle (r, Theme::corner);

            g.setColour (Theme::accent);
            g.fillRoundedRectangle (r.getX() + 10.0f, r.getY() + 9.0f, 3.0f, 10.0f, 1.5f);
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (11.5f, true, 0.2f));
            g.drawText (s.title, juce::Rectangle<float> (r.getX() + 18.0f, r.getY() + 6.0f, r.getWidth() - 24.0f, 16.0f),
                        juce::Justification::centredLeft, false);
        }

        g.setColour (Theme::textFaint);
        g.setFont (Theme::font (10.5f, true, 0.12f));
        g.drawText ("SYNC", syncCaption, juce::Justification::centredLeft, false);
        g.drawText ("SHAPE", shapeCaption, juce::Justification::centredLeft, false);
        g.drawText ("TARGET", targetCaption, juce::Justification::centredLeft, false);
    }

    //==============================================================================
    float MainPanel::plainValue (const char* id) const
    {
        return state.getRawParameterValue (id)->load();
    }

    juce::String MainPanel::statusText() const
    {
        const bool live = plainValue (ParamID::captureMode) >= 0.5f;
        const bool lockOn = plainValue (ParamID::pitchLock) >= 0.5f;
        const int cycles = juce::roundToInt (plainValue (ParamID::grainCycles));

        juce::String text = live ? "LIVE  /  REFRESH " + state.getParameter (ParamID::refresh)->getCurrentValueAsText().toUpperCase()
                                 : juce::String ("HOLD");
        text << "  /  " << cycles << (cycles == 1 ? " CYCLE" : " CYCLES");
        text << (lockOn ? "  /  PITCH LOCKED" : "  /  LOOP " + juce::String (cycles) + "X LONGER");
        return text;
    }

    void MainPanel::tick (const ScopeFrame* frame)
    {
        if (frame != nullptr)
            lastFrame = *frame;

        display.update (frame, statusText());
        keys.setHeldNotes (lastFrame.heldNotes);

        // Controls that do nothing in the current mode fade back (they still work).
        glide.setAlpha (plainValue (ParamID::mono) >= 0.5f ? 1.0f : 0.45f);
        refresh.setAlpha (plainValue (ParamID::captureMode) >= 0.5f ? 1.0f : 0.45f);
        lfoRate.setAlpha (juce::roundToInt (plainValue (ParamID::lfoSync)) == 0 ? 1.0f : 0.45f);

        // A dot on the knob the LFO is moving, riding at the modulated position.
        const bool lfoOn = plainValue (ParamID::lfoDepth) > 0.0f;
        const int target = juce::roundToInt (plainValue (ParamID::lfoTarget));
        auto modulate = [&] (Knob& knob, int targetIndex, float swingFraction)
        {
            auto& p = knob.getParameter();
            knob.setModulation (lfoOn && target == targetIndex, p.getValue() + lastFrame.lfoValue * swingFraction);
        };
        modulate (fine, (int) LfoTarget::pitch, lfoPitchRangeSemitones * 100.0f / 200.0f);
        modulate (formant, (int) LfoTarget::formant, lfoFormantRangeSemitones / 24.0f);
        modulate (grain, (int) LfoTarget::grainCycles, lfoCyclesRange / (float) (maxCycles - minCycles));

        const auto name = processor.getCurrentPresetName();
        if (name != shownPresetName)
            refreshPresetBox();
    }

    //==============================================================================
    void MainPanel::refreshPresetBox()
    {
        shownPresetName = processor.getCurrentPresetName();
        presetBox.setDisplayedName (shownPresetName);
    }

    void MainPanel::stepPreset (int delta)
    {
        const int count = processor.getNumPresets();
        if (count <= 0)
            return;

        const int current = juce::jmax (0, processor.getPresetIndex (processor.getCurrentPresetName()));
        processor.loadPreset ((current + delta + count) % count);
    }
}
