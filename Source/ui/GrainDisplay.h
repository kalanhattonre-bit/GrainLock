#pragma once

#include "Theme.h"
#include "../dsp/ScopeFrame.h"

namespace grainlock::ui
{
    /** One loop of the most recent voice's grain, with the seam crossfade shaded and
        the sounding notes shown as chips. Fed about 30 times a second by the editor. */
    class GrainDisplay final : public juce::Component
    {
    public:
        /** Call once per UI tick. frame is null when no new frame arrived this tick. */
        void update (const ScopeFrame* frame, const juce::String& statusText);

        void paint (juce::Graphics&) override;

    private:
        void paintSeam (juce::Graphics&, juce::Rectangle<float> area) const;
        void paintWave (juce::Graphics&, juce::Rectangle<float> area) const;
        void paintChips (juce::Graphics&, juce::Rectangle<float> header) const;

        ScopeFrame latest;
        juce::String status;
        float displayGain = 1.0f;
        float waveAlpha = 0.0f;
    };

    /** A thin keyboard across the whole MIDI range that lights up held notes. Display only. */
    class KeyStrip final : public juce::Component
    {
    public:
        void setHeldNotes (const std::array<juce::uint64, 2>& held);
        void paint (juce::Graphics&) override;

    private:
        std::array<juce::uint64, 2> heldNotes {};
    };
}
