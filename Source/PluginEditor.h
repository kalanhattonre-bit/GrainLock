#pragma once

#include "PluginProcessor.h"

class GrainLockEditor final : public juce::AudioProcessorEditor
{
public:
    explicit GrainLockEditor (GrainLockProcessor&);
    ~GrainLockEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    GrainLockProcessor& audioProcessor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainLockEditor)
};
