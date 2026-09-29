// Offline test runner for GrainLock. Runs the shipped processor with no host and exits
// non-zero if any check fails, so CI fails with it.

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

#include <cmath>
#include <cstdio>

namespace
{
    int failures = 0;

    void check (bool ok, const char* what)
    {
        std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (! ok)
            ++failures;
    }

    void testLoadsAndPassesAudio()
    {
        GrainLockProcessor proc;
        proc.setPlayConfigDetails (2, 2, 48000.0, 512);
        proc.prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> buffer (2, 512);
        juce::Random rng (1234);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample (ch, i, rng.nextFloat() * 0.5f - 0.25f);

        juce::AudioBuffer<float> input (buffer);
        juce::MidiBuffer midi;
        proc.processBlock (buffer, midi);

        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                maxDiff = std::max (maxDiff, std::abs (buffer.getSample (ch, i) - input.getSample (ch, i)));

        check (maxDiff <= 1.0e-6f, "scaffold: processor loads and passes audio through unchanged");
        check (proc.getLatencySamples() == 0, "scaffold: reports zero latency");
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    testLoadsAndPassesAudio();

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
