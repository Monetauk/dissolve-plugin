#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <vector>

class DissolveProcessor : public juce::AudioProcessor
{
public:
    DissolveProcessor();
    ~DissolveProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Dissolve"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    struct Grain
    {
        bool active = false;
        double readAbs = 0.0;
        double rate = 1.0;
        int age = 0, len = 1, peakAt = 1;
        float gainL = 0.7f, gainR = 0.7f, amp = 1.0f;
        float lpA = 1.0f, lpL = 0.0f, lpR = 0.0f;
        bool useLp = false;
    };

    struct GrainParams
    {
        float sizeMs, density, position, pitch, chaos, filter, width, shape;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    float param (const char* id) const { return apvts.getRawParameterValue (id)->load(); }
    void spawnGrain (const GrainParams& p);
    float readInterp (const std::vector<float>& ring, double absIndex) const;

    static constexpr int maxGrains = 64;
    std::array<Grain, maxGrains> grains;

    double fs = 44100.0;
    int ringN = 1;
    int writePos = 0;
    long long writeCount = 0;
    std::vector<float> ringL, ringR;
    std::vector<float> wetL, wetR, rvL, rvR;

    float spawnCountdown = 0.0f;
    juce::Random rnd { 4821 };
    juce::Reverb reverb;

    bool lastDissolve = false;
    bool dissolving = false;
    double dissolveSamples = 0.0;
    float dest = 0.0f;

    float holdL = 0.0f, holdR = 0.0f, acc = 0.0f, crackleVal = 0.0f;
    int crackleLeft = 0, dropLeft = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DissolveProcessor)
};
