#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace grainlock::ui
{
    /** Every colour and font the interface uses. Change the look here and nowhere else. */
    struct Theme
    {
        // Surfaces, darkest to lightest.
        static inline const juce::Colour background   { 0xff151618 };
        static inline const juce::Colour display      { 0xff0f1012 };
        static inline const juce::Colour panel        { 0xff1d1e21 };
        static inline const juce::Colour panelRaised  { 0xff27282c };
        static inline const juce::Colour outline      { 0xff33353a };
        static inline const juce::Colour track        { 0xff3a3c42 };

        // Text.
        static inline const juce::Colour text         { 0xffece8e1 };
        static inline const juce::Colour textDim      { 0xff9a9ea6 };
        static inline const juce::Colour textFaint    { 0xff5d6169 };

        // The one accent colour, and the dark ink drawn on top of it.
        static inline const juce::Colour accent       { 0xfff5a524 };
        static inline const juce::Colour onAccent     { 0xff1a1206 };

        static juce::Colour accentAlpha (float alpha) { return accent.withAlpha (alpha); }

        static juce::String fontName()
        {
           #if JUCE_WINDOWS
            return "Segoe UI";
           #else
            return juce::Font::getDefaultSansSerifFontName();
           #endif
        }

        /** tracking is extra letter spacing as a proportion of the height (0.1 = airy capitals). */
        static juce::Font font (float height, bool bold = false, float tracking = 0.0f)
        {
            return juce::Font (juce::FontOptions (fontName(), height, bold ? juce::Font::bold : juce::Font::plain)
                                   .withKerningFactor (tracking));
        }

        // Base editor size; the window scales this whole layout from 75% to 200%.
        static constexpr int baseWidth = 780;
        static constexpr int baseHeight = 470;
        static constexpr float corner = 6.0f;
    };
}
