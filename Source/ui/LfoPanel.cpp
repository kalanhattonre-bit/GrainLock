#include "LfoPanel.h"

namespace grainlock::ui
{
    LfoPage::LfoPage (juce::AudioProcessorValueTreeState& s, const ParamID::LfoIds& lfoIds)
        : state (s),
          ids (lfoIds),
          power (s, lfoIds.on, "On"),
          rate (s, lfoIds.rate, "Rate"),
          depth (s, lfoIds.depth, "Depth"),
          sync (s, lfoIds.sync, "Sync"),
          shape (*s.getParameter (lfoIds.shape), lfoShapeChoices(), paintLfoShapeIcon),
          trigger (s, lfoIds.trig, "Starts"),
          fade (s, lfoIds.fade, "Fade In"),
          phase (s, lfoIds.phase, "Phase"),
          invert (s, lfoIds.invert, "Invert")
    {
        for (auto* c : std::initializer_list<juce::Component*> { &power, &rate, &depth, &sync, &shape, &trigger, &fade, &phase, &invert })
            addAndMakeVisible (c);
    }

    juce::Rectangle<int> LfoPage::cell (int firstCell, int span) const
    {
        const float w = (float) getWidth() / 13.0f;
        const int x0 = juce::roundToInt (w * (float) firstCell);
        const int x1 = juce::roundToInt (w * (float) (firstCell + span));
        return juce::Rectangle<int> (x0, 0, x1 - x0, getHeight()).reduced (2, 0);
    }

    void LfoPage::resized()
    {
        power.setBounds (cell (0, 1));
        rate.setBounds (cell (1, 1));
        depth.setBounds (cell (2, 1));
        sync.setBounds (cell (3, 2));

        // Six shapes across three cells, with the name underneath like every other control.
        auto shapeArea = cell (5, 3);
        shapeCaption = shapeArea.removeFromBottom (15);
        shape.setBounds (shapeArea.withSizeKeepingCentre (shapeArea.getWidth() - 4, 24));

        trigger.setBounds (cell (8, 2));
        fade.setBounds (cell (10, 1));
        phase.setBounds (cell (11, 1));
        invert.setBounds (cell (12, 1));
    }

    void LfoPage::paint (juce::Graphics& g)
    {
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (12.0f, false, 0.06f));
        g.drawFittedText ("SHAPE", shapeCaption, juce::Justification::centred, 1, 0.8f);
    }

    void LfoPage::refresh()
    {
        const bool synced = juce::roundToInt (state.getRawParameterValue (ids.sync)->load()) != 0;

        // S&H and Random take their values per cycle: Phase does not move them.
        const auto shapeNow = (LfoShape) juce::roundToInt (state.getRawParameterValue (ids.shape)->load());
        const bool stepped = shapeNow == LfoShape::sampleHold || shapeNow == LfoShape::random;

        rate.setAlpha (synced ? 0.45f : 1.0f);                         // Sync picks the speed
        phase.setAlpha (stepped ? 0.45f : 1.0f);                       // every other shape is read at Phase
    }
}
