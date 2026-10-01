#include "Controls.h"

#include <cmath>

namespace grainlock::ui
{
    //==============================================================================
    FineSlider::FineSlider()
        : juce::Slider (juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox)
    {
        setScrollWheelEnabled (true);
        setPopupMenuEnabled (false);
    }

    void FineSlider::mouseDown (const juce::MouseEvent& e)
    {
        dragProportion = valueToProportionOfLength (getValue());
        lastDragPosition = e.position;
        juce::Slider::mouseDown (e);   // starts the host automation gesture
    }

    void FineSlider::mouseDrag (const juce::MouseEvent& e)
    {
        // Replaces Slider's own drag so Ctrl can switch to fine steps mid-drag without a jump.
        // No drag was started for an Alt+click reset, so nothing may move the value then.
        if (! dragging || ! isEnabled() || e.mods.isPopupMenu())
            return;

        const auto delta = e.position - lastDragPosition;
        lastDragPosition = e.position;

        const bool fine = e.mods.isCtrlDown() || e.mods.isCommandDown();
        const double pixelsForFullRange = fine ? 1600.0 : 200.0;

        dragProportion = juce::jlimit (0.0, 1.0, dragProportion + (double) (delta.x - delta.y) / pixelsForFullRange);
        setValue (proportionOfLengthToValue (dragProportion), juce::sendNotificationSync);
    }

    void FineSlider::mouseEnter (const juce::MouseEvent& e)
    {
        juce::Slider::mouseEnter (e);
        if (onHoverChange)
            onHoverChange();
    }

    void FineSlider::mouseExit (const juce::MouseEvent& e)
    {
        juce::Slider::mouseExit (e);
        if (onHoverChange)
            onHoverChange();
    }

