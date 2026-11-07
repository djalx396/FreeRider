#include "PluginEditor.h"

using namespace juce;

static Font uiFont (float h, bool bold = false) { return Font (FontOptions (h, bold ? Font::bold : Font::plain)); }

static String dbText (float v)
{
    return (v > 0.04f ? "+" : "") + String (v, 1) + " dB";
}

//==============================================================================
RiderLookAndFeel::RiderLookAndFeel()
{
    setColour (Slider::textBoxTextColourId, ui::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxBackgroundColourId, ui::bg);
    setColour (Label::textColourId, ui::text);
    setColour (TextButton::buttonColourId, ui::panel.brighter (0.05f));
    setColour (TextButton::textColourOffId, ui::dim);
    setColour (TextButton::textColourOnId, Colours::black);
}

void RiderLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, Slider& s)
{
    auto r = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (6);
    const float d = jmin (r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre (d, d);
    const auto c = r.getCentre();
    const float rad = d * 0.5f, ang = a0 + pos * (a1 - a0);
    const float zero = a0 + (float) s.valueToProportionOfLength (0.0) * (a1 - a0);

    Path bgArc; bgArc.addCentredArc (c.x, c.y, rad - 3, rad - 3, 0, a0, a1, true);
    g.setColour (ui::edge); g.strokePath (bgArc, PathStrokeType (4.f, PathStrokeType::curved, PathStrokeType::rounded));
    Path valArc; valArc.addCentredArc (c.x, c.y, rad - 3, rad - 3, 0, jmin (zero, ang), jmax (zero, ang), true);
    g.setColour (ui::ride); g.strokePath (valArc, PathStrokeType (4.f, PathStrokeType::curved, PathStrokeType::rounded));

    g.setColour (ui::panel.brighter (0.12f)); g.fillEllipse (r.reduced (9));
    g.setColour (ui::edge); g.drawEllipse (r.reduced (9), 1.f);
    Path ptr; ptr.addRoundedRectangle (-1.5f, -(rad - 11), 3.f, rad * 0.45f, 1.5f);
    g.setColour (ui::text); g.fillPath (ptr, AffineTransform::rotation (ang).translated (c.x, c.y));
}

void RiderLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                         Slider::SliderStyle, Slider&)
{
    const float cx = (float) x + (float) w * 0.5f;
    g.setColour (ui::bg); g.fillRoundedRectangle (cx - 3, (float) y, 6, (float) h, 3);
    g.setColour (ui::edge); g.drawRoundedRectangle (cx - 3, (float) y, 6, (float) h, 3, 1);
    auto cap = Rectangle<float> (cx - 18, pos - 9, 36, 18);
    g.setColour (ui::panel.brighter (0.25f)); g.fillRoundedRectangle (cap, 3);
    g.setColour (ui::text); g.fillRect (cap.getX() + 4, pos - 0.75f, cap.getWidth() - 8, 1.5f);
}

void RiderLookAndFeel::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1);
    const bool on = b.getToggleState();
    Colour c = on ? b.findColour (TextButton::buttonOnColourId) : ui::panel.brighter (0.06f);
    if (over && ! on) c = c.brighter (0.08f);
    if (down) c = c.darker (0.1f);
    g.setColour (c); g.fillRoundedRectangle (r, 4);
    g.setColour (on ? c.brighter (0.2f) : ui::edge); g.drawRoundedRectangle (r, 4, 1);
}

//==============================================================================
ChoiceButtons::ChoiceButtons (RangedAudioParameter& p, StringArray labels, Colour onColour)
    : attachment (p, [this] (float v)
      {
          const int sel = roundToInt (v);
          for (int i = 0; i < buttons.size(); ++i) buttons[i]->setToggleState (i == sel, dontSendNotification);
      })
{
    for (int i = 0; i < labels.size(); ++i)
    {
        auto* b = buttons.add (new TextButton (labels[i]));
        b->setColour (TextButton::buttonOnColourId, onColour);
        b->onClick = [this, i] { attachment.setValueAsCompleteGesture ((float) i); };
        addAndMakeVisible (b);
    }
    attachment.sendInitialUpdate();
}

