#include "GrainLookAndFeel.h"

#include <cmath>

namespace grainlock::ui
{
    GrainLookAndFeel::GrainLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, Theme::background);

        setColour (juce::PopupMenu::backgroundColourId, Theme::panelRaised);
        setColour (juce::PopupMenu::textColourId, Theme::text);
        setColour (juce::PopupMenu::headerTextColourId, Theme::textDim);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::accentAlpha (0.18f));
        setColour (juce::PopupMenu::highlightedTextColourId, Theme::accent);

        setColour (juce::ComboBox::backgroundColourId, Theme::panelRaised);
        setColour (juce::ComboBox::outlineColourId, Theme::outline);
        setColour (juce::ComboBox::textColourId, Theme::text);
        setColour (juce::ComboBox::arrowColourId, Theme::accent);
        setColour (juce::ComboBox::focusedOutlineColourId, Theme::accentAlpha (0.6f));

        setColour (juce::Label::textColourId, Theme::text);
        setColour (juce::Slider::rotarySliderFillColourId, Theme::accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, Theme::track);
    }

    void GrainLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                             float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
    {
        const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (1.0f);
        const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
        if (size <= 4.0f)
            return;

        const auto centre = bounds.getCentre();
        const float ringThickness = juce::jmax (2.0f, size * 0.075f);
        const float ringRadius = size * 0.5f - ringThickness * 0.5f - 1.0f;
        const bool enabled = slider.isEnabled();
        const bool hot = enabled && slider.isMouseOverOrDragging();
        const bool bipolar = (bool) slider.getProperties()["bipolar"];
        const auto stroke = juce::PathStrokeType (ringThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (Theme::track);
        g.strokePath (track, stroke);

        // Unipolar knobs fill from the left stop; bipolar ones (tune, formant, gain) from twelve o'clock.
        const float valueAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const float originAngle = bipolar ? 0.5f * (rotaryStartAngle + rotaryEndAngle) : rotaryStartAngle;
        if (std::abs (valueAngle - originAngle) > 0.01f)
        {
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f,
                               juce::jmin (originAngle, valueAngle), juce::jmax (originAngle, valueAngle), true);
            g.setColour (enabled ? Theme::accent : Theme::textFaint);
            g.strokePath (arc, stroke);
        }

        // Body and pointer.
        const float bodyRadius = ringRadius - ringThickness * 0.5f - size * 0.07f;
        const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);
        g.setColour (Theme::panelRaised);
        g.fillEllipse (body);
        g.setColour (hot ? Theme::accentAlpha (0.55f) : Theme::outline);
        g.drawEllipse (body, 1.0f);

        const juce::Point<float> direction (std::sin (valueAngle), -std::cos (valueAngle));
        g.setColour (enabled ? Theme::text : Theme::textFaint);
        g.drawLine (juce::Line<float> (centre + direction * (bodyRadius * 0.3f), centre + direction * (bodyRadius * 0.88f)),
                    juce::jmax (1.5f, size * 0.05f));

        // Where the LFO is pushing this control right now.
        const auto& props = slider.getProperties();
        if (props.contains ("modPos"))
        {
            const float modPos = juce::jlimit (0.0f, 1.0f, (float) props["modPos"]);
            const float angle = rotaryStartAngle + modPos * (rotaryEndAngle - rotaryStartAngle);
            const auto dot = centre + juce::Point<float> (std::sin (angle), -std::cos (angle)) * ringRadius;
            const float r = ringThickness * 0.85f;
            g.setColour (Theme::background);
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f + 2.0f, r * 2.0f + 2.0f).withCentre (dot));
            g.setColour (Theme::text);
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (dot));
        }
    }

    void GrainLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
    {
        const auto r = juce::Rectangle<float> ((float) width, (float) height).reduced (0.5f);
        g.setColour (Theme::panelRaised);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (box.isMouseOver (true) ? Theme::accentAlpha (0.5f) : Theme::outline);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        const float h = (float) height;
        const float cx = (float) width - h * 0.5f;
        const float cy = h * 0.5f;
        const float s = h * 0.13f;
        juce::Path arrow;
        arrow.addTriangle (cx - s * 1.3f, cy - s * 0.6f, cx + s * 1.3f, cy - s * 0.6f, cx, cy + s * 0.9f);
        g.setColour (box.isEnabled() ? Theme::accent : Theme::textFaint);
        g.fillPath (arrow);
    }

    juce::Font GrainLookAndFeel::getComboBoxFont (juce::ComboBox& box)
    {
        return Theme::font (juce::jmin (13.0f, (float) box.getHeight() * 0.56f));
    }

    void GrainLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
    {
        label.setBounds (6, 1, box.getWidth() - box.getHeight() - 6, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    void GrainLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
    {
        // Three short diagonal strokes in the corner, amber while it is being used.
        g.setColour (isMouseOver || isMouseDragging ? Theme::accent : Theme::textFaint);
        const float size = (float) juce::jmin (w, h);
        for (int i = 1; i <= 3; ++i)
        {
            const float d = size * 0.28f * (float) i;
            g.drawLine ((float) w - d, (float) h - 2.0f, (float) w - 2.0f, (float) h - d, 1.2f);
        }
    }

    juce::Font GrainLookAndFeel::getPopupMenuFont()
    {
        return Theme::font (14.0f);
    }

    void GrainLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
    {
        g.fillAll (Theme::panelRaised);
        g.setColour (Theme::outline);
        g.drawRect (0, 0, width, height, 1);
    }
}
