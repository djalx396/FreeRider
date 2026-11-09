// RiderEngine.h — automatic vocal gain rider (Waves Vocal Rider–style behaviour)
//
// No JUCE dependency, so it can be tested offline. Everything is defined in
// seconds/dB and recomputed from the sample rate, so behaviour is identical
// at 44.1, 48, 88.2, 96, 176.4 and 192 kHz. Zero latency: no lookahead.
//
// Signal flow (per sample):
//   vocal in ─► weighting (HPF 100 Hz + 1.5 kHz +4 dB shelf) ─► power (L²+R²)/2
//     ├─► 10 ms mean-square ─► gate level ─► vocal-activity gate → Ride / Idle
//     │      opens above Target − 24 − Vocal Sensitivity; a word ends when the level
//     │      drops 20 dB under the word's peak (or under the threshold); hold, then Idle
//     └─► peak-style follower (fast up / slow down, set by Attack) ─► ride level
//   music sidechain ─► same weighting ─► slow mean-square ─► Music offset on Target
//   Ride:  fader target = clamp(TargetEff − ride level, RangeMin, RangeMax)
//   Word ending / hold: fader frozen (no riding up into breaths)
//   Idle:  fader glides to the Idle arrow
//   fader ─► one-pole glide (Attack Fast/Slow) ─► mode switch (Off / Write / Read / manual)
//
// All the time constants below are first-guess values. They are what the
// capture kit is for: renders of the real plugin give its gain curve directly
// (output ÷ input), and these numbers get fitted to it.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>

namespace rider {

constexpr double kPi = 3.14159265358979323846;

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }

    void setHighpass (double fs, double f0, double q)
    {
        const double w = 2 * kPi * f0 / fs, c = std::cos (w), al = std::sin (w) / (2 * q);
        const double a0 = 1 + al;
        b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0;
        a1 = -2 * c / a0;      a2 = (1 - al) / a0;
    }
    void setHighShelf (double fs, double f0, double gainDb, double q = 0.7071)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = 2 * kPi * f0 / fs;
        const double c = std::cos (w), s = std::sin (w), al = s / (2 * q), sq = 2 * std::sqrt (A) * al;
        const double a0 = (A + 1) - (A - 1) * c + sq;
        b0 =  A * ((A + 1) + (A - 1) * c + sq) / a0;
        b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
        b2 =  A * ((A + 1) + (A - 1) * c - sq) / a0;
        a1 =  2 * ((A - 1) - (A + 1) * c) / a0;
        a2 = ((A + 1) - (A - 1) * c - sq) / a0;
    }
};

struct Weighting
{
    Biquad hp, shelf;
    void prepare (double fs) { hp.setHighpass (fs, 100.0, 0.7071); shelf.setHighShelf (fs, 1500.0, 4.0); reset(); }
    void reset() { hp.reset(); shelf.reset(); }
    inline double process (double x) noexcept { return shelf.process (hp.process (x)); }
};

enum class AttackMode { Slow = 0, Fast = 1 };
enum class AutoMode   { Off = 0, Write = 1, Read = 2 };

struct Settings
{
    double targetDb   = -21.0;  // 0 … -42 dBFS
    double vocalSens  =   0.0;  // -12 … +12 dB (higher = picks up quieter material as vocal)
    double musicSens  =   0.0;  // -12 … +12 dB (higher = music lifts the vocal more)
    double rangeMin   =  -6.0;  // -12 … +11 dB
    double rangeMax   =   6.0;  // -11 … +12 dB
    double idleDb     =   0.0;  // -12 … +12 dB (clamped into the range)
    AttackMode attack = AttackMode::Slow;
    AutoMode   mode   = AutoMode::Off;
    double readDb     =   0.0;  // Rider Fader parameter value, used in Read mode
    bool   manual     = false;  // fader being held by the user
    double manualDb   =   0.0;
};

