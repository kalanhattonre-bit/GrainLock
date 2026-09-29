#include "GrainDisplay.h"
#include "../Parameters.h"

#include <cmath>

namespace grainlock::ui
{
    void GrainDisplay::update (const ScopeFrame* frame, const juce::String& statusText)
    {
        bool changed = statusText != status;
        status = statusText;

        if (frame != nullptr)
        {
            // When the last note ends, keep drawing its loop while it fades; take everything else.
            if (frame->hasWave)
            {
                latest = *frame;
            }
            else
            {
                latest.notes = frame->notes;
                latest.numNotes = frame->numNotes;
                latest.focusNote = frame->focusNote;
                latest.seamFraction = frame->seamFraction;
                latest.lfoValue = frame->lfoValue;
                latest.lfoTarget = frame->lfoTarget;
                latest.live = frame->live;
                latest.heldNotes = frame->heldNotes;
                latest.hasWave = false;
            }
            changed = true;

            if (latest.hasWave)
            {
                float peak = 0.0f;
                for (float v : latest.wave)
                    peak = std::max (peak, std::abs (v));

                // Ease toward a gain that fills ~85% of the height, so quiet grains stay readable.
                const float target = 0.85f / std::max (peak, 0.02f);
                displayGain += 0.25f * (target - displayGain);
            }
        }

        // Fade in while a note sounds, fade the last loop out after it stops.
        const float targetAlpha = latest.hasWave ? 1.0f : 0.0f;
        if (std::abs (targetAlpha - waveAlpha) > 0.001f)
        {
            waveAlpha += (targetAlpha - waveAlpha) * (latest.hasWave ? 0.5f : 0.12f);
            changed = true;
        }
        else if (! juce::exactlyEqual (waveAlpha, targetAlpha))
        {
            waveAlpha = targetAlpha;
            changed = true;
        }

        if (changed)
            repaint();
    }

    void GrainDisplay::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (Theme::display);
        g.fillRoundedRectangle (bounds, Theme::corner);
        g.setColour (Theme::outline);
        g.drawRoundedRectangle (bounds.reduced (0.5f), Theme::corner, 1.0f);

        auto inner = bounds.reduced (12.0f, 8.0f);
        const auto header = inner.removeFromTop (20.0f);
        inner.removeFromTop (4.0f);