void ChoiceButtons::resized()
{
    auto r = getLocalBounds();
    const int w = r.getWidth() / jmax (1, buttons.size());
    for (auto* b : buttons) b->setBounds (r.removeFromLeft (w).reduced (2, 0));
}

//==============================================================================
TargetMeter::TargetMeter (RiderProcessor& p)
    : proc (p), attachment (*p.apvts.getParameter (ID::target), [this] (float v) { targetDb = v; repaint(); })
{
    attachment.sendInitialUpdate();
}

Rectangle<float> TargetMeter::bar() const { return getLocalBounds().toFloat().reduced (14, 0).withTrimmedTop (22).withTrimmedBottom (20); }
float TargetMeter::dbToX (float db) const { auto b = bar(); return b.getX() + (jlimit (kLo, kHi, db) - kLo) / (kHi - kLo) * b.getWidth(); }
float TargetMeter::xToDb (float x) const  { auto b = bar(); return kLo + (x - b.getX()) / b.getWidth() * (kHi - kLo); }

void TargetMeter::tick()
{
    const float l = proc.telemetry.levelDb.load();
    dispLevel = l > dispLevel ? l : jmax (l, dispLevel - 1.5f);
    repaint();
}

void TargetMeter::paint (Graphics& g)
{
    auto all = getLocalBounds().toFloat();
    g.setColour (ui::panel); g.fillRoundedRectangle (all, 6);
    auto b = bar();
    g.setColour (ui::bg); g.fillRoundedRectangle (b, 3);

    g.setFont (uiFont (10.f));
    for (int db = (int) kLo; db <= (int) kHi; db += 6)
    {
        const float x = dbToX ((float) db);
        g.setColour (ui::edge); g.drawVerticalLine ((int) x, b.getY(), b.getBottom());
        g.setColour (ui::dim);
        g.drawText (String (db), Rectangle<float> (x - 15, b.getBottom() + 3, 30, 12), Justification::centred);
    }

    const bool riding = proc.telemetry.riding.load();
    if (dispLevel > kLo)
    {
        auto lv = b.reduced (0, 6).withRight (dbToX (dispLevel));
        g.setColour (riding ? ui::ride : ui::dim); g.fillRoundedRectangle (lv, 2);
    }

    const float lo = proc.telemetry.rangeLoDb.load(), hi = proc.telemetry.rangeHiDb.load();
    if (hi > kLo && lo > -99.f)
    {
        g.setColour (ui::ride.withAlpha (0.8f));
        g.fillRect (Rectangle<float> (dbToX (lo), b.getY() + 2, jmax (2.f, dbToX (hi) - dbToX (lo)), 3.f));
    }

    // Target "transparent screen"
    auto scr = Rectangle<float> (dbToX (targetDb - kScreenHalf), b.getY() - 4, dbToX (targetDb + kScreenHalf) - dbToX (targetDb - kScreenHalf), b.getHeight() + 8);
    g.setColour (ui::target.withAlpha (dragging ? 0.30f : 0.18f)); g.fillRoundedRectangle (scr, 3);
    g.setColour (ui::target); g.drawRoundedRectangle (scr, 3, 1.5f);
    g.fillRect (dbToX (targetDb) - 0.75f, scr.getY(), 1.5f, scr.getHeight());

    const float te = proc.telemetry.targetEff.load();
    if (te - targetDb > 0.1f)
    {
        g.setColour (ui::ride.brighter (0.3f));
        const float x = dbToX (te);
        for (float yy = scr.getY(); yy < scr.getBottom(); yy += 6) g.fillRect (x - 0.75f, yy, 1.5f, 3.f);
    }

    g.setFont (uiFont (11.f, true));
    g.setColour (ui::dim); g.drawText ("VOCAL LEVEL", Rectangle<float> (b.getX(), 3, 150, 16), Justification::centredLeft);
    g.setColour (ui::target);
    g.drawText ("TARGET  " + String (targetDb, 1) + " dBFS", Rectangle<float> (b.getRight() - 220, 3, 220, 16), Justification::centredRight);
}