// Values the editor reads (written once per block by the audio thread).
struct Telemetry
{
    std::atomic<float> levelDb   { -100.f };  // detected vocal level
    std::atomic<float> rangeLoDb { -100.f };  // recent vocal range (while riding)
    std::atomic<float> rangeHiDb { -100.f };
    std::atomic<float> riderDb   { 0.f };     // fader position actually applied
    std::atomic<float> autoDb    { 0.f };     // what the internal engine wants
    std::atomic<float> musicDb   { -100.f };  // sidechain level
    std::atomic<float> musicLed  { 0.f };     // 0…1 brightness
    std::atomic<float> targetEff { -21.f };   // Target after music offset
    std::atomic<bool>  riding    { false };
    std::atomic<float> duckGrDb  { 0.f };     // ducker gain reduction (positive dB)
    std::atomic<float> keyDb     { -100.f };  // duck key level
};

class Engine
{
public:
    // ---- tuning (first guesses; to be fitted against captures) ---------------
    //            follower att/rel, fader glide, hold, idle glide, onset (down-only) window
    struct Times { double att, rel, ride, hold, idle, onset; };
    static constexpr Times kSlow { 0.030, 0.300, 0.150, 0.250, 0.600, 0.060 };
    static constexpr Times kFast { 0.008, 0.150, 0.050, 0.120, 0.300, 0.020 };
    static constexpr double kGateTau        = 0.010;
    static constexpr double kCalDb          = 3.0103; // sine reads its peak level
    static constexpr double kAbsBelowTarget = 24.0;   // gate threshold at sens 0, relative to Target
    static constexpr double kHysteresis     = 3.0;
    static constexpr double kEndDrop        = 20.0;   // word ends this far under its peak
    static constexpr double kWordPkDecayDbPerS = 6.0;
    static constexpr double kMusicTau       = 0.300, kMusicSmoothTau = 1.0;
    static constexpr double kMusicRefBelowTarget = 6.0, kMusicSlope = 0.5, kMusicMaxLift = 12.0;
    static constexpr double kReadTau        = 0.005;  // de-zipper for automation / manual moves

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        for (auto& w : vocW) w.prepare (fs);
        for (auto& w : musW) w.prepare (fs);
        musicCoef  = coef (kMusicTau);
        musicSCoef = coef (kMusicSmoothTau);
        readCoef   = coef (kReadTau);
        gateCoef   = coef (kGateTau);
        wordPkDecayPerSample = kWordPkDecayDbPerS / fs;
        rangeDecayPerSample = 3.0 / fs;  // display range relaxes 3 dB/s
        setAttack (AttackMode::Slow);
        reset();
    }

    void reset()
    {
        for (auto& w : vocW) w.reset();
        for (auto& w : musW) w.reset();
        msG = 0; msR = 0; mms = 0; musicDb = -100; level = -100; gateLevel = -100; wordPk = -100;
        active = false; holding = false; holdLeft = 0; onsetLeft = 0;
        autoDb = 0; appliedDb = 0; rangeLo = rangeHi = -100;
    }

    // Process a block in place. vocal: numCh channels; sc may be null.
    // Returns nothing; gain is applied to `io`, output trim is NOT applied here.
    void process (float* const* io, int numCh, const float* const* sc, int scCh, int n, const Settings& s, Telemetry* t)
    {
        if (s.attack != curAttack) setAttack (s.attack);

        const double rMin = std::min (s.rangeMin, s.rangeMax - 1.0);
        const double rMax = std::max (s.rangeMax, rMin + 1.0);
        const double idle = std::clamp (s.idleDb, rMin, rMax);
        const double absThr = s.targetDb - kAbsBelowTarget - s.vocalSens;
        const int nv = std::clamp (numCh, 0, kMaxCh);
        const int ns = std::clamp (scCh, 0, kMaxCh);
        const double invCh = 1.0 / std::max (1, nv);
        const double invSc = 1.0 / std::max (1, ns);
        double lift = 0;

        for (int i = 0; i < n; ++i)
        {
            // --- vocal level --------------------------------------------------
            // Power average of the weighted channels: (L²+R²)/2, so a wide or
            // out-of-phase stereo vocal is measured correctly.
            double p = 0;
            for (int c = 0; c < nv; ++c) { const double w = vocW[c].process (io[c][i]); p += w * w; }
            p *= invCh;
            msG += gateCoef * (p - msG);
            msR += (msG > msR ? attCoef : relCoef) * (msG - msR);
            gateLevel = 10.0 * std::log10 (msG + 1e-20) + kCalDb;
            level     = 10.0 * std::log10 (msR + 1e-20) + kCalDb;

            // --- music sidechain ------------------------------------------------
            if (sc != nullptr && scCh > 0)
            {
                double pm = 0;
                for (int c = 0; c < ns; ++c) { const double w = musW[c].process (sc[c][i]); pm += w * w; }
                pm *= invSc;
                mms += musicCoef * (pm - mms);
                const double md = 10.0 * std::log10 (mms + 1e-20) + kCalDb;
                musicDb += musicSCoef * (md - musicDb);
            }
            else
                musicDb += musicSCoef * (-100.0 - musicDb);

            lift = 0;
            if (musicDb > -70.0)
                lift = std::clamp (kMusicSlope * (musicDb + s.musicSens - (s.targetDb - kMusicRefBelowTarget)), 0.0, kMusicMaxLift);
            const double targetEff = s.targetDb + lift;

            // --- vocal activity gate --------------------------------------------
            if (! active)
            {
                if (gateLevel > absThr)
                {
                    active = true; holding = false; onsetLeft = onsetSamples; wordPk = gateLevel;
                }
            }
            else
            {
                wordPk = std::max (gateLevel, wordPk - wordPkDecayPerSample);
                const double endThr = std::max (absThr - kHysteresis, wordPk - kEndDrop);
                if (gateLevel > endThr) { holding = false; holdLeft = holdSamples; }
                else
                {
                    holding = true;
                    if (--holdLeft <= 0) { active = false; holding = false; }
                }
            }

            // --- internal ride ----------------------------------------------------
            if (active && ! holding)
            {
                const double want = std::clamp (targetEff - level, rMin, rMax);
                // First moments of a word: the detector is still rising, so only let
                // the fader come down (catch loud starts), never jump up.
                if (onsetLeft > 0) { --onsetLeft; if (want < autoDb) autoDb += rideCoef * (want - autoDb); }
                else autoDb += rideCoef * (want - autoDb);

                if (level > rangeHi) rangeHi = level; else rangeHi -= rangeDecayPerSample;
                if (level < rangeLo || rangeLo < -99) rangeLo = level; else rangeLo += rangeDecayPerSample;
                if (rangeLo > rangeHi) rangeLo = rangeHi;
            }
            else if (! active)
                autoDb += idleCoef * (idle - autoDb);
            // holding: fader stays where it is

            autoDb = std::clamp (autoDb, rMin, rMax);

            // --- what actually gets applied --------------------------------------
            double dest;
            if (s.manual)                       dest = s.manualDb;
            else if (s.mode == AutoMode::Read)  dest = s.readDb;
            else                                dest = autoDb;   // Off and Write

            if (s.manual || s.mode == AutoMode::Read) appliedDb += readCoef * (dest - appliedDb);
            else                                      appliedDb = dest;

            const float g = (float) std::exp (appliedDb * 0.11512925464970229); // ln(10)/20
            for (int c = 0; c < numCh; ++c) io[c][i] *= g;
        }

        if (t != nullptr)
        {
            t->levelDb.store ((float) level);
            t->rangeLoDb.store ((float) rangeLo);
            t->rangeHiDb.store ((float) rangeHi);
            t->riderDb.store ((float) appliedDb);
            t->autoDb.store ((float) autoDb);
            t->musicDb.store ((float) musicDb);
            t->musicLed.store ((float) std::clamp (lift / 6.0, 0.0, 1.0));
            t->targetEff.store ((float) (s.targetDb + lift));
            t->riding.store (active);
        }
    }

    double getAutoDb() const noexcept { return autoDb; }
    double getAppliedDb() const noexcept { return appliedDb; }
    bool   isRiding() const noexcept { return active; }
    double getLevelDb() const noexcept { return level; }