        // Header: mode on the left, sounding notes on the right.
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (12.0f, false, 0.08f));
        g.drawText (status, header, juce::Justification::centredLeft, true);
        paintChips (g, header);

        // Centre line and cycle boundaries, only while there is a loop to draw.
        const bool showWave = waveAlpha > 0.01f;
        if (showWave)
        {
            g.setColour (Theme::outline.withAlpha (0.8f * waveAlpha));
            g.drawHorizontalLine (juce::roundToInt (inner.getCentreY()), inner.getX(), inner.getRight());
        }

        const int cycles = showWave ? juce::jmax (1, latest.loopCycles) : 1;
        if (cycles > 1)
        {
            g.setColour (Theme::outline.withAlpha (0.6f * waveAlpha));
            for (int c = 1; c < cycles; ++c)
            {
                const float x = inner.getX() + inner.getWidth() * (float) c / (float) cycles;
                const float dashes[] = { 3.0f, 4.0f };
                g.drawDashedLine (juce::Line<float> (x, inner.getY(), x, inner.getBottom()), dashes, 2, 1.0f);
            }
        }

        paintSeam (g, inner);

        if (showWave)
        {
            paintWave (g, inner);
        }
        else
        {
            g.setColour (Theme::textFaint);
            g.setFont (Theme::font (13.0f));
            g.drawText ("Hold a MIDI note to freeze whatever is playing", inner, juce::Justification::centred, true);
        }
    }

    void GrainDisplay::paintSeam (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        const float seam = juce::jlimit (0.0f, 0.5f, latest.seamFraction);
        if (seam <= 0.001f)
            return;

        const auto region = area.withLeft (area.getRight() - area.getWidth() * seam);
        g.setColour (Theme::accentAlpha (0.06f));
        g.fillRect (region);

        // Diagonal hatching marks the crossfade into the start of the loop.
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (region.toNearestInt());
            g.setColour (Theme::accentAlpha (0.16f));
            for (float x = region.getX() - region.getHeight(); x < region.getRight(); x += 7.0f)
                g.drawLine (x, region.getBottom(), x + region.getHeight(), region.getY(), 1.0f);
        }

        g.setColour (Theme::accentAlpha (0.45f));
        g.drawVerticalLine (juce::roundToInt (region.getX()), region.getY(), region.getBottom());
        g.setFont (Theme::font (10.5f, true, 0.15f));
        g.drawText ("SEAM", region.withHeight (14.0f).translated (0.0f, 2.0f), juce::Justification::centred, false);
    }

    void GrainDisplay::paintWave (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        const int n = ScopeFrame::numPoints;
        const float mid = area.getCentreY();
        const float halfHeight = area.getHeight() * 0.5f;

        juce::Path line, fill;
        fill.startNewSubPath (area.getX(), mid);
        for (int i = 0; i < n; ++i)
        {
            const float x = area.getX() + area.getWidth() * (float) i / (float) (n - 1);
            const float v = juce::jlimit (-1.0f, 1.0f, latest.wave[(size_t) i] * displayGain);
            const float y = mid - v * halfHeight;
            if (i == 0) line.startNewSubPath (x, y);
            else        line.lineTo (x, y);
            fill.lineTo (x, y);
        }
        fill.lineTo (area.getRight(), mid);
        fill.closeSubPath();

        g.setColour (Theme::accentAlpha (0.10f * waveAlpha));
        g.fillPath (fill);
        g.setColour (Theme::accentAlpha (waveAlpha));
        g.strokePath (line, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void GrainDisplay::paintChips (juce::Graphics& g, juce::Rectangle<float> header) const
    {
        const auto font = Theme::font (12.0f, true, 0.04f);
        g.setFont (font);

        float right = header.getRight();
        for (int i = latest.numNotes - 1; i >= 0; --i)
        {
            const int note = latest.notes[(size_t) i];
            const auto label = formatNoteName (note);
            const float w = juce::GlyphArrangement::getStringWidth (font, label) + 14.0f;
            const auto chip = juce::Rectangle<float> (right - w, header.getY() + 2.0f, w, header.getHeight() - 4.0f);
            right -= w + 5.0f;

            if (chip.getX() < header.getCentreX())
                break;

            const bool focus = note == latest.focusNote;
            if (focus)
            {
                g.setColour (Theme::accent);
                g.fillRoundedRectangle (chip, chip.getHeight() * 0.5f);
                g.setColour (Theme::onAccent);
            }
            else
            {
                g.setColour (Theme::accentAlpha (0.55f));
                g.drawRoundedRectangle (chip.reduced (0.5f), chip.getHeight() * 0.5f, 1.0f);
                g.setColour (Theme::text);
            }
            g.drawText (label, chip, juce::Justification::centred, false);
        }
    }

    //==============================================================================
    void KeyStrip::setHeldNotes (const std::array<juce::uint64, 2>& held)
    {
        if (held != heldNotes)
        {
            heldNotes = held;
            repaint();
        }
    }

    void KeyStrip::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (bounds, 3.0f);

        auto isHeld = [this] (int note)
        {
            return ((heldNotes[(size_t) (note >> 6)] >> (note & 63)) & 1u) != 0;
        };
        auto isBlack = [] (int note)
        {
            const int k = note % 12;
            return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
        };

        int whiteCount = 0;
        for (int note = 0; note < 128; ++note)
            if (! isBlack (note))
                ++whiteCount;

        const auto keys = bounds.reduced (1.0f);
        const float whiteWidth = keys.getWidth() / (float) whiteCount;

        // Octave names only when a key is wide enough on screen (after scaling) to read them.
        const bool showLabels = whiteWidth * g.getInternalContext().getPhysicalPixelScaleFactor() >= 9.5f;

        // White keys first, then black keys on top.
        int whiteIndex = 0;
        for (int note = 0; note < 128; ++note)
        {
            if (isBlack (note))
                continue;

            const auto key = juce::Rectangle<float> (keys.getX() + whiteWidth * (float) whiteIndex, keys.getY(),
                                                     whiteWidth, keys.getHeight()).reduced (0.5f, 0.0f);
            g.setColour (isHeld (note) ? Theme::accent : Theme::panelRaised);
            g.fillRect (key);

            if (note % 12 == 0 && showLabels)
            {
                g.setColour (isHeld (note) ? Theme::onAccent : Theme::textFaint);
                g.setFont (Theme::font (9.0f));
                g.drawText (formatNoteName (note), key.withTrimmedLeft (1.5f).removeFromBottom (9.0f).withWidth (whiteWidth * 2.0f),
                            juce::Justification::centredLeft, false);
            }
            ++whiteIndex;
        }

        whiteIndex = 0;
        for (int note = 0; note < 128; ++note)
        {
            if (! isBlack (note))
            {
                ++whiteIndex;
                continue;
            }

            const float x = keys.getX() + whiteWidth * (float) whiteIndex - whiteWidth * 0.32f;
            const auto key = juce::Rectangle<float> (x, keys.getY(), whiteWidth * 0.64f, keys.getHeight() * 0.58f);
            g.setColour (isHeld (note) ? Theme::accent : Theme::background);
            g.fillRect (key);
        }
    }
}