void TargetMeter::mouseDown (const MouseEvent& e)
{
    dragging = true;
    const float d = xToDb ((float) e.x);
    grabOffset = std::abs (d - targetDb) <= kScreenHalf ? targetDb - d : 0.f;  // grab the screen where clicked
    downTarget = targetDb;
    attachment.beginGesture();
    attachment.setValueAsPartOfGesture (jlimit (-42.f, 0.f, d + grabOffset));
}
void TargetMeter::mouseDrag (const MouseEvent& e)
{
    // Shift = fine (a quarter of the movement)
    const float d = e.mods.isShiftDown() ? downTarget + (xToDb ((float) e.x) - xToDb ((float) e.getMouseDownX())) * 0.25f
                                         : xToDb ((float) e.x) + grabOffset;
    attachment.setValueAsPartOfGesture (jlimit (-42.f, 0.f, std::round (d * 10.f) / 10.f));
}
void TargetMeter::mouseUp (const MouseEvent&) { attachment.endGesture(); dragging = false; repaint(); }
void TargetMeter::mouseDoubleClick (const MouseEvent&) { attachment.setValueAsCompleteGesture (-21.f); }

//==============================================================================
RiderFader::RiderFader (RiderProcessor& p)
    : proc (p),
      minAtt   (*p.apvts.getParameter (ID::rmin),  [this] (float v) { minDb = v;  repaint(); }),
      maxAtt   (*p.apvts.getParameter (ID::rmax),  [this] (float v) { maxDb = v;  repaint(); }),
      idleAtt  (*p.apvts.getParameter (ID::idle),  [this] (float v) { idleDb = v; repaint(); }),
      riderAtt (*p.apvts.getParameter (ID::rider), [] (float) {})
{
    minAtt.sendInitialUpdate(); maxAtt.sendInitialUpdate(); idleAtt.sendInitialUpdate();
}

Rectangle<float> RiderFader::track() const
{
    auto r = getLocalBounds().toFloat();
    return { r.getX() + r.getWidth() * 0.30f, 26.f, r.getWidth() * 0.40f, r.getHeight() - 26.f - 44.f };
}
float RiderFader::dbToY (float db) const { auto t = track(); return t.getBottom() - (jlimit (-12.f, 12.f, db) + 12.f) / 24.f * t.getHeight(); }
float RiderFader::yToDb (float y) const  { auto t = track(); return jlimit (-12.f, 12.f, (t.getBottom() - y) / t.getHeight() * 24.f - 12.f); }

