#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

namespace ui
{
    const juce::Colour bg      { 0xff15171b };
    const juce::Colour panel   { 0xff1e2127 };
    const juce::Colour edge    { 0xff2c3038 };
    const juce::Colour text    { 0xffc9ced6 };
    const juce::Colour dim     { 0xff6c737e };
    const juce::Colour ride    { 0xff3ec7a5 };   // riding / vocal
    const juce::Colour target  { 0xffe8a33d };   // target & range
    const juce::Colour clip    { 0xffe5484d };
}

class RiderLookAndFeel : public juce::LookAndFeel_V4
{
public:
    RiderLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
};

// Row of mutually exclusive buttons bound to a choice parameter.
class ChoiceButtons : public juce::Component
{
public:
    ChoiceButtons (juce::RangedAudioParameter&, juce::StringArray labels, juce::Colour onColour);
    void resized() override;
private:
    juce::OwnedArray<juce::TextButton> buttons;
    juce::ParameterAttachment attachment;
};

// Horizontal vocal level meter with the draggable transparent Target screen.
class TargetMeter : public juce::Component
{
public:
    TargetMeter (RiderProcessor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void tick();
private:
    float dbToX (float db) const;
    float xToDb (float x) const;
    juce::Rectangle<float> bar() const;
    RiderProcessor& proc;
    juce::ParameterAttachment attachment;
    float targetDb = -21.f, dispLevel = -100.f, grabOffset = 0.f, downTarget = -21.f;
    bool dragging = false;
    static constexpr float kLo = -60.f, kHi = 0.f, kScreenHalf = 3.f;
};

// The Rider Fader: range handles (left), idle arrow (right), cap = applied gain.
class RiderFader : public juce::Component
{
public:
    RiderFader (RiderProcessor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
private:
    enum class Grab { none, min, max, idle, cap };
    juce::Rectangle<float> track() const;
    float dbToY (float db) const;
    float yToDb (float y) const;
    RiderProcessor& proc;
    juce::ParameterAttachment minAtt, maxAtt, idleAtt, riderAtt;
    float minDb = -6, maxDb = 6, idleDb = 0;
    Grab grab = Grab::none;
};

class Led : public juce::Component
{
public:
    void set (float brightness, juce::Colour c) { if (std::abs (b - brightness) > 0.004f || c != col) { b = brightness; col = c; repaint(); } }
    void paint (juce::Graphics&) override;
private:
    float b = 0; juce::Colour col;
};

class RiderEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit RiderEditor (RiderProcessor&);
    ~RiderEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
private:
    void timerCallback() override;
    void setupKnob (juce::Slider&, juce::Label&, const juce::String&);

    RiderProcessor& proc;
    RiderLookAndFeel lnf;
    TargetMeter meter;
    RiderFader fader;
    juce::Slider vsens, msens, output;
    juce::Label vsensLabel, msensLabel, outputLabel;
    juce::AudioProcessorValueTreeState::SliderAttachment vsensAtt, msensAtt, outputAtt;
    ChoiceButtons attack, mode;
    Led rideLed, musicLed, clipLed;
    float outDisp = 0.f;
    juce::Rectangle<int> outMeterArea, clipArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RiderEditor)
};
