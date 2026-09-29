#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Theme.h"

namespace grainlock::ui
{
    /** A rotary slider dragged up/down (or right/left). Hold Ctrl (Cmd on a Mac) for fine steps. */
    class FineSlider final : public juce::Slider
    {
    public:
        FineSlider();

        std::function<void()> onHoverChange;

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void startedDragging() override { dragging = true; }
        void stoppedDragging() override { dragging = false; }

    private:
        bool dragging = false;   // only move the value inside a real drag (inside the host gesture)
        double dragProportion = 0.0;
        juce::Point<float> lastDragPosition;
    };

    /** A parameter knob with its name underneath. The name turns into the value while
        the knob is hovered or dragged. Double-click resets to the default. */
    class Knob final : public juce::Component
    {
    public:
        Knob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId,
              const juce::String& displayName, bool bipolar = false);

        void paint (juce::Graphics&) override;
        void resized() override;

        /** Shows the LFO's current push on this control (normalised 0..1 position), or hides it. */
        void setModulation (bool visible, float normalisedPosition);

        juce::RangedAudioParameter& getParameter() noexcept { return parameter; }

    private:
        juce::RangedAudioParameter& parameter;
        juce::String name;
        FineSlider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
        bool showingModulation = false;
        float lastModulation = -1.0f;
    };

    /** An on/off parameter drawn as a small pill switch with its name underneath. */
    class PillToggle final : public juce::Button
    {
    public:
        PillToggle (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, const juce::String& caption);

        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

    private:
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    };

    /** A choice parameter as a row of segments; click one to select it. */
    class SegmentedControl final : public juce::Component
    {
    public:
        /** Draws segment i's icon inside area in the given colour. Leave empty for text labels. */
        using IconPainter = std::function<void (juce::Graphics&, int index, juce::Rectangle<float> area, juce::Colour colour)>;

        SegmentedControl (juce::RangedAudioParameter& parameter, const juce::StringArray& labels,
                          IconPainter iconPainter = nullptr);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

        float textHeight = 12.0f;

    private:
        int segmentAt (juce::Point<float> position) const;

        juce::StringArray labels;
        IconPainter icons;
        int selected = 0;
        int hovered = -1;
        juce::ParameterAttachment attachment;
    };

    /** Draws one cycle of an LFO shape (sine, triangle, square, S&H) inside area. */
    void paintLfoShapeIcon (juce::Graphics&, int shapeIndex, juce::Rectangle<float> area, juce::Colour colour);

    /** The top-bar preset picker. It shows the current preset's name, and any pick from its menu
        loads, including the preset already shown (to throw away edits). */
    class PresetButton final : public juce::Component
    {
    public:
        std::function<juce::StringArray()> getNames;
        std::function<int()> getCurrentIndex;
        std::function<void (int)> onPick;

        void setDisplayedName (const juce::String& name);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override  { repaint(); }

    private:
        juce::String displayed;
    };

    /** A small chevron button for stepping through presets. */
    class ChevronButton final : public juce::Button
    {
    public:
        explicit ChevronButton (bool pointsRight);
        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

    private:
        bool right;
    };
}