void RiderFader::paint (Graphics& g)
{
    auto all = getLocalBounds().toFloat();
    g.setColour (ui::panel); g.fillRoundedRectangle (all, 6);
    auto t = track();
    const float lo = jmin (minDb, maxDb - 1.f), hi = jmax (maxDb, lo + 1.f);

    g.setFont (uiFont (11.f, true)); g.setColour (ui::dim);
    g.drawText ("RIDER", all.withHeight (22).withTrimmedTop (4), Justification::centred);

    g.setColour (ui::bg); g.fillRoundedRectangle (t, 4);
    // effective range
    g.setColour (ui::target.withAlpha (0.10f));
    g.fillRect (Rectangle<float> (t.getX(), dbToY (hi), t.getWidth(), dbToY (lo) - dbToY (hi)));

    g.setFont (uiFont (9.5f));
    for (int db = -12; db <= 12; db += 3)
    {
        const float y = dbToY ((float) db);
        g.setColour (db == 0 ? ui::dim : ui::edge);
        g.drawHorizontalLine ((int) y, t.getX(), t.getX() + (db % 6 == 0 ? 12.f : 7.f));
        g.drawHorizontalLine ((int) y, t.getRight() - (db % 6 == 0 ? 12.f : 7.f), t.getRight());
        if (db % 6 == 0)
        {
            g.setColour (ui::dim);
            g.drawText ((db > 0 ? "+" : "") + String (db), Rectangle<float> (t.getX() + 13, y - 6, 24, 12), Justification::centredLeft);
        }
    }

    // range handles (left)
    auto handle = [&] (float db, bool top)
    {
        const float y = dbToY (db), xr = t.getX() - 4, xl = all.getX() + 10;
        g.setColour (ui::target.withAlpha (0.5f));
        g.drawLine (xr, y, xr + 6, y, 1.f);
        Path p; p.addTriangle (xr, y, xr - 12, y - 7, xr - 12, y + 7);
        g.setColour (ui::target); g.fillPath (p);
        g.setFont (uiFont (10.f));
        g.drawText (dbText (db).upToFirstOccurrenceOf (" ", false, false), Rectangle<float> (xl, y + (top ? -18.f : 6.f), xr - xl - 4, 12), Justification::centredRight);
    };
    g.setColour (ui::target.withAlpha (0.35f));
    g.fillRect (t.getX() - 9, dbToY (hi), 2.f, dbToY (lo) - dbToY (hi));
    handle (hi, true); handle (lo, false);

    // idle arrow (right)
    {
        const float y = dbToY (jlimit (lo, hi, idleDb)), xl = t.getRight() + 4;
        Path p; p.addTriangle (xl, y, xl + 12, y - 7, xl + 12, y + 7);
        g.setColour (ui::text.withAlpha (0.8f)); g.fillPath (p);
        g.setFont (uiFont (9.5f));
        g.drawText ("IDLE", Rectangle<float> (xl + 14, y - 6, all.getRight() - xl - 16, 12), Justification::centredLeft);
    }

    // the fader cap
    const float applied = proc.telemetry.riderDb.load();
    const bool manual = proc.manualActive.load();
    const bool riding = proc.telemetry.riding.load();
    auto cap = Rectangle<float> (t.getX() - 2, dbToY (applied) - 11, t.getWidth() + 4, 22);
    g.setColour (ui::panel.brighter (manual ? 0.45f : 0.3f)); g.fillRoundedRectangle (cap, 4);
    g.setColour (manual ? ui::target : (riding ? ui::ride : ui::dim)); g.drawRoundedRectangle (cap, 4, 1.5f);
    g.fillRect (cap.getX() + 6, cap.getCentreY() - 0.75f, cap.getWidth() - 12, 1.5f);

    g.setFont (uiFont (15.f, true)); g.setColour (ui::text);
    g.drawText (dbText (applied), Rectangle<float> (all.getX(), t.getBottom() + 8, all.getWidth(), 18), Justification::centred);
    g.setFont (uiFont (9.5f)); g.setColour (ui::dim);
    g.drawText ("range " + String (hi - lo, 0) + " dB", Rectangle<float> (all.getX(), t.getBottom() + 26, all.getWidth(), 12), Justification::centred);
}

void RiderFader::mouseDown (const MouseEvent& e)
{
    auto t = track();
    const float x = (float) e.x, y = (float) e.y;
    if (x < t.getX() - 2)
        grab = std::abs (y - dbToY (maxDb)) < std::abs (y - dbToY (minDb)) ? Grab::max : Grab::min;
    else if (x > t.getRight() + 2) grab = Grab::idle;
    else grab = Grab::cap;

    switch (grab)
    {
        case Grab::min:  minAtt.beginGesture();  break;
        case Grab::max:  maxAtt.beginGesture();  break;
        case Grab::idle: idleAtt.beginGesture(); break;
        case Grab::cap:
            if (proc.getMode() == rider::AutoMode::Read) { riderAtt.beginGesture(); riderAtt.setValueAsPartOfGesture (yToDb (y)); }
            proc.manualDb.store (yToDb (y));
            proc.manualActive.store (true);
            break;
        case Grab::none: break;
    }
}

void RiderFader::mouseDrag (const MouseEvent& e)
{
    const float d = std::round (yToDb ((float) e.y) * 10.f) / 10.f;
    switch (grab)
    {
        case Grab::min:  minAtt.setValueAsPartOfGesture (jlimit (-12.f, jmin (11.f, maxDb - 1.f), d)); break;
        case Grab::max:  maxAtt.setValueAsPartOfGesture (jlimit (jmax (-11.f, minDb + 1.f), 12.f, d)); break;
        case Grab::idle: idleAtt.setValueAsPartOfGesture (d); break;
        case Grab::cap:
            proc.manualDb.store (d);
            if (proc.getMode() == rider::AutoMode::Read) riderAtt.setValueAsPartOfGesture (d);
            break;
        case Grab::none: break;
    }
    repaint();
}

