#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "RiderEngine.h"
#include "EcoBus.h"

namespace ID
{
    static const juce::String target { "target" }, vsens { "vsens" }, attack { "attack" }, msens { "msens" },
                              rmin { "rmin" }, rmax { "rmax" }, idle { "idle" }, rider { "rider" },
                              mode { "automode" }, output { "output" },
                              duck { "duck" }, duckDepth { "duckdepth" }, duckThresh { "duckthresh" },
                              send { "send" }, align { "align" };
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

    const juce::String getName() const override { return "Free Rider"; }
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

    // ---- Ecosystem ---------------------------------------------------------------
    bool   ecoAvailable() const { return bus != nullptr; }
    int    getDuckChannel() const { return juce::roundToInt (duckParam->load()); }
    int    getSendChannel() const { return juce::roundToInt (sendParam->load()); }
    int    getLatencyNow() const { return curAlign.load(); }
    juce::String channelLabel (int ch) const;          // "Off", "01", "Kick" (message thread)
    void   renameChannel (int ch, const juce::String& name);
    int    getNamesVersion() const { return namesVersion; }
    juce::String getEcoStatus() const;                  // one or two lines for the editor

    std::atomic<int>   duckSenders { 0 };     // live senders heard on the duck channel
    std::atomic<float> duckLatePct { 0.f };   // share of key audio that arrived too late
    std::atomic<bool>  timelineMoving { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;   // automation write, Ecosystem housekeeping (message thread)
    int  alignForSettings() const;
    void syncNames (bool force);

    rider::Engine engine;
    rider::Ducker ducker;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outGain;

    std::atomic<float>* targetParam = nullptr; std::atomic<float>* vsensParam = nullptr;
    std::atomic<float>* attackParam = nullptr; std::atomic<float>* msensParam = nullptr;
    std::atomic<float>* rminParam   = nullptr; std::atomic<float>* rmaxParam  = nullptr;
    std::atomic<float>* idleParam   = nullptr; std::atomic<float>* riderParamValue = nullptr;
    std::atomic<float>* modeParam   = nullptr; std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* duckParam   = nullptr; std::atomic<float>* depthParam = nullptr;
    std::atomic<float>* threshParam = nullptr; std::atomic<float>* sendParam = nullptr;
    std::atomic<float>* alignParam  = nullptr;
    juce::RangedAudioParameter* riderParam = nullptr;

    std::atomic<bool> playing { false };
    bool  gestureOpen = false;
    float lastWrittenDb = 1000.f;

    // Ecosystem bus
    eco::Shared* bus = nullptr;
    uint64_t ecoId = 0;
    eco::Sender sender;
    std::atomic<int> curAlign { 0 };          // latency in samples currently applied
    int  appliedAlign = -1;                    // audio thread's view, to reset the delay on change
    double sr = 48000.0;
    int64_t lastPos = 0; int lastN = 0; bool lastPosValid = false;

    // alignment delay (vocal 2 ch + music sidechain 2 ch)
    juce::AudioBuffer<float> delayBuf;
    int delayMask = 0, delayWrite = 0;
    juce::AudioBuffer<float> keyBuf, sendBuf;

    // channel names: local copy (saved with the project) and the bus's table
    char names[eco::kNumChannels][eco::kNameLen] {};
    int64_t namesEditedMs = 0;
    int namesVersion = 0;
    int nameTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RiderProcessor)
};
