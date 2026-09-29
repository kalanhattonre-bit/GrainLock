#pragma once

#include "Theme.h"

namespace grainlock::ui
{
    /** Draws every standard JUCE widget GrainLock uses. No bitmaps: all vector drawing. */
    class GrainLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        GrainLookAndFeel();

        void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                               float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;

        void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                           int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
        juce::Font getComboBoxFont (juce::ComboBox&) override;
        void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

        void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

        juce::Font getPopupMenuFont() override;
        void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    };
}
