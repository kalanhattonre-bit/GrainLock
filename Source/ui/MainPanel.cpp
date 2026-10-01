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

        void paintCaption (juce::Graphics& g, const juce::String& title, juce::Rectangle<float> strip)
        {
            g.setColour (Theme::accent);
            g.fillRoundedRectangle (strip.getX(), strip.getY() + 3.0f, 3.0f, 10.0f, 1.5f);
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (11.5f, true, 0.2f));
            g.drawText (title, strip.withTrimmedLeft (8.0f), juce::Justification::centredLeft, false);
        }

        constexpr int pageTop = 232;       // where the first row of a page starts
        constexpr int rowHeight = 100;     // caption strip + controls
        constexpr int rowGap = 4;
        constexpr int captionHeight = 20;
        constexpr float fadedAlpha = 0.45f;
    }

    MainPanel::MainPanel (GrainLockProcessor& p)
        : processor (p),
          state (p.apvts),
          captureMode (param (state, ParamID::captureMode), { "HOLD", "LIVE" }),
          mix         (state, ParamID::mix, "Mix"),
          gain        (state, ParamID::outGain, "Gain", true),
          dryWhenIdle (state, ParamID::dryWhenIdle, "Dry Idle"),
          autoGain    (state, ParamID::autoGain, "Auto Gain"),
          pageTabs    ({ "FREEZE", "PLAY", "MOTION", "KEYS", "TONE" }, false),
          lfoTabs     ({ "PITCH", "FORMANT", "GRAIN" }, true)
    {
        for (auto* c : std::initializer_list<juce::Component*> {
                 &previousPreset, &nextPreset, &presetBox, &captureMode, &display,
                 &mix, &gain, &dryWhenIdle, &autoGain, &pageTabs, &keys })
            addAndMakeVisible (c);

        captureMode.textHeight = 12.5f;
        addChildComponent (lfoTabs);

        for (int i = 0; i < numLfos; ++i)
        {
            lfoPages[(size_t) i] = std::make_unique<LfoPage> (state, ParamID::lfo[i]);
            addChildComponent (*lfoPages[(size_t) i]);
        }

        buildPages();
        finishPages();

        // The open page and the open LFO tab are part of the saved session, like the window size.
        lfoTabs.onSelect = [this] (int index)
        {
            showLfoPage (index);
            state.state.setProperty ("lfoTab", index, nullptr);
        };
        lfoTabs.setSelected (juce::jlimit (0, numLfos - 1, (int) state.state.getProperty ("lfoTab", 0)));

        pageTabs.onSelect = [this] (int index)
        {
            showPage (index);
            state.state.setProperty ("page", index, nullptr);
        };
        const int openPage = juce::jlimit (0, numPages - 1, (int) state.state.getProperty ("page", 0));
        pageTabs.setSelected (openPage);
        showPage (openPage);

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
    MainPanel::Cell MainPanel::knob (const char* id, const char* label, bool bipolar)
    {
        auto control = std::make_unique<Knob> (state, id, label, bipolar);
        auto* raw = control.get();
        addChildComponent (*raw);
        byId[id] = raw;
        owned.push_back (std::move (control));
        return { raw, 1 };
    }

    MainPanel::Cell MainPanel::pill (const char* id, const char* label)
    {
        auto control = std::make_unique<PillToggle> (state, id, label);
        auto* raw = control.get();
        addChildComponent (*raw);
        byId[id] = raw;
        owned.push_back (std::move (control));
        return { raw, 1 };
    }

    MainPanel::Cell MainPanel::choice (const char* id, const char* label)
    {
        auto control = std::make_unique<ChoiceBox> (state, id, label);
        auto* raw = control.get();
        addChildComponent (*raw);
        byId[id] = raw;
        owned.push_back (std::move (control));
        return { raw, 2 };
    }

    void MainPanel::buildPages()
    {
        // FREEZE: what a note grabs, and when.
        pages[0].rows[0] = {
            Group { "GRAIN",    { knob (ParamID::grainCycles, "Grain"), knob (ParamID::smooth, "Smooth"), pill (ParamID::pitchLock, "Lock") } },
            Group { "TIMING",   { knob (ParamID::offset, "Offset"), choice (ParamID::offsetSync, "Offset Sync"),
                                  knob (ParamID::refresh, "Refresh"), choice (ParamID::refreshSync, "Refresh Sync") } },
            Group { "GRID",     { pill (ParamID::gridGrabs, "On Grid"), knob (ParamID::skipChance, "Skip") } },
            Group { "FEEDBACK", { knob (ParamID::feedback, "Amount"), gap() } } };
        pages[0].rows[1] = {
            Group { "GRAB",     { choice (ParamID::grabAt, "Grab"), knob (ParamID::wait, "Wait"), choice (ParamID::waitSync, "Wait Sync"),
                                  knob (ParamID::snap, "Snap") } },
            Group { "INPUT",    { knob (ParamID::threshold, "Thresh"), knob (ParamID::maxWait, "Max Wait"),
                                  pill (ParamID::skipHiss, "Skip Hiss"), pill (ParamID::gate, "Gate") } } };

        // PLAY: the note itself.
        pages[1].rows[0] = {
            Group { "PITCH",    { knob (ParamID::tune, "Tune", true), knob (ParamID::fine, "Fine", true) } },
            Group { "FORMANT",  { knob (ParamID::formant, "Formant", true), wide (pill (ParamID::formantTrack, "Key Follow")) } },
            Group { "GLIDE",    { knob (ParamID::glide, "Glide"), pill (ParamID::glideLegato, "Legato"),
                                  pill (ParamID::glideRate, "Per Oct"), pill (ParamID::polyGlide, "Poly") } },
            Group { "VOICES",   { pill (ParamID::mono, "Mono"), knob (ParamID::voices, "Voices") } } };
        pages[1].rows[1] = {
            Group { "AMP ENVELOPE", { knob (ParamID::attack, "A"), knob (ParamID::decay, "D"), knob (ParamID::sustain, "S"),
                                      knob (ParamID::release, "R"), knob (ParamID::velSens, "Vel") } } };

        // MOTION: the first row is the LFO (its own component); the second, what moves once per note.
        pages[motionPage].rows[0] = { Group { "LFO", {} } };
        pages[motionPage].rows[1] = {
            Group { "NOTE ENVELOPE", { knob (ParamID::envAttack, "A"), knob (ParamID::envDecay, "D"), knob (ParamID::envPitch, "Pitch", true),
                                       knob (ParamID::envFormant, "Formant", true), knob (ParamID::envGrain, "Grain", true),
                                       pill (ParamID::tapeStop, "Tape Stop") } },
            Group { "VIBRATO",  { knob (ParamID::vibRate, "Rate"), knob (ParamID::vibDepth, "Depth") } } };

        // KEYS: what the keyboard's other controls do, and when a note ends.
        pages[3].rows[0] = {
            Group { "BEND",       { knob (ParamID::bendUp, "Up"), knob (ParamID::bendDown, "Down") } },
            Group { "MOD WHEEL",  { choice (ParamID::wheelDest, "Moves"), knob (ParamID::wheelAmt, "Amount", true) } },
            Group { "AFTERTOUCH", { choice (ParamID::touchDest, "Moves"), knob (ParamID::touchAmt, "Amount", true) } },
            Group { "EXPRESSION", { choice (ParamID::exprDest, "Moves"), knob (ParamID::exprAmt, "Amount", true) } } };
        pages[3].rows[1] = {
            Group { "KEY UP",   { choice (ParamID::keyUpMode, "Mode"), choice (ParamID::noteLength, "Length"),
                                  pill (ParamID::sustainPedal, "Sus Pedal") } } };

        // TONE: on the frozen sound only.
        pages[4].rows[0] = {
            Group { "FILTER",   { knob (ParamID::lowCut, "Low Cut"), knob (ParamID::highCut, "High Cut"), knob (ParamID::tilt, "Tilt", true) } },
            Group { "COLOUR",   { knob (ParamID::drive, "Drive"), knob (ParamID::hollow, "Hollow"), knob (ParamID::diffuse, "Diffuse") } } };
        pages[4].rows[1] = {
            Group { "STEREO",   { knob (ParamID::spread, "Spread"), choice (ParamID::spreadMode, "Spread Mode"),
                                  knob (ParamID::width, "Width"), knob (ParamID::drift, "Drift") } } };
    }

    void MainPanel::finishPages()
    {
        for (auto& page : pages)
            for (auto& row : page.rows)
            {
                int next = 0;
                for (auto& group : row)
                {
                    group.firstCell = next;
                    for (const auto& cell : group.cells)
                        next += cell.span;
                    group.numCells = next - group.firstCell;
                }
                jassert (next <= cellsPerRow);
            }

        pages[motionPage].rows[0].front().numCells = cellsPerRow;
    }

    //==============================================================================
    juce::Rectangle<int> MainPanel::rowBounds (int row) const
    {
        return { 12, pageTop + row * (rowHeight + rowGap), 756, rowHeight };
    }

    juce::Rectangle<int> MainPanel::cellBounds (int row, int firstCell, int span) const
    {
        const auto r = rowBounds (row);
        return juce::Rectangle<int> (r.getX() + 1 + cellWidth * firstCell, r.getY() + captionHeight,
                                     cellWidth * span, rowHeight - captionHeight - 4).reduced (2, 0);
    }

    void MainPanel::resized()
    {
        // Top bar
        previousPreset.setBounds (262, 10, 24, 24);
        presetBox.setBounds (290, 10, 200, 24);
        nextPreset.setBounds (494, 10, 24, 24);
        captureCaption = { 560, 10, 62, 24 };
        captureMode.setBounds (628, 10, 140, 24);

        // The display, with the OUTPUT block to its right.
        display.setBounds (12, 50, 618, 150);
        outputPanel = { 636, 50, 132, 150 };
        mix.setBounds (642, 72, 60, 70);
        gain.setBounds (702, 72, 60, 70);
        dryWhenIdle.setBounds (642, 146, 60, 48);
        autoGain.setBounds (702, 146, 60, 48);

        pageTabs.setBounds (12, 206, 520, 22);

        for (auto& page : pages)
            for (int r = 0; r < 2; ++r)
                for (const auto& group : page.rows[(size_t) r])
                {
                    int at = group.firstCell;
                    for (const auto& cell : group.cells)
                    {
                        if (cell.component != nullptr)
                            cell.component->setBounds (cellBounds (r, at, cell.span));
                        at += cell.span;
                    }
                }

        // The LFO tabs sit in the caption strip of MOTION's first row; the open LFO fills the row.
        const auto lfoRow = rowBounds (0);
        lfoTabs.setBounds (lfoRow.getX() + 1 + cellWidth, lfoRow.getY() + 2, 330, 18);
        for (auto& page : lfoPages)
            page->setBounds (lfoRow.getX() + 1, lfoRow.getY() + captionHeight, cellWidth * cellsPerRow, rowHeight - captionHeight - 4);

        keys.setBounds (12, 442, 738, 20);   // stops short of the window's resize corner
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

        // OUTPUT
        {
            const auto r = outputPanel.toFloat();
            g.setColour (Theme::panel);
            g.fillRoundedRectangle (r, Theme::corner);
            paintCaption (g, "OUTPUT", { r.getX() + 10.0f, r.getY() + 5.0f, r.getWidth() - 16.0f, 16.0f });
        }

        // The open page: two rows, each a panel with its groups named along the top.
        const auto& page = pages[(size_t) currentPage];
        for (int row = 0; row < 2; ++row)
        {
            const auto r = rowBounds (row).toFloat();
            g.setColour (Theme::panel);
            g.fillRoundedRectangle (r, Theme::corner);

            for (const auto& group : page.rows[(size_t) row])
            {
                const float x = r.getX() + 1.0f + (float) (cellWidth * group.firstCell);
                const float width = (float) (cellWidth * group.numCells);
                paintCaption (g, group.title, { x + 9.0f, r.getY() + 4.0f, width - 14.0f, 16.0f });

                if (group.firstCell > 0)
                {
                    g.setColour (Theme::outline);
                    g.drawVerticalLine (juce::roundToInt (x), r.getY() + 26.0f, r.getBottom() - 12.0f);
                }
            }
        }
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

        juce::String text ("HOLD");
        if (live)
        {
            // A Refresh that follows the tempo shows its note value; the knob's ms mean nothing then.
            const bool synced = juce::roundToInt (plainValue (ParamID::refreshSync)) != 0;
            text = "LIVE  /  REFRESH " + state.getParameter (synced ? ParamID::refreshSync : ParamID::refresh)->getCurrentValueAsText().toUpperCase();
            if (synced && plainValue (ParamID::gridGrabs) >= 0.5f)
                text << "  /  GRID";
        }

        text << "  /  " << cycles << (cycles == 1 ? " CYCLE" : " CYCLES");
        // (One cycle is one note period, Lock or no Lock: nothing is longer.)
        text << (lockOn ? juce::String ("  /  PITCH LOCKED")
                        : (cycles == 1 ? juce::String ("  /  LOCK OFF") : "  /  LOOP " + juce::String (cycles) + "X LONGER"));
        return text;
    }

    juce::String MainPanel::waitingText() const
    {
        // Why a key that is down is not sounding yet: the engine says which stage it is at.
        switch (lastFrame.waitingFor)
        {
            case 2:  return "WAITING FOR SOUND";
            case 1:  return "WAITING FOR A HIT";
            default: return "WAITING TO GRAB";
        }
    }

    void MainPanel::fade (const char* id, bool isLive)
    {
        const auto found = byId.find (id);
        if (found != byId.end())
            found->second->setAlpha (isLive ? 1.0f : fadedAlpha);
    }

    void MainPanel::updateFades()
    {
        // A control that does nothing with the other settings as they are fades back. It still works
        // (turning it is how you find out), and what decides is always other controls, never the
        // song or the keyboard, so nothing flickers.
        auto on = [this] (const char* id) { return plainValue (id) >= 0.5f; };
        auto index = [this] (const char* id) { return juce::roundToInt (plainValue (id)); };

        const bool live = on (ParamID::captureMode);
        const bool refreshFree = index (ParamID::refreshSync) == 0;
        const bool mono = on (ParamID::mono);
        const bool glideLive = mono || on (ParamID::polyGlide);
        const bool thresholdOn = plainValue (ParamID::threshold) > thresholdOffDb;
        const auto keyUp = (KeyUpMode) index (ParamID::keyUpMode);

        fade (ParamID::refresh, live && refreshFree);
        fade (ParamID::refreshSync, live);
        fade (ParamID::skipHiss, live);
        fade (ParamID::skipChance, live);
        fade (ParamID::gridGrabs, live && ! refreshFree);
        fade (ParamID::offset, index (ParamID::offsetSync) == 0);
        fade (ParamID::wait, index (ParamID::waitSync) == 0);
        fade (ParamID::maxWait, thresholdOn);
        fade (ParamID::gate, thresholdOn);

        fade (ParamID::glide, glideLive);
        fade (ParamID::glideLegato, glideLive && plainValue (ParamID::glide) > 0.0f);
        fade (ParamID::glideRate, glideLive && plainValue (ParamID::glide) > 0.0f);
        fade (ParamID::polyGlide, ! mono);
        fade (ParamID::voices, ! mono);

        const bool envInUse = ! juce::exactlyEqual (plainValue (ParamID::envPitch), 0.0f)
                              || ! juce::exactlyEqual (plainValue (ParamID::envFormant), 0.0f)
                              || index (ParamID::envGrain) != 0;
        fade (ParamID::envAttack, envInUse);
        fade (ParamID::envDecay, envInUse);

        bool vibratoInUse = false;
        for (const auto& source : ParamID::source)
        {
            const auto dest = (ModDest) index (source.dest);
            vibratoInUse = vibratoInUse || dest == ModDest::vibrato;
            fade (source.amount, dest != ModDest::off);
        }
        fade (ParamID::vibRate, vibratoInUse);
        fade (ParamID::vibDepth, vibratoInUse);

        fade (ParamID::noteLength, keyUp == KeyUpMode::toGrid || keyUp == KeyUpMode::fixed);
        fade (ParamID::sustainPedal, keyUp == KeyUpMode::normal || keyUp == KeyUpMode::toGrid);
        fade (ParamID::spreadMode, plainValue (ParamID::spread) > 0.0f);

        // Auto Gain evens out the summed cycles of a Pitch-Locked loop: with one cycle there is nothing to even out.
        bool severalCycles = index (ParamID::grainCycles) > 1 || index (ParamID::envGrain) > 0
                             || (on (ParamID::grainLfoOn) && plainValue (ParamID::grainLfoDepth) > 0.0f);
        for (const auto& source : ParamID::source)
            severalCycles = severalCycles || ((ModDest) index (source.dest) == ModDest::grain && plainValue (source.amount) > 0.0f);
        autoGain.setAlpha (on (ParamID::pitchLock) && severalCycles ? 1.0f : fadedAlpha);
        fade (ParamID::pitchLock, severalCycles);   // one cycle is one note period either way
    }

    void MainPanel::tick (const ScopeFrame* frame)
    {
        if (frame != nullptr)
            lastFrame = *frame;

        display.update (frame, statusText(), waitingText());
        keys.setNotes (lastFrame.heldNotes, lastFrame.waitingNotes);

        updateFades();
        for (int i = 0; i < numLfos; ++i)
        {
            lfoPages[(size_t) i]->refresh();
            lfoTabs.setActive (i, plainValue (ParamID::lfo[i].on) >= 0.5f);
        }

        // A dot on every knob an LFO is moving, riding at the modulated position; all three can move at once.
        auto modulate = [this] (const char* id, int lfoIndex, float swingFraction)
        {
            const auto found = byId.find (id);
            if (auto* target = found != byId.end() ? dynamic_cast<Knob*> (found->second) : nullptr)
                target->setModulation (lastFrame.lfoActive[(size_t) lfoIndex],
                                       target->getParameter().getValue() + lastFrame.lfoValues[(size_t) lfoIndex] * swingFraction);
        };
        modulate (ParamID::fine, (int) LfoTarget::pitch, lfoPitchRangeSemitones * 100.0f / 200.0f);
        modulate (ParamID::formant, (int) LfoTarget::formant, lfoFormantRangeSemitones / 24.0f);
        modulate (ParamID::grainCycles, (int) LfoTarget::grainCycles, lfoCyclesRange / (float) (maxCycles - minCycles));

        const auto name = processor.getCurrentPresetName();
        if (name != shownPresetName)
            refreshPresetBox();
    }

    void MainPanel::showPage (int index)
    {
        currentPage = juce::jlimit (0, numPages - 1, index);

        for (int p = 0; p < numPages; ++p)
            for (const auto& row : pages[(size_t) p].rows)
                for (const auto& group : row)
                    for (const auto& cell : group.cells)
                        if (cell.component != nullptr)
                            cell.component->setVisible (p == currentPage);

        lfoTabs.setVisible (currentPage == motionPage);
        showLfoPage (lfoTabs.getSelected());
        repaint();
    }

    void MainPanel::showLfoPage (int index)
    {
        for (int i = 0; i < numLfos; ++i)
            lfoPages[(size_t) i]->setVisible (currentPage == motionPage && i == index);
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
