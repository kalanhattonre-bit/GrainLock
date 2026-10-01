#pragma once

#include "Controls.h"
#include "GrainDisplay.h"
#include "LfoPanel.h"

#include <array>
#include <map>
#include <memory>
#include <vector>

class GrainLockProcessor;

namespace grainlock::ui
{
    /** The whole interface at its base size (780 x 470). The editor scales it to the window.

        Top bar, the display with the OUTPUT block beside it (always in view), then five pages of
        controls (FREEZE, PLAY, MOTION, KEYS, TONE) and the key strip. A page is two rows of thirteen
        cells; a knob or a switch takes one cell, a drop-down two. */
    class MainPanel final : public juce::Component
    {
    public:
        static constexpr int numPages = 5;
        static constexpr int motionPage = 2;

        explicit MainPanel (GrainLockProcessor& processor);

        void paint (juce::Graphics&) override;
        void resized() override;

        /** Called about 30 times a second. frame is null when the audio thread sent nothing new. */
        void tick (const ScopeFrame* frame);

    private:
        static constexpr int cellsPerRow = 13;
        static constexpr int cellWidth = 58;

        struct Cell
        {
            juce::Component* component = nullptr;   // null = an empty cell
            int span = 1;
        };

        struct Group
        {
            juce::String title;
            std::vector<Cell> cells;
            int firstCell = 0, numCells = 0;   // worked out by finishPages()
        };

        struct Page
        {
            std::array<std::vector<Group>, 2> rows;
        };

        Cell knob (const char* id, const char* label, bool bipolar = false);
        Cell pill (const char* id, const char* label);
        Cell choice (const char* id, const char* label);
        static Cell gap() { return {}; }
        /** The same control over two cells: for a name too long for one. */
        static Cell wide (Cell cell) { cell.span = 2; return cell; }

        void buildPages();
        void finishPages();
        void showPage (int index);
        void showLfoPage (int index);
        void updateFades();
        void fade (const char* id, bool isLive);

        juce::Rectangle<int> rowBounds (int row) const;
        juce::Rectangle<int> cellBounds (int row, int firstCell, int span) const;

        void refreshPresetBox();
        void stepPreset (int delta);
        juce::String statusText() const;
        juce::String waitingText() const;
        float plainValue (const char* id) const;

        GrainLockProcessor& processor;
        juce::AudioProcessorValueTreeState& state;

        // Top bar
        ChevronButton previousPreset { false }, nextPreset { true };
        PresetButton presetBox;
        SegmentedControl captureMode;
        juce::Rectangle<int> captureCaption;

        GrainDisplay display;

        // OUTPUT: beside the display, whichever page is open.
        Knob mix, gain;
        PillToggle dryWhenIdle, autoGain;
        juce::Rectangle<int> outputPanel;

        // Pages
        TabStrip pageTabs;
        std::array<Page, numPages> pages;
        std::vector<std::unique_ptr<juce::Component>> owned;
        std::map<juce::String, juce::Component*> byId;
        int currentPage = 0;

        // MOTION's first row: three LFOs that all run together; the tabs pick whose controls are shown.
        TabStrip lfoTabs;
        std::array<std::unique_ptr<LfoPage>, numLfos> lfoPages;

        KeyStrip keys;

        ScopeFrame lastFrame;
        juce::String shownPresetName;
    };
}
