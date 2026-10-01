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

    for (size_t i = 0; i < (size_t) numLfos; ++i)
    {
        const auto& refs = params.lfo[i];
        const auto& fallback = d.lfos[i];
        auto& l = p.lfos[i];
        l.on           = asBool (refs.on, fallback.on);
        l.rateHz       = value (refs.rate, fallback.rateHz);
        l.sync         = asInt (refs.sync, fallback.sync);
        l.shape        = (LfoShape) juce::jlimit (0, numLfoShapes - 1, asInt (refs.shape, (int) fallback.shape));
        l.depthPercent = value (refs.depth, fallback.depthPercent);
        l.trig         = (LfoTrig) juce::jlimit (0, numLfoTrigs - 1, asInt (refs.trig, (int) fallback.trig));
        l.fadeMs       = value (refs.fade, fallback.fadeMs);
        l.phaseDegrees = value (refs.phase, fallback.phaseDegrees);
        l.invert       = asBool (refs.invert, fallback.invert);
    }

    p.mixPercent       = value (params.mix, d.mixPercent);
    p.dryWhenIdle      = asBool (params.dryWhenIdle, d.dryWhenIdle);
    p.outGainDb        = value (params.outGain, d.outGainDb);
    p.bypass           = asBool (params.bypass, false);

    p.formantTrack     = asBool (params.formantTrack, d.formantTrack);
    p.autoGain         = asBool (params.autoGain, d.autoGain);

    p.envAttackMs      = value (params.envAttack, d.envAttackMs);
    p.envDecayMs       = value (params.envDecay, d.envDecayMs);
    p.envPitch         = juce::jlimit (-24.0f, 24.0f, value (params.envPitch, d.envPitch));
    p.envFormant       = juce::jlimit (-12.0f, 12.0f, value (params.envFormant, d.envFormant));
    p.envGrain         = juce::jlimit (-15, 15, asInt (params.envGrain, d.envGrain));
    p.tapeStop         = asBool (params.tapeStop, d.tapeStop);

    p.bendUp           = juce::jlimit (0, 24, asInt (params.bendUp, d.bendUp));
    p.bendDown         = juce::jlimit (0, 24, asInt (params.bendDown, d.bendDown));
    p.vibRateHz        = juce::jlimit (0.1f, 12.0f, value (params.vibRate, d.vibRateHz));
    p.vibDepthCents    = juce::jlimit (0.0f, 200.0f, value (params.vibDepth, d.vibDepthCents));
    for (size_t i = 0; i < (size_t) numModSources; ++i)
    {
        p.sources[i].dest          = (ModDest) juce::jlimit (0, numModDests - 1, asInt (params.source[i].dest, (int) d.sources[i].dest));
        p.sources[i].amountPercent = value (params.source[i].amount, d.sources[i].amountPercent);
    }

    p.grabAtKey        = asInt (params.grabAt, d.grabAtKey ? 1 : 0) == 1;
    p.waitMs           = juce::jlimit (0.0f, 2000.0f, value (params.wait, d.waitMs));
    p.waitSync         = asInt (params.waitSync, d.waitSync);
    p.offsetSync       = asInt (params.offsetSync, d.offsetSync);
    p.refreshSync      = asInt (params.refreshSync, d.refreshSync);

    p.snapMs            = juce::jlimit (0.0f, 100.0f, value (params.snap, d.snapMs));
    p.thresholdDb       = juce::jlimit (thresholdOffDb, -10.0f, value (params.threshold, d.thresholdDb));
    p.maxWaitMs         = juce::jlimit (0.0f, 2000.0f, value (params.maxWait, d.maxWaitMs));
    p.skipHiss          = asBool (params.skipHiss, d.skipHiss);
    p.gate              = asBool (params.gate, d.gate);
    p.gridGrabs         = asBool (params.gridGrabs, d.gridGrabs);
    p.skipChancePercent = juce::jlimit (0.0f, 100.0f, value (params.skipChance, d.skipChancePercent));
    p.feedbackPercent   = juce::jlimit (0.0f, 100.0f, value (params.feedback, d.feedbackPercent));

    p.sustainPedal      = asBool (params.sustainPedal, d.sustainPedal);
    p.keyUpMode          = (KeyUpMode) juce::jlimit (0, numKeyUpModes - 1, asInt (params.keyUpMode, (int) d.keyUpMode));
    p.noteLength          = juce::jlimit (0, 8, asInt (params.noteLength, d.noteLength));
    p.glideLegato       = asBool (params.glideLegato, d.glideLegato);
    p.glidePerOctave    = asBool (params.glideRate, d.glidePerOctave);
    p.polyGlide         = asBool (params.polyGlide, d.polyGlide);
    p.voices            = juce::jlimit (1, 8, asInt (params.voices, d.voices));

    p.lowCutHz          = juce::jlimit (20.0f, 2000.0f, value (params.lowCut, d.lowCutHz));
    p.highCutHz         = juce::jlimit (500.0f, 20000.0f, value (params.highCut, d.highCutHz));
    p.tiltDb            = juce::jlimit (-6.0f, 6.0f, value (params.tilt, d.tiltDb));
    p.driveDb           = juce::jlimit (0.0f, 24.0f, value (params.drive, d.driveDb));
    p.hollowPercent     = juce::jlimit (0.0f, 100.0f, value (params.hollow, d.hollowPercent));
    p.diffusePercent    = juce::jlimit (0.0f, 100.0f, value (params.diffuse, d.diffusePercent));
    p.spreadPercent     = juce::jlimit (0.0f, 100.0f, value (params.spread, d.spreadPercent));
    p.spreadMode        = (SpreadMode) juce::jlimit (0, numSpreadModes - 1, asInt (params.spreadMode, (int) d.spreadMode));
    p.widthPercent      = juce::jlimit (0.0f, 100.0f, value (params.width, d.widthPercent));
    p.driftPercent      = juce::jlimit (0.0f, 100.0f, value (params.drift, d.driftPercent));
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
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            migrateLegacyLfoState (*xml);   // v0.1 saved one shared LFO; move it onto the matching new one
            fillMissingParameters (*xml, apvts);   // what an older version did not save gets a definite value
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GrainLockProcessor();
}
