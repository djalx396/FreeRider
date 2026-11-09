#include "PluginProcessor.h"
#include "PluginEditor.h"

using APF = juce::AudioParameterFloat;
using APC = juce::AudioParameterChoice;
using API = juce::AudioParameterInt;

static constexpr int kChunk = 1024;                                  // internal sub-block
static constexpr int kAlignBase[] = { 1024, 4096, 8192, 16384 };      // Short / Normal / Safe / Max @ 44.1/48k

static juce::NormalisableRange<float> dbRange (float lo, float hi) { return { lo, hi, 0.1f }; }

static juce::String chText (int v) { return v <= 0 ? juce::String ("Off") : juce::String (v).paddedLeft ('0', 2); }

juce::AudioProcessorValueTreeState::ParameterLayout RiderProcessor::createLayout()
{
    auto dbAttr = juce::AudioParameterFloatAttributes().withLabel ("dB")
                    .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1); });
    auto chAttr = juce::AudioParameterIntAttributes().withAutomatable (false)
                    .withStringFromValueFunction ([] (int v, int) { return chText (v); })
                    .withValueFromStringFunction ([] (const juce::String& t) { return t.trim().equalsIgnoreCase ("off") ? 0 : t.getIntValue(); });
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
    // Ecosystem
    p.push_back (std::make_unique<API> (juce::ParameterID { ID::duck,   1 }, "Duck Channel", 0, eco::kNumChannels, 0, chAttr));
    p.push_back (std::make_unique<APF> (juce::ParameterID { ID::duckDepth,  1 }, "Duck Depth",     dbRange (0.f, 24.f), 6.f, dbAttr));
    p.push_back (std::make_unique<APF> (juce::ParameterID { ID::duckThresh, 1 }, "Duck Threshold", dbRange (-60.f, 0.f), -30.f, dbAttr));
    p.push_back (std::make_unique<API> (juce::ParameterID { ID::send,   1 }, "Send Channel", 0, eco::kNumChannels, 0, chAttr));
    p.push_back (std::make_unique<APC> (juce::ParameterID { ID::align,  1 }, "Align", juce::StringArray { "Short", "Normal", "Safe", "Max" }, 1,
                                        juce::AudioParameterChoiceAttributes().withAutomatable (false)));
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
    duckParam   = apvts.getRawParameterValue (ID::duck);
    depthParam  = apvts.getRawParameterValue (ID::duckDepth);
    threshParam = apvts.getRawParameterValue (ID::duckThresh);
    sendParam   = apvts.getRawParameterValue (ID::send);
    alignParam  = apvts.getRawParameterValue (ID::align);
    riderParam  = apvts.getParameter (ID::rider);

    bus = eco::attach();
    ecoId = eco::newInstanceId (bus);
    syncNames (true);
    startTimerHz (50);
}

RiderProcessor::~RiderProcessor()
{
    stopTimer();
    sender.release();
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

// Duck off: zero latency (Free Rider as before). Duck on: the Alignment Buffer, scaled with
// the sample rate like every Ecosystem latency, so it's the same time at every rate.
int RiderProcessor::alignForSettings() const
{
    if (getDuckChannel() <= 0) return 0;
    const int f = sr > 130000.0 ? 4 : (sr > 70000.0 ? 2 : 1);
    const int a = juce::jlimit (0, 3, juce::roundToInt (alignParam->load()));
    return kAlignBase[a] * f;
}

void RiderProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    engine.prepare (sampleRate);
    ducker.prepare (sampleRate);
    outGain.reset (sampleRate, 0.02);
    outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    const int maxAlign = kAlignBase[3] * 4;
    const int size = juce::nextPowerOfTwo (maxAlign + juce::jmax (samplesPerBlock, kChunk) + 16);
    delayBuf.setSize (4, size);
    delayBuf.clear();
    delayMask = size - 1; delayWrite = 0;
    keyBuf.setSize (2, kChunk);
    sendBuf.setSize (1, kChunk);

    const int a = alignForSettings();
    curAlign.store (a);
    appliedAlign = -1;
    setLatencySamples (a);
    sender.setRate ((uint32_t) juce::roundToInt (sampleRate));
    lastPosValid = false;
}

void RiderProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    auto main = getBusBuffer (buffer, true, 0);
    const int numIn = getMainBusNumInputChannels(), numOut = getMainBusNumOutputChannels();
    for (int c = numIn; c < numOut; ++c) buffer.clear (c, 0, n);

    float* sc[2] = { nullptr, nullptr };
    int scCh = 0;
    if (getBusCount (true) > 1 && getBus (true, 1) != nullptr && getBus (true, 1)->isEnabled())
    {
        auto scBuf = getBusBuffer (buffer, true, 1);
        scCh = juce::jmin (2, scBuf.getNumChannels());
        for (int c = 0; c < scCh; ++c) sc[c] = scBuf.getWritePointer (c);
    }

    // timeline
    int64_t pos = 0; bool havePos = false, isPlay = false;
    if (auto* ph = getPlayHead())
        if (auto p = ph->getPosition())
        {
            if (auto t = p->getTimeInSamples()) { pos = *t; havePos = true; }
            isPlay = p->getIsPlaying() || p->getIsRecording();
        }
    playing.store (isPlay);
    const bool moving = havePos && (isPlay || (lastPosValid && pos == lastPos + lastN && n > 0));
    lastPos = pos; lastN = n; lastPosValid = havePos;
    timelineMoving.store (moving);

    const int A = curAlign.load();
    if (A != appliedAlign) { delayBuf.clear(); delayWrite = 0; appliedAlign = A; ducker.reset(); }

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

    const int duckCh = getDuckChannel(), sendCh = getSendChannel();
    rider::DuckSettings d;
    d.on       = duckCh > 0;
    d.depthDb  = depthParam->load();
    d.threshDb = threshParam->load();
    d.attack   = s.attack;                       // the ducker follows the rider's speed

    const int ch = juce::jmin (2, main.getNumChannels());
    float* io[2] = { ch > 0 ? main.getWritePointer (0) : nullptr, ch > 1 ? main.getWritePointer (1) : nullptr };
    outGain.setTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    float peak = 0.f;
    int heardSenders = 0; long missing = 0, expected = 0;
    const uint32_t srHz = (uint32_t) juce::roundToInt (sr);

    for (int off = 0; off < n; off += kChunk)
    {
        const int m = juce::jmin (kChunk, n - off);
        float* cio[2] = { io[0] ? io[0] + off : nullptr, io[1] ? io[1] + off : nullptr };
        float* csc[2] = { sc[0] ? sc[0] + off : nullptr, sc[1] ? sc[1] + off : nullptr };

        // 1. alignment delay (only while ducking): vocal and music sidechain alike
        if (A > 0)
        {
            float* lines[4] = { cio[0], cio[1], csc[0], csc[1] };
            for (int c = 0; c < 4; ++c)
            {
                if (lines[c] == nullptr) continue;
                float* ring = delayBuf.getWritePointer (c);
                for (int i = 0; i < m; ++i) ring[(delayWrite + i) & delayMask] = lines[c][i];
                for (int i = 0; i < m; ++i) lines[c][i] = ring[(delayWrite + i - A) & delayMask];
            }
            delayWrite = (delayWrite + m) & delayMask;
        }

        // 2. the rider, exactly as without the duck
        engine.process (cio, ch, scCh > 0 ? csc : nullptr, scCh, m, s, &telemetry);

        // 3. the ducker, right after the rider, keyed from the Ecosystem channel at the
        //    timeline position this output corresponds to
        const float* kl = nullptr; const float* kr = nullptr;
        const int64_t outPos = pos + off - A;
        if (d.on && bus != nullptr && moving)
        {
            auto r = eco::read (bus, duckCh, outPos, m, srHz, ecoId, keyBuf.getWritePointer (0), keyBuf.getWritePointer (1));
            heardSenders = juce::jmax (heardSenders, r.senders);
            missing += r.missing; expected += (long) r.senders * m;
            kl = keyBuf.getReadPointer (0); kr = keyBuf.getReadPointer (1);
        }
        ducker.process (cio, ch, kl, kr, m, d, &telemetry);

        // 4. output trim
        for (int i = 0; i < m; ++i)
        {
            const float g = outGain.getNextValue();
            for (int c = 0; c < ch; ++c) { cio[c][i] *= g; peak = juce::jmax (peak, std::abs (cio[c][i])); }
        }

        // 5. publish the sound it made
        if (sendCh > 0 && bus != nullptr && moving && sender.active() && ch > 0)
            sender.write (outPos, cio[0], ch > 1 ? cio[1] : cio[0], m);
    }

    if (moving)
    {
        duckSenders.store (heardSenders);
        const float pct = expected > 0 ? 100.f * (float) missing / (float) expected : 0.f;
        duckLatePct.store (duckLatePct.load() + 0.1f * (pct - duckLatePct.load()));
    }
    sidechainConnected.store (telemetry.musicDb.load() > -70.f);
    if (peak > outPeak.load()) outPeak.store (peak);
    if (peak > 1.0f) clipped.store (true);
}