void RiderFader::mouseUp (const MouseEvent&)
{
    switch (grab)
    {
        case Grab::min:  minAtt.endGesture();  break;
        case Grab::max:  maxAtt.endGesture();  break;
        case Grab::idle: idleAtt.endGesture(); break;
        case Grab::cap:
            if (proc.getMode() == rider::AutoMode::Read) riderAtt.endGesture();
            proc.manualActive.store (false);   // automatic riding resumes on release
            break;
        case Grab::none: break;
    }
    grab = Grab::none;
}

void RiderFader::mouseDoubleClick (const MouseEvent& e)
{
    auto t = track();
    if (e.x < t.getX() - 2) { minAtt.setValueAsCompleteGesture (-6.f); maxAtt.setValueAsCompleteGesture (6.f); }
    else if (e.x > t.getRight() + 2) idleAtt.setValueAsCompleteGesture (0.f);
}

//==============================================================================
void Led::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1);
    g.setColour (ui::bg); g.fillEllipse (r);
    if (b > 0.01f)
    {
        g.setColour (col.withAlpha (0.25f * b)); g.fillEllipse (r.expanded (0));
        g.setColour (col.withAlpha (0.3f + 0.7f * b)); g.fillEllipse (r.reduced (2));
    }
    g.setColour (ui::edge); g.drawEllipse (r, 1);
}

//==============================================================================
RiderEditor::RiderEditor (RiderProcessor& p)
    : AudioProcessorEditor (&p), proc (p), meter (p), fader (p),
      vsensAtt (p.apvts, ID::vsens, vsens), msensAtt (p.apvts, ID::msens, msens), outputAtt (p.apvts, ID::output, output),
      attack (*p.apvts.getParameter (ID::attack), { "SLOW", "FAST" }, ui::ride),
      mode   (*p.apvts.getParameter (ID::mode),   { "OFF", "WRITE", "READ" }, ui::target)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (meter);
    addAndMakeVisible (fader);
    setupKnob (vsens, vsensLabel, "VOCAL SENS");
    setupKnob (msens, msensLabel, "MUSIC SENS");
    output.setSliderStyle (Slider::LinearVertical);
    output.setTextBoxStyle (Slider::TextBoxBelow, false, 74, 18);
    output.setDoubleClickReturnValue (true, 0.0);
    output.textFromValueFunction = [] (double v) { return dbText ((float) v); };
    output.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    output.updateText();
    addAndMakeVisible (output);
    addAndMakeVisible (attack);
    addAndMakeVisible (mode);
    for (auto* l : { &rideLed, &musicLed, &clipLed }) { l->setInterceptsMouseClicks (false, false); addAndMakeVisible (l); }
    setSize (780, 470);
    startTimerHz (30);
}

RiderEditor::~RiderEditor() { stopTimer(); setLookAndFeel (nullptr); }

void RiderEditor::setupKnob (Slider& s, Label& l, const String& name)
{
    s.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (Slider::TextBoxBelow, false, 64, 18);
    s.setDoubleClickReturnValue (true, 0.0);
    s.textFromValueFunction = [] (double v) { return dbText ((float) v); };
    s.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    s.updateText();
    addAndMakeVisible (s);
    l.setText (name, dontSendNotification);
    l.setFont (uiFont (11.f, true));
    l.setColour (Label::textColourId, ui::dim);
    l.setJustificationType (Justification::centred);
    addAndMakeVisible (l);
}

