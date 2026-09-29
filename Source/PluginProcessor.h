#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"
#include "dsp/GrainEngine.h"

class GrainLockProcessor final : public juce::AudioProcessor
{
public:
    GrainLockProcessor();
    ~GrainLockProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;

    /** Cubase's bypass button lands here: pass audio through and make sure no note is left stuck. */
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "GrainLock"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 5.0; }   // longest Release

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    grainlock::ScopeFifo& getScopeFifo() noexcept { return engine.getScopeFifo(); }

    /** For the test runner: what reached the output limiter (NaNs, peaks) since the last reset. */
    const grainlock::LimiterStats& getLimiterStats() const noexcept { return engine.getLimiterStats(); }
    void resetLimiterStats() noexcept { engine.resetLimiterStats(); }


    juce::AudioProcessorValueTreeState apvts;

private:
    grainlock::EngineParams readParameters() const noexcept;
    grainlock::HostTiming readHostTiming() const noexcept;

    grainlock::ParameterRefs params;
    grainlock::GrainEngine engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainLockProcessor)
};
