#include "PluginProcessor.h"
#include "PluginEditor.h"

using APF = juce::AudioParameterFloat;
using APC = juce::AudioParameterChoice;

static juce::NormalisableRange<float> dbRange (float lo, float hi) { return { lo, hi, 0.1f }; }

juce::AudioProcessorValueTreeState::ParameterLayout RiderProcessor::createLayout()
{
    auto dbAttr = juce::AudioParameterFloatAttributes().withLabel ("dB")
                    .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1); });
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back (std::make_unique<APF> (juce::ParameterID { ID::target, 1 }, "Target",            dbRange (-42.f, 0.f), -21.f, dbAttr));
    p.push_back (std::make_unique<APF> (juce::ParameterID { ID::vsens,  1 }, "Vocal Sensitivity", dbRange (-12.f, 12.f), 0.f, dbAttr));
    p.push_back (std::make_unique<APC> (juce::ParameterID { ID::attack, 1 }, "Attack", juce::StringArray { "Slow", "Fast" }, 0));
    p.push_back (std::make_unique<APF> (juce::ParameterID { ID::msens,  1 }, "Music Sensitivity", dbRange (-12.f, 12.f), 0.f, dbAttr));
    p.push_back (std::make_unique<APF> (juce::ParameterID { ID::rmin,   1 }, "Range Min",         dbRange (-12.f, 11.f), -6.f, dbAttr));
    p.push_back (std::make_unique<APF> (juce::ParameterID { ID::rmax,   1 }, "Range Max",         dbRange (-11.f, 12.f), 6.f, dbAttr));
    p.push_back (std::make_unique<APF> (juce::ParameterID { ID::idle,   1 }, "Idle",              dbRange (-12.f, 12.f), 0.f, dbAttr));
    p.push_back (std::make_unique<APF> (juce::ParameterID { ID::rider,  1 }, "Rider Fader",
                                        juce::NormalisableRange<float> (-12.f, 12.f), 0.f, dbAttr));
    p.push_back (std::make_unique<APC> (juce::ParameterID { ID::mode,   1 }, "Automation", juce::StringArray { "Off", "Write", "Read" }, 0,
                                        juce::AudioParameterChoiceAttributes().withAutomatable (false)));
    p.push_back (std::make_unique<APF> (juce::ParameterID { ID::output, 1 }, "Output",            dbRange (-12.f, 12.f), 0.f, dbAttr));
    return { p.begin(), p.end() };
}

RiderProcessor::RiderProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                        .withInput  ("Music",  juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "RIDER", createLayout())
{
    targetParam = apvts.getRawParameterValue (ID::target);
    vsensParam  = apvts.getRawParameterValue (ID::vsens);
    attackParam = apvts.getRawParameterValue (ID::attack);
    msensParam  = apvts.getRawParameterValue (ID::msens);
    rminParam   = apvts.getRawParameterValue (ID::rmin);
    rmaxParam   = apvts.getRawParameterValue (ID::rmax);
    idleParam   = apvts.getRawParameterValue (ID::idle);
    riderParamValue = apvts.getRawParameterValue (ID::rider);
    modeParam   = apvts.getRawParameterValue (ID::mode);
    outputParam = apvts.getRawParameterValue (ID::output);
    riderParam  = apvts.getParameter (ID::rider);
    startTimerHz (50);
}

RiderProcessor::~RiderProcessor()
{
    stopTimer();
    if (gestureOpen) riderParam->endChangeGesture();
}

bool RiderProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    if (in != out) return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo()) return false;
    if (l.inputBuses.size() > 1)
    {
        const auto sc = l.inputBuses[1];
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void RiderProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
    outGain.reset (sampleRate, 0.02);
    outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));
}

void RiderProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    auto main = getBusBuffer (buffer, true, 0);
    const int numIn = getMainBusNumInputChannels(), numOut = getMainBusNumOutputChannels();
    for (int c = numIn; c < numOut; ++c) buffer.clear (c, 0, n);

    const float* sc[2] = { nullptr, nullptr };
    int scCh = 0;
    if (getBusCount (true) > 1 && getBus (true, 1) != nullptr && getBus (true, 1)->isEnabled())
    {
        auto scBuf = getBusBuffer (buffer, true, 1);
        scCh = juce::jmin (2, scBuf.getNumChannels());
        for (int c = 0; c < scCh; ++c) sc[c] = scBuf.getReadPointer (c);
    }

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            playing.store (pos->getIsPlaying());

    rider::Settings s;
    s.targetDb  = targetParam->load();
    s.vocalSens = vsensParam->load();
    s.musicSens = msensParam->load();
    s.rangeMin  = rminParam->load();
    s.rangeMax  = rmaxParam->load();
    s.idleDb    = idleParam->load();
    s.attack    = attackParam->load() > 0.5f ? rider::AttackMode::Fast : rider::AttackMode::Slow;
    s.mode      = getMode();
    s.readDb    = riderParamValue->load();
    s.manual    = manualActive.load() && s.mode != rider::AutoMode::Read; // in Read, a drag edits the lane itself
    s.manualDb  = manualDb.load();

    const int ch = juce::jmin (2, main.getNumChannels());
    engine.process (main.getArrayOfWritePointers(), ch, scCh > 0 ? sc : nullptr, scCh, n, s, &telemetry);
    sidechainConnected.store (telemetry.musicDb.load() > -70.f);

    outGain.setTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));
    float peak = 0.f;
    if (outGain.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = outGain.getNextValue();
            for (int c = 0; c < ch; ++c) { auto* d = main.getWritePointer (c); d[i] *= g; peak = juce::jmax (peak, std::abs (d[i])); }
        }
    }
    else
    {
        const float g = outGain.getTargetValue();
        for (int c = 0; c < ch; ++c)
        {
            main.applyGain (c, 0, n, g);
            peak = juce::jmax (peak, main.getMagnitude (c, 0, n));
        }
    }
    if (peak > outPeak.load()) outPeak.store (peak);
    if (peak > 1.0f) clipped.store (true);
}

// Write mode: push the fader position to the host as if the user were moving it.
// Runs on the message thread, so it works with the editor closed.
void RiderProcessor::timerCallback()
{
    const bool writing = getMode() == rider::AutoMode::Write && playing.load();
    if (writing)
    {
        if (! gestureOpen) { riderParam->beginChangeGesture(); gestureOpen = true; lastWrittenDb = 1000.f; }
        const float db = telemetry.riderDb.load();
        if (std::abs (db - lastWrittenDb) > 0.02f)
        {
            riderParam->setValueNotifyingHost (riderParam->convertTo0to1 (db));
            lastWrittenDb = db;
        }
    }
    else if (gestureOpen)
    {
        riderParam->endChangeGesture();
        gestureOpen = false;
    }
}

void RiderProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}

void RiderProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* RiderProcessor::createEditor() { return new RiderEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RiderProcessor(); }
