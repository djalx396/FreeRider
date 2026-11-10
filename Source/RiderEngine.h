// RiderEngine.h — Free Rider audio engine (v0.3)
//
// No JUCE dependency, so it can be tested offline. Zero latency.
//
//   vocal in ─► detector filters (HPF + 1.5 kHz bell, fitted) ─► power, averaged over the channels
//   music sidechain ─► same filters ─► power
//   every ~1 ms chunk: RiderModel::step(vocal power, music power) → fader position
//   (the riding law fitted to Waves Vocal Rider; see RiderModel.h)
//   gain: across each chunk the fader glides from the value two chunks back to the value one
//         chunk back, so the result never depends on the host's block size
//   then the mode switch (Off / Write / Read / manual) and, in the plug-in, the ducker and Output.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include "RiderModel.h"

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
    void setBell (double fs, double f0, double gainDb, double q)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = 2 * kPi * f0 / fs;
        const double c = std::cos (w), al = std::sin (w) / (2 * q);
        const double a0 = 1 + al / A;
        b0 = (1 + al * A) / a0; b1 = -2 * c / a0; b2 = (1 - al * A) / a0;
        a1 = -2 * c / a0;       a2 = (1 - al / A) / a0;
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

// Detector filters of the riding model: 2nd-order high-pass + bell at 1.5 kHz (Q 0.7).
struct DetectorFilter
{
    Biquad hp, bell;
    void prepare (double fs, double hpf, double bellDb)
    {
        hp.setHighpass (fs, std::clamp (hpf, 10.0, 0.45 * fs), 0.7071);
        bell.setBell (fs, std::min (1500.0, 0.4 * fs), bellDb, 0.7);
        reset();
    }
    void reset() { hp.reset(); bell.reset(); }
    inline double process (double x) noexcept { return bell.process (hp.process (x)); }
};

// The ducker's key detector (unchanged from v0.2): HPF 100 Hz + 1.5 kHz +4 dB shelf.
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
    double vocalSens  =   0.0;  // -12 … +12 dB
    double musicSens  =   0.0;  // -12 … +12 dB
    double rangeMin   =  -6.0;  // -12 … +11 dB
    double rangeMax   =   6.0;  // -11 … +12 dB
    double idleDb     =   0.0;  // -12 … +12: position inside the range (-12 = Min, 0 = middle, +12 = Max)
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
    std::atomic<float> targetEff { -21.f };   // Target after music lift
    std::atomic<bool>  riding    { false };
    std::atomic<float> duckGrDb  { 0.f };     // ducker gain reduction (positive dB)
    std::atomic<float> keyDb     { -100.f };  // duck key level
};

class Engine
{
public:
    static constexpr double kCalDb = 3.0103;   // meters: a sine reads its peak level
    static constexpr double kReadTau = 0.005;  // de-zipper for automation / manual moves