private:
    double coef (double tau) const { return 1.0 - std::exp (-1.0 / (tau * fs)); }

    void setAttack (AttackMode a)
    {
        curAttack = a;
        const Times& T = (a == AttackMode::Fast) ? kFast : kSlow;
        attCoef  = coef (T.att);
        relCoef  = coef (T.rel);
        rideCoef = coef (T.ride);
        idleCoef = coef (T.idle);
        holdSamples  = (int) std::lround (T.hold * fs);
        onsetSamples = (int) std::lround (T.onset * fs);
    }

    double fs = 48000;
    static constexpr int kMaxCh = 2;
    Weighting vocW[kMaxCh], musW[kMaxCh];
    AttackMode curAttack = AttackMode::Slow;
    double attCoef = 0, relCoef = 0, rideCoef = 0, idleCoef = 0, gateCoef = 0;
    double musicCoef = 0, musicSCoef = 0, readCoef = 0;
    double wordPkDecayPerSample = 0, rangeDecayPerSample = 0;
    int holdSamples = 0, onsetSamples = 0;

    double msG = 0, msR = 0, mms = 0, level = -100, gateLevel = -100, wordPk = -100, musicDb = -100;
    bool active = false, holding = false; int holdLeft = 0, onsetLeft = 0;
    double autoDb = 0, appliedDb = 0, rangeLo = -100, rangeHi = -100;
};