// Message thread, 50 Hz: rider automation writing + Ecosystem housekeeping.
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

    // latency follows the duck (0 when off) and the Align setting
    const int a = alignForSettings();
    if (a != curAlign.load()) { curAlign.store (a); setLatencySamples (a); }

    // send slot: claimed only while sending
    const int sendCh = getSendChannel();
    if (sendCh > 0 && bus != nullptr)
    {
        if (! sender.active()) sender.claim (bus, ecoId, (uint32_t) juce::roundToInt (sr));
        sender.setChannel (sendCh);
        sender.heartbeat();
    }
    else if (sender.active()) sender.release();

    if (! timelineMoving.load() && getDuckChannel() > 0)
        duckSenders.store (eco::countSenders (bus, getDuckChannel(), ecoId));

    static constexpr int kNamesEvery = 25;   // 0.5 s
    if (++nameTick >= kNamesEvery) { nameTick = 0; syncNames (false); }
}

//==============================================================================
void RiderProcessor::syncNames (bool force)
{
    if (bus == nullptr) { if (force) ++namesVersion; return; }
    char t[eco::kNumChannels][eco::kNameLen];
    const int64_t stamp = eco::readNames (bus, t);
    if (force || stamp != namesEditedMs || std::memcmp (t, names, sizeof (names)) != 0)
    {
        std::memcpy (names, t, sizeof (names));
        namesEditedMs = stamp;
        ++namesVersion;
    }
}

juce::String RiderProcessor::channelLabel (int ch) const
{
    if (ch <= 0 || ch > eco::kNumChannels) return "Off";
    const juce::String num = chText (ch);
    const juce::String nm = juce::String::fromUTF8 (names[ch - 1]);
    return nm.isEmpty() ? num : num + " " + nm;
}

void RiderProcessor::renameChannel (int ch, const juce::String& name)
{
    if (ch < 1 || ch > eco::kNumChannels) return;
    const auto utf8 = name.trim().substring (0, eco::kNameLen - 1).toRawUTF8();
    if (bus != nullptr) { eco::renameChannel (bus, ch, utf8); syncNames (true); }
    else
    {
        std::memset (names[ch - 1], 0, eco::kNameLen);
        std::strncpy (names[ch - 1], utf8, eco::kNameLen - 1);
        namesEditedMs = eco::wallMs(); ++namesVersion;
    }
}

juce::String RiderProcessor::getEcoStatus() const
{
    juce::String s;
    const int dch = getDuckChannel(), sch = getSendChannel();
    if (bus == nullptr) return "Ecosystem bus unavailable in this process";
    if (dch <= 0) s << "Duck off  |  latency 0";
    else
    {
        const int n = duckSenders.load();
        s << "Duck from " << channelLabel (dch) << ": ";
        if (n == 0) s << "nothing is sending on this channel";
        else
        {
            s << n << (n == 1 ? " sender" : " senders");
            if (timelineMoving.load()) s << ", ducking " << juce::String (telemetry.duckGrDb.load(), 1) << " dB";
            else s << " (paused: transport stopped)";
        }
        s << "  |  latency " << curAlign.load() << " smp";
    }
    s << "\n" << (sch > 0 ? "Sending on " + channelLabel (sch) : juce::String ("Not sending"));
    if (dch > 0 && duckLatePct.load() > 2.f)
        s << "\nWARNING: " << juce::String (duckLatePct.load(), 0) << "% of the key arrives too late. Raise Align, or disable anticipative FX on these tracks.";
    return s;
}

//==============================================================================
void RiderProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    juce::ValueTree ecoTree ("ECO");
    for (int c = 0; c < eco::kNumChannels; ++c)
        ecoTree.setProperty ("n" + juce::String (c + 1), juce::String::fromUTF8 (names[c]), nullptr);
    ecoTree.setProperty ("edited", juce::String (namesEditedMs), nullptr);
    state.removeChild (state.getChildWithName ("ECO"), nullptr);
    state.appendChild (ecoTree, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void RiderProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            auto ecoTree = tree.getChildWithName ("ECO");
            if (ecoTree.isValid())
            {
                char t[eco::kNumChannels][eco::kNameLen] {};
                for (int c = 0; c < eco::kNumChannels; ++c)
                    std::strncpy (t[c], ecoTree.getProperty ("n" + juce::String (c + 1)).toString().toRawUTF8(), eco::kNameLen - 1);
                const int64_t stamp = ecoTree.getProperty ("edited").toString().getLargeIntValue();
                if (bus != nullptr) eco::offerNames (bus, t, stamp);
                else { std::memcpy (names, t, sizeof (names)); namesEditedMs = stamp; }
                tree.removeChild (ecoTree, nullptr);
            }
            apvts.replaceState (tree);
            syncNames (true);
            const int a = alignForSettings();
            if (a != curAlign.load()) { curAlign.store (a); setLatencySamples (a); }
        }
}

juce::AudioProcessorEditor* RiderProcessor::createEditor() { return new RiderEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RiderProcessor(); }