    //==============================================================================
    Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId,
                const juce::String& displayName, bool bipolar)
        : parameter (*state.getParameter (parameterId)), name (displayName)
    {
        slider.getProperties().set ("bipolar", bipolar);
        slider.onHoverChange = [this] { repaint(); };
        slider.onValueChange = [this] { repaint(); };
        slider.onDragStart = [this] { repaint(); };
        slider.onDragEnd = [this] { repaint(); };
        addAndMakeVisible (slider);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, parameterId, slider);
    }

    void Knob::resized()
    {
        auto area = getLocalBounds();
        area.removeFromBottom (15);
        const int size = juce::jmin (area.getWidth(), area.getHeight());
        slider.setBounds (area.withSizeKeepingCentre (size, size));
    }

    void Knob::paint (juce::Graphics& g)
    {
        const bool hot = slider.isMouseOverOrDragging();
        const auto labelArea = getLocalBounds().removeFromBottom (15).toFloat();

        if (hot)
        {
            g.setColour (Theme::accent);
            g.setFont (Theme::font (12.5f, true));
            g.drawFittedText (slider.getTextFromValue (slider.getValue()), labelArea.toNearestInt(),
                              juce::Justification::centred, 1, 0.8f);
        }
        else
        {
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (12.0f, false, 0.06f));
            g.drawFittedText (name.toUpperCase(), labelArea.toNearestInt(), juce::Justification::centred, 1, 0.8f);
        }
    }

    void Knob::setModulation (bool visible, float normalisedPosition)
    {
        if (! visible)
        {
            if (showingModulation)
            {
                showingModulation = false;
                slider.getProperties().remove ("modPos");
                slider.repaint();
            }
            return;
        }

        const float pos = juce::jlimit (0.0f, 1.0f, normalisedPosition);
        if (showingModulation && std::abs (pos - lastModulation) < 0.002f)
            return;

        showingModulation = true;
        lastModulation = pos;
        slider.getProperties().set ("modPos", pos);
        slider.repaint();
    }

    //==============================================================================
    PillToggle::PillToggle (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, const juce::String& caption)
        : juce::Button (caption)
    {
        setClickingTogglesState (true);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, parameterId, *this);
    }

    void PillToggle::paintButton (juce::Graphics& g, bool isMouseOver, bool)
    {
        auto area = getLocalBounds().toFloat();
        const auto labelArea = area.removeFromBottom (15.0f);

        const bool on = getToggleState();
        const float w = juce::jmin (30.0f, area.getWidth() - 4.0f);
        const float h = w * 0.52f;
        const auto pill = juce::Rectangle<float> (w, h).withCentre (area.getCentre());

        g.setColour (on ? Theme::accent : Theme::track);
        g.fillRoundedRectangle (pill, h * 0.5f);
        if (isMouseOver)
        {
            g.setColour (Theme::text.withAlpha (0.35f));
            g.drawRoundedRectangle (pill.expanded (1.5f), h * 0.5f + 1.5f, 1.0f);
        }

        const float d = h - 4.0f;
        const auto thumb = juce::Rectangle<float> (d, d).withCentre ({ on ? pill.getRight() - h * 0.5f : pill.getX() + h * 0.5f,
                                                                       pill.getCentreY() });
        g.setColour (on ? Theme::onAccent : Theme::textDim);
        g.fillEllipse (thumb);

        g.setColour (on ? Theme::text : Theme::textDim);
        g.setFont (Theme::font (12.0f, false, 0.06f));
        g.drawFittedText (getButtonText().toUpperCase(), labelArea.toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }

    //==============================================================================
    SegmentedControl::SegmentedControl (juce::RangedAudioParameter& parameter, const juce::StringArray& segmentLabels,
                                        IconPainter iconPainter)
        : labels (segmentLabels),
          icons (std::move (iconPainter)),
          attachment (parameter, [this] (float value)
                      {
                          selected = juce::roundToInt (value);
                          repaint();
                      })
    {
        attachment.sendInitialUpdate();
    }

    int SegmentedControl::segmentAt (juce::Point<float> position) const
    {
        if (labels.isEmpty() || getWidth() <= 0)
            return -1;
        return juce::jlimit (0, labels.size() - 1, (int) (position.x * (float) labels.size() / (float) getWidth()));
    }

    void SegmentedControl::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const int n = juce::jmax (1, labels.size());
        const float segmentWidth = bounds.getWidth() / (float) n;

        g.setColour (Theme::panelRaised);
        g.fillRoundedRectangle (bounds, 4.0f);

        for (int i = 0; i < n; ++i)
        {
            const auto seg = juce::Rectangle<float> (bounds.getX() + segmentWidth * (float) i, bounds.getY(),
                                                     segmentWidth, bounds.getHeight());
            const bool on = i == selected;

            if (on)
            {
                g.setColour (Theme::accent);
                g.fillRoundedRectangle (seg.reduced (2.0f), 3.0f);
            }
            else if (i == hovered)
            {
                g.setColour (Theme::text.withAlpha (0.07f));
                g.fillRoundedRectangle (seg.reduced (2.0f), 3.0f);
            }

            const auto ink = on ? Theme::onAccent : (i == hovered ? Theme::text : Theme::textDim);

            if (icons)
            {
                icons (g, i, seg.reduced (seg.getWidth() * 0.22f, seg.getHeight() * 0.28f), ink);
            }
            else
            {
                g.setColour (ink);
                g.setFont (Theme::font (textHeight, on, 0.08f));
                g.drawFittedText (labels[i], seg.toNearestInt(), juce::Justification::centred, 1, 0.7f);
            }
        }

        g.setColour (Theme::outline);
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
    }

    void SegmentedControl::mouseDown (const juce::MouseEvent& e)
    {
        const int index = segmentAt (e.position);
        if (index >= 0 && index != selected)
            attachment.setValueAsCompleteGesture ((float) index);
    }

    void SegmentedControl::mouseMove (const juce::MouseEvent& e)
    {
        const int index = segmentAt (e.position);
        if (index != hovered)
        {
            hovered = index;
            repaint();
        }
    }

    void SegmentedControl::mouseExit (const juce::MouseEvent&)
    {
        hovered = -1;
        repaint();
    }

    //==============================================================================
    void paintLfoShapeIcon (juce::Graphics& g, int shapeIndex, juce::Rectangle<float> area, juce::Colour colour)
    {
        juce::Path p;
        const float x0 = area.getX(), w = area.getWidth();
        const float mid = area.getCentreY(), amp = area.getHeight() * 0.5f;

        switch (shapeIndex)
        {
            case 0: // sine
                for (int i = 0; i <= 24; ++i)
                {
                    const float t = (float) i / 24.0f;
                    const float y = mid - amp * std::sin (t * juce::MathConstants<float>::twoPi);
                    if (i == 0) p.startNewSubPath (x0, y);
                    else        p.lineTo (x0 + t * w, y);
                }
                break;

            case 1: // triangle
                p.startNewSubPath (x0, mid);
                p.lineTo (x0 + w * 0.25f, mid - amp);
                p.lineTo (x0 + w * 0.75f, mid + amp);
                p.lineTo (x0 + w, mid);
                break;

            case 2: // square
                p.startNewSubPath (x0, mid + amp);
                p.lineTo (x0, mid - amp);
                p.lineTo (x0 + w * 0.5f, mid - amp);
                p.lineTo (x0 + w * 0.5f, mid + amp);
                p.lineTo (x0 + w, mid + amp);
                p.lineTo (x0 + w, mid - amp);
                break;

            case 3: // sample & hold
            {
                const float levels[] = { 0.3f, -0.8f, 0.9f, -0.2f };
                p.startNewSubPath (x0, mid - amp * levels[0]);
                for (int i = 0; i < 4; ++i)
                {
                    const float xa = x0 + w * (float) i / 4.0f;
                    const float xb = x0 + w * (float) (i + 1) / 4.0f;
                    p.lineTo (xa, mid - amp * levels[i]);
                    p.lineTo (xb, mid - amp * levels[i]);
                }
                break;
            }

            case 4: // saw: falls (Invert makes it rise)
                p.startNewSubPath (x0, mid + amp);
                p.lineTo (x0, mid - amp);
                p.lineTo (x0 + w, mid + amp);
                break;

            default: // random: a smooth wander from one value to the next
            {
                const float levels[] = { 0.1f, 0.8f, -0.5f, 0.4f, -0.9f, 0.2f };
                const float step = w / 5.0f;
                p.startNewSubPath (x0, mid - amp * levels[0]);
                for (int i = 1; i < 6; ++i)
                {
                    const float xa = x0 + step * (float) (i - 1), xb = x0 + step * (float) i;
                    const float ya = mid - amp * levels[i - 1], yb = mid - amp * levels[i];
                    p.cubicTo (xa + step * 0.5f, ya, xb - step * 0.5f, yb, xb, yb);
                }
                break;
            }
        }

        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    }

    //==============================================================================
    ChoiceBox::ChoiceBox (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, const juce::String& caption)
        : name (caption)
    {
        if (auto* choices = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (parameterId)))
            box.addItemList (choices->choices, 1);
        addAndMakeVisible (box);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, parameterId, box);
    }

    void ChoiceBox::resized()
    {
        auto area = getLocalBounds();
        area.removeFromBottom (15);
        box.setBounds (area.withSizeKeepingCentre (juce::jmax (20, area.getWidth() - 4), 24));
    }

    void ChoiceBox::paint (juce::Graphics& g)
    {
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (12.0f, false, 0.06f));
        g.drawFittedText (name.toUpperCase(), getLocalBounds().removeFromBottom (15), juce::Justification::centred, 1, 0.8f);
    }

    //==============================================================================
    TabStrip::TabStrip (const juce::StringArray& tabNames, bool withLights)
        : names (tabNames), lights (withLights), active ((size_t) tabNames.size(), false)
    {
    }

    void TabStrip::setSelected (int index)
    {
        index = juce::jlimit (0, juce::jmax (0, names.size() - 1), index);
        if (index != selected)
        {
            selected = index;
            repaint();
        }
    }

    void TabStrip::setActive (int index, bool isOn)
    {
        if (juce::isPositiveAndBelow (index, names.size()) && active[(size_t) index] != isOn)
        {
            active[(size_t) index] = isOn;
            repaint();
        }
    }

    int TabStrip::tabAt (juce::Point<float> position) const
    {
        if (getWidth() <= 0 || names.isEmpty())
            return -1;
        return juce::jlimit (0, names.size() - 1, (int) (position.x * (float) names.size() / (float) getWidth()));
    }

    void TabStrip::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        const int count = juce::jmax (1, names.size());
        const float tabWidth = bounds.getWidth() / (float) count;

        for (int i = 0; i < names.size(); ++i)
        {
            const auto tab = juce::Rectangle<float> (bounds.getX() + tabWidth * (float) i, bounds.getY(),
                                                     tabWidth, bounds.getHeight()).reduced (2.0f, 0.0f);
            const bool isSelected = i == selected;

            g.setColour (isSelected ? Theme::panelRaised : (i == hovered ? Theme::panelRaised.withAlpha (0.5f) : Theme::panel));
            g.fillRoundedRectangle (tab, 4.0f);
            if (isSelected)
            {
                g.setColour (Theme::accent);
                g.fillRect (juce::Rectangle<float> (tab.getX() + 6.0f, tab.getBottom() - 2.0f, tab.getWidth() - 12.0f, 2.0f));
            }

            float textLeft = 10.0f;
            if (lights)
            {
                // Amber while this tab's thing is running.
                const bool isOn = active[(size_t) i];
                const float d = 7.0f;
                const auto led = juce::Rectangle<float> (d, d).withCentre ({ tab.getX() + 12.0f, tab.getCentreY() });
                g.setColour (isOn ? Theme::accent : Theme::track);
                g.fillEllipse (led);
                if (isOn)
                {
                    g.setColour (Theme::accentAlpha (0.25f));
                    g.fillEllipse (led.expanded (3.0f));
                }
                textLeft = 22.0f;
            }

            g.setColour (isSelected ? Theme::text : Theme::textDim);
            g.setFont (Theme::font (11.0f, isSelected, 0.12f));
            if (lights)
                g.drawText (names[i], tab.withTrimmedLeft (textLeft), juce::Justification::centredLeft, false);
            else
                g.drawText (names[i], tab, juce::Justification::centred, false);
        }
    }

    void TabStrip::mouseDown (const juce::MouseEvent& e)
    {
        const int index = tabAt (e.position);
        if (index >= 0 && index != selected)
        {
            setSelected (index);
            if (onSelect)
                onSelect (index);
        }
    }

    void TabStrip::mouseMove (const juce::MouseEvent& e)
    {
        const int index = tabAt (e.position);
        if (index != hovered)
        {
            hovered = index;
            repaint();
        }
    }

    void TabStrip::mouseExit (const juce::MouseEvent&)
    {
        hovered = -1;
        repaint();
    }

    //==============================================================================
    void PresetButton::setDisplayedName (const juce::String& name)
    {
        if (name != displayed)
        {
            displayed = name;
            repaint();
        }
    }

    void PresetButton::paint (juce::Graphics& g)
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool over = isMouseOver (true);

        g.setColour (Theme::panelRaised);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (over ? Theme::accentAlpha (0.5f) : Theme::outline);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        const float h = r.getHeight();
        const float cx = r.getRight() - h * 0.5f;
        const float cy = r.getCentreY();
        const float s = h * 0.13f;
        juce::Path arrow;
        arrow.addTriangle (cx - s * 1.3f, cy - s * 0.6f, cx + s * 1.3f, cy - s * 0.6f, cx, cy + s * 0.9f);
        g.setColour (Theme::accent);
        g.fillPath (arrow);

        g.setColour (Theme::text);
        g.setFont (Theme::font (13.5f));
        g.drawFittedText (displayed, r.reduced (h, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }

    void PresetButton::mouseDown (const juce::MouseEvent&)
    {
        const auto names = getNames ? getNames() : juce::StringArray();
        const int current = getCurrentIndex ? getCurrentIndex() : -1;

        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        for (int i = 0; i < names.size(); ++i)
            menu.addItem (i + 1, names[i], true, i == current);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                                      .withMinimumWidth (getWidth())
                                                      .withMaximumNumColumns (1),
                            [safe = juce::Component::SafePointer<PresetButton> (this)] (int result)
                            {
                                if (safe != nullptr && result > 0 && safe->onPick)
                                    safe->onPick (result - 1);
                            });
    }

    //==============================================================================
    ChevronButton::ChevronButton (bool pointsRight)
        : juce::Button (pointsRight ? "Next preset" : "Previous preset"), right (pointsRight)
    {
    }

    void ChevronButton::paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown)
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (isButtonDown ? Theme::accentAlpha (0.25f) : Theme::panelRaised);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (isMouseOver ? Theme::accentAlpha (0.5f) : Theme::outline);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        const auto c = r.getCentre();
        const float s = r.getHeight() * 0.16f;
        juce::Path chevron;
        const float dir = right ? 1.0f : -1.0f;
        chevron.startNewSubPath (c.x - dir * s * 0.6f, c.y - s);
        chevron.lineTo (c.x + dir * s * 0.6f, c.y);
        chevron.lineTo (c.x - dir * s * 0.6f, c.y + s);
        g.setColour (isMouseOver ? Theme::accent : Theme::text);
        g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}
