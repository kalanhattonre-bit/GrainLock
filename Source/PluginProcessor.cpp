#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Presets.h"

#include <cmath>

using namespace grainlock;

GrainLockProcessor::GrainLockProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "GrainLockState", createParameterLayout()),
      params (apvts)
{
}

void GrainLockProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
}

void GrainLockProcessor::releaseResources() {}

void GrainLockProcessor::reset()
{
    engine.reset();
}

bool GrainLockProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    const auto in = layouts.getMainInputChannelSet();
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

EngineParams GrainLockProcessor::readParameters() const noexcept
{
    // A host can hand over a NaN or infinite value; anything non-finite falls back to the default.
    const EngineParams d {};
    auto value  = [] (const std::atomic<float>* p, float fallback) { const float v = p->load(); return std::isfinite (v) ? v : fallback; };
    auto asInt  = [&value] (const std::atomic<float>* p, int fallback) { return (int) std::lround (value (p, (float) fallback)); };
    auto asBool = [&value] (const std::atomic<float>* p, bool fallback) { return value (p, fallback ? 1.0f : 0.0f) >= 0.5f; };

    EngineParams p;
    p.grainCycles      = juce::jlimit (minCycles, maxCycles, asInt (params.grainCycles, d.grainCycles));
    p.smoothPercent    = value (params.smooth, d.smoothPercent);
    p.offsetMs         = value (params.offset, d.offsetMs);
    p.captureMode      = asInt (params.captureMode, (int) d.captureMode) == (int) CaptureMode::hold ? CaptureMode::hold : CaptureMode::live;
    p.refreshMs        = value (params.refresh, d.refreshMs);
    p.pitchLock        = asBool (params.pitchLock, d.pitchLock);

    p.formantSemitones = value (params.formant, d.formantSemitones);
    p.tuneSemitones    = asInt (params.tune, d.tuneSemitones);
    p.fineCents        = value (params.fine, d.fineCents);
    p.glideMs          = value (params.glide, d.glideMs);
    p.mono             = asBool (params.mono, d.mono);
    p.velSensPercent   = value (params.velSens, d.velSensPercent);

    p.attackMs         = value (params.attack, d.attackMs);
    p.decayMs          = value (params.decay, d.decayMs);
    p.sustainPercent   = value (params.sustain, d.sustainPercent);
    p.releaseMs        = value (params.release, d.releaseMs);

    p.lfoRateHz        = value (params.lfoRate, d.lfoRateHz);
    p.lfoSync          = asInt (params.lfoSync, d.lfoSync);
    p.lfoShape         = (LfoShape) juce::jlimit (0, 3, asInt (params.lfoShape, (int) d.lfoShape));
    p.lfoDepthPercent  = value (params.lfoDepth, d.lfoDepthPercent);
    p.lfoTarget        = (LfoTarget) juce::jlimit (0, 2, asInt (params.lfoTarget, (int) d.lfoTarget));

    p.mixPercent       = value (params.mix, d.mixPercent);
    p.dryWhenIdle      = asBool (params.dryWhenIdle, d.dryWhenIdle);
    p.outGainDb        = value (params.outGain, d.outGainDb);
    return p;
}

HostTiming GrainLockProcessor::readHostTiming() const noexcept
{
    HostTiming timing;
    if (auto* host = getPlayHead())
    {
        if (const auto position = host->getPosition())
        {
            // Ignore tempos and positions no real session has; the LFO keeps its own clock instead.
            if (const auto bpm = position->getBpm())
                if (std::isfinite (*bpm) && *bpm > 0.0 && *bpm <= 1000.0)
                    timing.bpm = *bpm;

            if (const auto ppq = position->getPpqPosition())
            {
                if (std::isfinite (*ppq) && std::abs (*ppq) < 1.0e12)
                {
                    timing.hasPpq = true;
                    timing.ppq = *ppq;
                }
            }

            timing.playing = position->getIsPlaying();
        }
    }
    return timing;
}

void GrainLockProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), getTotalNumInputChannels(),
                    buffer.getNumSamples(), midi, readParameters(), readHostTiming());
}

void GrainLockProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Bypassed audio is the input, on both sides even for a mono input.
    if (getTotalNumInputChannels() == 1 && buffer.getNumChannels() > 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, buffer.getNumSamples());

    engine.processBypassed (buffer.getArrayOfReadPointers(), buffer.getNumChannels(), getTotalNumInputChannels(),
                            buffer.getNumSamples());
}

int GrainLockProcessor::getNumPresets() const
{
    return (int) factoryPresets().size();
}

juce::String GrainLockProcessor::getPresetName (int index) const
{
    const auto& presets = factoryPresets();
    return juce::isPositiveAndBelow (index, (int) presets.size()) ? juce::String (presets[(size_t) index].name) : juce::String();
}

int GrainLockProcessor::getPresetIndex (const juce::String& name) const
{
    const auto& presets = factoryPresets();
    for (size_t i = 0; i < presets.size(); ++i)
        if (name == presets[i].name)
            return (int) i;
    return -1;
}

void GrainLockProcessor::loadPreset (int index)
{
    const auto& presets = factoryPresets();
    if (juce::isPositiveAndBelow (index, (int) presets.size()))
        applyPreset (apvts, presets[(size_t) index]);
}

juce::String GrainLockProcessor::getCurrentPresetName() const
{
    return apvts.state.getProperty ("presetName", "Init").toString();
}

juce::AudioProcessorEditor* GrainLockProcessor::createEditor()
{
    return new GrainLockEditor (*this);
}

void GrainLockProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void GrainLockProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GrainLockProcessor();
}