//==============================================================================
// Ducker: sits right after the rider. Listens to a key (an Ecosystem channel) and
// pulls the vocal down while the key is above the threshold, by up to Depth dB.
// Its key detector is the rider's own (same weighting, same 10 ms gate RMS, same
// peak-style follower), and it follows the rider's Attack switch, so Fast/Slow
// set the duck speed exactly as they set the riding speed:
//   Slow: 30 ms down / 300 ms back up      Fast: 8 ms down / 150 ms back up
//   reduction = clamp(keyLevel − Threshold, 0, Depth)   (1 dB of duck per dB over)
struct DuckSettings
{
    bool   on        = false;   // a channel is selected
    double depthDb   = 6.0;     // 0 … 24
    double threshDb  = -30.0;   // -60 … 0 dBFS
    AttackMode attack = AttackMode::Slow;
};

class Ducker
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        for (auto& w : keyW) w.prepare (fs);
        gateCoef = coef (Engine::kGateTau);
        setAttack (AttackMode::Slow);
        reset();
    }
    void reset() { for (auto& w : keyW) w.reset(); msG = msR = 0; grDb = 0; keyDb = -100; }

    // keyL/keyR may be null (no key: the duck lets go at the release speed).
    void process (float* const* io, int numCh, const float* keyL, const float* keyR, int n,
                  const DuckSettings& d, Telemetry* t)
    {
        if (d.attack != curAttack) setAttack (d.attack);
        const bool haveKey = d.on && keyL != nullptr;
        const double depth = d.on ? std::clamp (d.depthDb, 0.0, 24.0) : 0.0;
        for (int i = 0; i < n; ++i)
        {
            double p = 0;
            if (haveKey)
            {
                const double a = keyW[0].process (keyL[i]);
                const double b = keyW[1].process (keyR != nullptr ? keyR[i] : keyL[i]);
                p = 0.5 * (a * a + b * b);
            }
            msG += gateCoef * (p - msG);
            msR += (msG > msR ? attCoef : relCoef) * (msG - msR);
            keyDb = 10.0 * std::log10 (msR + 1e-20) + Engine::kCalDb;
            grDb = std::clamp (keyDb - d.threshDb, 0.0, depth);
            if (grDb > 1e-4)
            {
                const float g = (float) std::exp (-grDb * 0.11512925464970229);
                for (int c = 0; c < numCh; ++c) io[c][i] *= g;
            }
        }
        if (t != nullptr)
        {
            t->duckGrDb.store ((float) grDb);
            t->keyDb.store ((float) keyDb);
        }
    }

    double getGrDb() const noexcept { return grDb; }

private:
    double coef (double tau) const { return 1.0 - std::exp (-1.0 / (tau * fs)); }
    void setAttack (AttackMode a)
    {
        curAttack = a;
        const auto& T = (a == AttackMode::Fast) ? Engine::kFast : Engine::kSlow;
        attCoef = coef (T.att);
        relCoef = coef (T.rel);
    }
    double fs = 48000;
    Weighting keyW[2];
    AttackMode curAttack = AttackMode::Slow;
    double gateCoef = 0, attCoef = 0, relCoef = 0;
    double msG = 0, msR = 0, grDb = 0, keyDb = -100;
};

} // namespace rider
