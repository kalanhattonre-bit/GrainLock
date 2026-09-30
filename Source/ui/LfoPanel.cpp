#include "LfoPanel.h"

namespace grainlock::ui
{
    LfoPage::LfoPage (juce::AudioProcessorValueTreeState& s, const ParamID::LfoIds& lfoIds)
        : state (s),
          ids (lfoIds),
          power (s, lfoIds.on, "On"),
          rate (s, lfoIds.rate, "Rate"),
          depth (s, lfoIds.depth, "Depth"),
          shape (*s.getParameter (lfoIds.shape), { "Sine", "Triangle", "Square", "S&H" }, paintLfoShapeIcon)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &power, &rate, &depth, &sync, &shape })
            addAndMakeVisible (c);

        sync.addItemList (lfoSyncChoices(), 1);
        syncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, ids.sync, sync);
    }

    void LfoPage::resized()
    {
        auto area = getLocalBounds();

        power.setBounds (area.removeFromLeft (56).reduced (2, 0));
        rate.setBounds (area.removeFromLeft (64).reduced (2, 0));
        depth.setBounds (area.removeFromLeft (64).reduced (2, 0));
        area.removeFromLeft (10);

        // Sync and shape side by side, centred on the knob row.
        auto row = area.withSizeKeepingCentre (area.getWidth(), 36).translated (0, -6);
        auto captions = row.removeFromTop (13);
        syncCaption = captions.removeFromLeft (84);
        captions.removeFromLeft (10);
        shapeCaption = captions;

        sync.setBounds (row.removeFromLeft (84).withHeight (22));
        row.removeFromLeft (10);
        shape.setBounds (row.withHeight (22));
    }

    void LfoPage::paint (juce::Graphics& g)
    {
        g.setColour (Theme::textFaint);
        g.setFont (Theme::font (10.5f, true, 0.12f));
        g.drawText ("SYNC", syncCaption, juce::Justification::centredLeft, false);
        g.drawText ("SHAPE", shapeCaption, juce::Justification::centredLeft, false);
    }

    void LfoPage::refresh()
    {
        const bool synced = juce::roundToInt (state.getRawParameterValue (ids.sync)->load()) != 0;
        rate.setAlpha (synced ? 0.45f : 1.0f);
    }

    //==============================================================================
    void LfoTabs::setSelected (int index)
    {
        index = juce::jlimit (0, numLfos - 1, index);
        if (index != selected)
        {
            selected = index;
            repaint();
        }
    }

    void LfoTabs::setActive (int index, bool isOn)
    {
        if (juce::isPositiveAndBelow (index, numLfos) && active[(size_t) index] != isOn)
        {
            active[(size_t) index] = isOn;
            repaint();
        }
    }

    int LfoTabs::tabAt (juce::Point<float> position) const
    {
        if (getWidth() <= 0)
            return -1;
        return juce::jlimit (0, numLfos - 1, (int) (position.x * (float) numLfos / (float) getWidth()));
    }

    void LfoTabs::paint (juce::Graphics& g)
    {
        static const char* names[] = { "PITCH", "FORMANT", "GRAIN" };
        const auto bounds = getLocalBounds().toFloat();
        const float tabWidth = bounds.getWidth() / (float) numLfos;

        for (int i = 0; i < numLfos; ++i)
        {
            const auto tab = juce::Rectangle<float> (bounds.getX() + tabWidth * (float) i, bounds.getY(),
                                                     tabWidth, bounds.getHeight()).reduced (2.0f, 0.0f);
            const bool isSelected = i == selected;
            const bool isOn = active[(size_t) i];

            g.setColour (isSelected ? Theme::panelRaised : (i == hovered ? Theme::panelRaised.withAlpha (0.5f) : Theme::panel));
            g.fillRoundedRectangle (tab, 4.0f);
            if (isSelected)
            {
                g.setColour (Theme::accent);
                g.fillRect (juce::Rectangle<float> (tab.getX() + 6.0f, tab.getBottom() - 2.0f, tab.getWidth() - 12.0f, 2.0f));
            }

            // Power light: amber while this LFO is running.
            const float d = 7.0f;
            const auto led = juce::Rectangle<float> (d, d).withCentre ({ tab.getX() + 12.0f, tab.getCentreY() });
            g.setColour (isOn ? Theme::accent : Theme::track);
            g.fillEllipse (led);
            if (isOn)
            {
                g.setColour (Theme::accentAlpha (0.25f));
                g.fillEllipse (led.expanded (3.0f));
            }

            g.setColour (isSelected ? Theme::text : Theme::textDim);
            g.setFont (Theme::font (11.0f, isSelected, 0.12f));
            g.drawText (names[i], tab.withTrimmedLeft (22.0f), juce::Justification::centredLeft, false);
        }
    }

    void LfoTabs::mouseDown (const juce::MouseEvent& e)
    {
        const int index = tabAt (e.position);
        if (index >= 0 && index != selected)
        {
            setSelected (index);
            if (onSelect)
                onSelect (index);
        }
    }

    void LfoTabs::mouseMove (const juce::MouseEvent& e)
    {
        const int index = tabAt (e.position);
        if (index != hovered)
        {
            hovered = index;
            repaint();
        }
    }

    void LfoTabs::mouseExit (const juce::MouseEvent&)
    {
        hovered = -1;
        repaint();
    }
}
