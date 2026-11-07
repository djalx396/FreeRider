#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "RiderEngine.h"

namespace ID
{
    static const juce::String target { "target" }, vsens { "vsens" }, attack { "attack" }, msens { "msens" },
                              rmin { "rmin" }, rmax { "rmax" }, idle { "idle" }, rider { "rider" },
                              mode { "automode" }, output { "output" };
}

class RiderProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    RiderProcessor();
    ~RiderProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Rider"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    rider::Telemetry telemetry;

    // Editor → audio: the user is holding the Rider Fader.
    std::atomic<bool>  manualActive { false };
    std::atomic<float> manualDb { 0.f };

    std::atomic<float> outPeak { 0.f };          // linear, decays in the editor
    std::atomic<bool>  clipped { false };
    std::atomic<bool>  sidechainConnected { false };

    rider::AutoMode getMode() const { return (rider::AutoMode) (int) modeParam->load(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;   // writes Rider Fader automation (message thread)

    rider::Engine engine;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outGain;

    std::atomic<float>* targetParam = nullptr; std::atomic<float>* vsensParam = nullptr;
    std::atomic<float>* attackParam = nullptr; std::atomic<float>* msensParam = nullptr;
    std::atomic<float>* rminParam   = nullptr; std::atomic<float>* rmaxParam  = nullptr;
    std::atomic<float>* idleParam   = nullptr; std::atomic<float>* riderParamValue = nullptr;
    std::atomic<float>* modeParam   = nullptr; std::atomic<float>* outputParam = nullptr;
    juce::RangedAudioParameter* riderParam = nullptr;

    std::atomic<bool> playing { false };
    bool  gestureOpen = false;
    float lastWrittenDb = 1000.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RiderProcessor)
};