void RiderEditor::resized()
{
    meter.setBounds (20, 48, 740, 78);
    fader.setBounds (290, 140, 200, 310);

    vsensLabel.setBounds (30, 150, 110, 16);
    vsens.setBounds (35, 168, 100, 112);
    rideLed.setBounds (52, 290, 12, 12);
    msensLabel.setBounds (150, 150, 110, 16);
    msens.setBounds (155, 168, 100, 112);
    musicLed.setBounds (172, 290, 12, 12);
    attack.setBounds (36, 368, 218, 28);

    mode.setBounds (522, 172, 226, 28);
    output.setBounds (556, 250, 78, 190);
    outMeterArea = { 650, 258, 10, 160 };
    clipLed.setBounds (670, 258, 14, 14);
    clipArea = clipLed.getBounds().expanded (6);
}

void RiderEditor::mouseDown (const MouseEvent& e)
{
    if (clipArea.contains (e.getPosition())) proc.clipped.store (false);
}

void RiderEditor::timerCallback()
{
    meter.tick();
    fader.repaint();
    rideLed.set (proc.telemetry.riding.load() ? 1.f : 0.f, ui::ride);
    musicLed.set (proc.sidechainConnected.load() ? 0.15f + 0.85f * proc.telemetry.musicLed.load() : 0.f, ui::ride);
    clipLed.set (proc.clipped.load() ? 1.f : 0.f, ui::clip);
    outDisp = jmax (proc.outPeak.exchange (0.f), outDisp * 0.86f);
    repaint (outMeterArea.expanded (2));
    repaint (Rectangle<int> (20, 286, 250, 70));  // LED captions
    repaint (Rectangle<int> (510, 200, 250, 30));
}

void RiderEditor::paint (Graphics& g)
{
    g.fillAll (ui::bg);
    g.setFont (uiFont (20.f, true)); g.setColour (ui::text);
    g.drawText ("RIDER", 22, 10, 120, 28, Justification::centredLeft);
    g.setFont (uiFont (11.f)); g.setColour (ui::dim);
    g.drawText ("automatic vocal gain rider   |   v0.1", 104, 14, 300, 22, Justification::centredLeft);
    g.drawText (proc.getTotalNumInputChannels() > 2 ? "music sidechain: track ch 3/4" : "no sidechain bus",
                520, 14, 240, 22, Justification::centredRight);

    g.setColour (ui::panel);
    g.fillRoundedRectangle (20, 140, 250, 310, 6);
    g.fillRoundedRectangle (510, 140, 250, 310, 6);

    g.setFont (uiFont (10.f, true));
    const bool riding = proc.telemetry.riding.load();
    g.setColour (riding ? ui::ride : ui::dim);
    g.drawText (riding ? "RIDE" : "IDLE", 68, 288, 60, 16, Justification::centredLeft);
    const bool sc = proc.sidechainConnected.load();
    g.setColour (sc ? ui::ride : ui::dim);
    g.drawText (sc ? "MUSIC " + dbText (proc.telemetry.targetEff.load() - proc.apvts.getRawParameterValue (ID::target)->load()) : "NO MUSIC",
                188, 288, 82, 16, Justification::centredLeft);

    g.setColour (ui::dim);
    g.drawText ("ATTACK", 36, 348, 218, 16, Justification::centred);
    g.drawText ("AUTOMATION", 522, 152, 226, 16, Justification::centred);
    g.drawText ("OUTPUT", 556, 232, 78, 16, Justification::centred);
    g.drawText ("CLIP", 688, 258, 40, 14, Justification::centredLeft);

    const auto m = proc.getMode();
    g.setFont (uiFont (9.5f));
    g.drawText (m == rider::AutoMode::Write ? "arm the Rider Fader lane, host in Write/Touch/Latch"
              : m == rider::AutoMode::Read  ? "fader follows the automation lane"
                                            : "internal engine, no automation",
                512, 204, 246, 14, Justification::centred);

    // output meter
    auto om = outMeterArea.toFloat();
    g.setColour (ui::bg); g.fillRoundedRectangle (om, 2);
    const float db = Decibels::gainToDecibels (outDisp, -60.f);
    const float h = jlimit (0.f, 1.f, (db + 60.f) / 60.f) * om.getHeight();
    g.setColour (db > -0.1f ? ui::clip : ui::ride);
    g.fillRoundedRectangle (om.withTop (om.getBottom() - h), 2);
}