    explicit Engine (const double* params = kFitted) { model.setParams (params); std::copy (params, params + P_COUNT, prm); }

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        chunk = std::max (1, (int) std::lround (fs / 1000.0));
        model.prepare ((double) chunk / fs);
        for (auto& w : vocW) w.prepare (fs, prm[P_HPF], prm[P_BELL]);
        for (auto& w : musW) w.prepare (fs, prm[P_HPF], prm[P_BELL]);
        readCoef = 1.0 - std::exp (-1.0 / (kReadTau * fs));
        rangeDecayPerChunk = 3.0 * chunk / fs;   // range display relaxes 3 dB/s
        reset();
    }

    void reset()
    {
        for (auto& w : vocW) w.reset();
        for (auto& w : musW) w.reset();
        model.reset();
        accV = accM = 0; count = 0;
        rampFrom = rampTo = 0; started = false;
        appliedDb = 0; autoDb = 0; rangeLo = rangeHi = -100; musicDb = -100;
    }

    // Process a block in place. sc may be null (no music sidechain).
    void process (float* const* io, int numCh, const float* const* sc, int scCh, int n, const Settings& s, Telemetry* t)
    {
        ModelSettings ms;
        ms.target = s.targetDb; ms.vsens = s.vocalSens; ms.msens = s.musicSens;
        ms.rmin = s.rangeMin; ms.rmax = s.rangeMax; ms.idle = s.idleDb;
        ms.fast = s.attack == AttackMode::Fast;

        const int nv = std::clamp (numCh, 0, kMaxCh);
        const int ns = (sc != nullptr) ? std::clamp (scCh, 0, kMaxCh) : 0;
        const double invV = 1.0 / std::max (1, nv), invM = 1.0 / std::max (1, ns);

        for (int i = 0; i < n; ++i)
        {
            double p = 0;
            for (int c = 0; c < nv; ++c) { const double w = vocW[c].process (io[c][i]); p += w * w; }
            accV += p * invV;
            if (ns > 0)
            {
                double q = 0;
                for (int c = 0; c < ns; ++c) { const double w = musW[c].process (sc[c][i]); q += w * w; }
                accM += q * invM;
            }

            // fader for this sample: glide from rampFrom to rampTo across the chunk
            autoDb = rampFrom + (rampTo - rampFrom) * (double) (count + 1) / (double) chunk;

            if (++count == chunk)
            {
                const double f = model.step (accV / chunk, accM / chunk, ms);
                if (! started) { rampTo = f; started = true; }
                rampFrom = rampTo; rampTo = f;
                musicDb = 10.0 * std::log10 (accM / chunk + 1e-20) + kCalDb;
                const double lv = model.getLevelDb() + kCalDb;
                if (model.isRiding())
                {
                    if (lv > rangeHi) rangeHi = lv; else rangeHi -= rangeDecayPerChunk;
                    if (lv < rangeLo || rangeLo < -99) rangeLo = lv; else rangeLo += rangeDecayPerChunk;
                    if (rangeLo > rangeHi) rangeLo = rangeHi;
                }
                accV = accM = 0; count = 0;
            }

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
            const double lift = model.getLiftDb();
            t->levelDb.store ((float) (model.getLevelDb() + kCalDb));
            t->rangeLoDb.store ((float) rangeLo);
            t->rangeHiDb.store ((float) rangeHi);
            t->riderDb.store ((float) appliedDb);
            t->autoDb.store ((float) model.getFader());
            t->musicDb.store ((float) musicDb);
            t->musicLed.store ((float) std::clamp (lift / 4.0, 0.0, 1.0));
            t->targetEff.store ((float) (s.targetDb + lift));
            t->riding.store (model.isActive());
        }
    }

    double getAutoDb() const noexcept { return autoDb; }
    double getAppliedDb() const noexcept { return appliedDb; }
    bool   isRiding() const noexcept { return model.isActive(); }
    double getLevelDb() const noexcept { return model.getLevelDb() + kCalDb; }

private:
    static constexpr int kMaxCh = 2;
    double prm[P_COUNT] {};
    RiderModel model;
    DetectorFilter vocW[kMaxCh], musW[kMaxCh];
    double fs = 48000, readCoef = 0, rangeDecayPerChunk = 0;
    int chunk = 48, count = 0;
    double accV = 0, accM = 0, rampFrom = 0, rampTo = 0, autoDb = 0, appliedDb = 0;
    double rangeLo = -100, rangeHi = -100, musicDb = -100;
    bool started = false;
};

//==============================================================================
// Ducker: sits right after the rider (unchanged from v0.2). Listens to a key (an Ecosystem
// channel) and pulls the vocal down while the key is above the threshold, by up to Depth dB.
// It follows the rider's Attack switch:
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
        gateCoef = coef (0.010);
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
        attCoef = coef (a == AttackMode::Fast ? 0.008 : 0.030);
        relCoef = coef (a == AttackMode::Fast ? 0.150 : 0.300);
    }
    double fs = 48000;
    Weighting keyW[2];
    AttackMode curAttack = AttackMode::Slow;
    double gateCoef = 0, attCoef = 0, relCoef = 0;
    double msG = 0, msR = 0, grDb = 0, keyDb = -100;
};

} // namespace rider
