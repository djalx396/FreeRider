// RiderModel.h — Free Rider v0.3 riding law, fitted to Waves Vocal Rider captures.
//
// The model runs at a fixed control rate of about 1 kHz: the plug-in measures the vocal and
// music power over each ~1 ms chunk (after the detector filters) and calls step() once per
// chunk. Everything is in milliseconds/dB, so behaviour is the same at every sample rate.
// The same code is used by the fitting tools, so what was fitted is exactly what ships.
//
// Structure (from the captures, see claude/free-rider-clone-status.md):
//   detector   fast power follower (attack by Attack mode, shared release) and a slow
//              box average; level = max(fast, slow + offset)
//   law        e = k·(Target + musicLift − level) + c, then a soft limit into
//              [Range Min, Range Max]: each side follows its own limit and eases into it
//   gate       vocal when level > Target + g0 − g1·VocalSens AND level > recent peak − (gRel + g2·VocalSens)
//   fader      falls with a time constant (Attack), rises rate-limited (dB/s) with an
//              optional rounding time constant; between phrases it holds, then glides to Idle
//   idle       Idle is a position inside the range (-12 = Range Min, 0 = middle, +12 = Range Max)
//   music      lift = scale·w·ln(1 + exp((music + MusicSens − Target − 21 − x0) / w))
//   silence    in digital silence the detectors pause; the fader drifts toward Idle slowly
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace rider {

enum P : int
{
    P_HPF = 0,       // detector high-pass (Hz)
    P_BELL,          // detector bell gain at 1.5 kHz (dB)
    P_TATT_S,        // fast-follower attack, Slow (ms)
    P_TATT_F,        // fast-follower attack, Fast (ms)
    P_TREL,          // fast-follower release (ms)
    P_WBOX,          // slow box window (ms)
    P_CSLOW,         // slow level offset (dB) before max()
    P_K,             // levelling slope
    P_C,             // law offset (dB)
    P_SOFT,          // soft-limit exponent
    P_G0,            // gate threshold relative to Target (dB)
    P_G1,            // gate shift per dB of Vocal Sens
    P_GREL,          // gate: dB below recent peak that still counts as vocal
    P_PKDEC,         // recent-peak decay (dB/s)
    P_HOLD_S,        // hold between phrases, Slow (ms)
    P_HOLD_F,        // hold between phrases, Fast (ms)
    P_UPRATE,        // max rise speed (dB/s)
    P_TDN_S,         // fall time constant, Slow (ms)
    P_TDN_F,         // fall time constant, Fast (ms)
    P_IDLERATE,      // glide to Idle (dB/s)
    P_TUP,           // rise rounding time constant (ms)
    P_TMUS,          // music power time constant (ms)
    P_MX0,           // music knee position (dB)
    P_MW,            // music knee width (dB)
    P_MSCALE,        // music lift scale
    P_SILRATE,       // glide toward Idle during digital silence (dB/s); detectors pause in silence
    P_G2,            // detection window (dB below recent peak) added per dB of Vocal Sens
    P_COUNT
};

struct ModelSettings
{
    double target = -21, vsens = 0, msens = 0, rmin = -6, rmax = 6, idle = 0;
    bool fast = false;
    bool musicOff = false;   // Music Sensitivity at "Off"
};

// Fitted values (2026-11-10, mean |error| 0.66 dB, 15 Vocal Rider settings). Order as enum P.
static const double kFitted[P_COUNT] = {
    100, 0, 10.5394, 3.4372, 130.642,
    74.6307, -36.6418, 0.862737, 0.609085, 6.39445,
    -33.4119, 0.000122555, 4.88168, 6.42927, 1496.64,
    1793.78, 5.19843, 125.192, 47.1212, 30.3779,
    4.49166, 1733.56, -14.7739, 5.13361, 1.22752,
    3.95153, 0.804852 };

class RiderModel
{
public:
    void setParams (const double* p) { std::copy (p, p + P_COUNT, prm); derive(); }
    void prepare (double dtSeconds) { dt = dtSeconds; derive(); reset(); }

    void reset()
    {
        ef = 0; mus = 0; box.assign ((size_t) std::max (1, boxN), 0.0); boxSum = 0; boxIdx = 0;
        peak = -200; holdLeft = 0; fader = 0; levelDb = -200; liftDb = 0; active = false; holding = false;
        first = true;
    }

    // One control step. pv / pm = mean power of the weighted vocal / music over the chunk.
    // Returns the internal fader position (dB) for this chunk.
    double step (double pv, double pm, const ModelSettings& s)
    {
        const double rmin = std::min (s.rmin, s.rmax - 1.0), rmax = std::max (s.rmax, rmin + 1.0);
        // Vocal Rider's Idle is a position inside the range: -12 = Range Min, 0 = middle,
        // +12 = Range Max (measured: Range -12/+3 idles at -4.5; Range ±6 with Idle +6 at +3).
        const double idle = rmin + (std::clamp (s.idle, -12.0, 12.0) + 12.0) / 24.0 * (rmax - rmin);
        if (first) { fader = idle; first = false; }

        // digital silence: the detectors pause and the fader drifts toward Idle
        if (pv < 1e-12)
        {
            holdLeft = 0; active = false; holding = false;
            fader += std::clamp (idle - fader, -silPerStep, silPerStep);
            fader = std::clamp (fader, rmin, rmax);
            mus += aMus * (pm - mus);
            return fader;
        }

        // detector
        const double aA = s.fast ? aAttF : aAttS;
        ef += (pv > ef ? aA : aRel) * (pv - ef);
        boxSum += pv - box[(size_t) boxIdx]; box[(size_t) boxIdx] = pv; boxIdx = (boxIdx + 1) % boxN;
        const double lf = 10.0 * std::log10 (ef + 1e-20);
        const double ls = 10.0 * std::log10 (std::max (boxSum, 0.0) / boxN + 1e-20) + prm[P_CSLOW];
        levelDb = std::max (lf, ls);

        // music
        mus += aMus * (pm - mus);
        const double mDb = 10.0 * std::log10 (mus + 1e-20);
        liftDb = 0;
        if (! s.musicOff && mDb > -100)
        {
            const double w = std::max (0.1, prm[P_MW]);
            const double x = (mDb + s.msens - (s.target + 21.0) - prm[P_MX0]) / w;
            liftDb = prm[P_MSCALE] * w * (x > 30 ? x : std::log1p (std::exp (x)));
        }

        // gate (vocal activity)
        peak = std::max (lf, peak - pkDecPerStep);
        const double thr = s.target + prm[P_G0] - prm[P_G1] * s.vsens;
        const double window = std::max (0.5, prm[P_GREL] + prm[P_G2] * s.vsens);
        const bool vocal = lf > thr && lf > peak - window;
        const int holdSteps = (int) std::lround ((s.fast ? prm[P_HOLD_F] : prm[P_HOLD_S]) * 1e-3 / dt);
        if (vocal) { holdLeft = holdSteps; active = true; holding = false; }
        else if (holdLeft > 0) { --holdLeft; holding = true; }
        else { active = false; holding = false; }

        // fader
        if (vocal)
        {
            const double want = law (prm[P_K] * (s.target + liftDb - levelDb) + prm[P_C], rmin, rmax);
            if (want < fader) fader += (s.fast ? aDnF : aDnS) * (want - fader);
            else
            {
                double up = want - fader;
                if (aUp < 1.0) up *= aUp;
                fader += std::min (up, upPerStep);
            }
        }
        else if (! holding)
        {
            const double d = idle - fader;
            fader += std::clamp (d, -idlePerStep, idlePerStep);
        }
        fader = std::clamp (fader, rmin, rmax);
        return fader;
    }

    // soft limit: each side eases into its own limit
    double law (double e, double rmin, double rmax) const
    {
        const double R = e >= 0 ? rmax : -rmin;
        if (R <= 1e-6) return e >= 0 ? std::min (e, rmax) : std::max (e, rmin);
        const double u = std::abs (e) / R, p = std::max (0.5, prm[P_SOFT]);
        const double y = R * u / std::pow (1.0 + std::pow (u, p), 1.0 / p);
        return e >= 0 ? std::min (y, rmax) : std::max (-y, rmin);
    }

    double getLevelDb() const { return levelDb; }
    double getLiftDb() const { return liftDb; }
    bool   isRiding() const { return active && ! holding; }
    bool   isActive() const { return active; }
    double getFader() const { return fader; }
    const double* params() const { return prm; }

private:
    static double coef (double ms, double dt) { return ms <= 0 ? 1.0 : 1.0 - std::exp (-dt / (ms * 1e-3)); }
    void derive()
    {
        aAttS = coef (prm[P_TATT_S], dt); aAttF = coef (prm[P_TATT_F], dt); aRel = coef (prm[P_TREL], dt);
        aDnS = coef (prm[P_TDN_S], dt);  aDnF = coef (prm[P_TDN_F], dt);  aUp = coef (prm[P_TUP], dt);
        aMus = coef (prm[P_TMUS], dt);
        upPerStep = prm[P_UPRATE] * dt; idlePerStep = prm[P_IDLERATE] * dt; pkDecPerStep = prm[P_PKDEC] * dt;
        silPerStep = prm[P_SILRATE] * dt;
        const int n = std::max (1, (int) std::lround (prm[P_WBOX] * 1e-3 / dt));
        if (n != boxN) { boxN = n; box.assign ((size_t) boxN, 0.0); boxSum = 0; boxIdx = 0; }
    }

    double prm[P_COUNT] { };
    double dt = 0.001;
    double aAttS = 1, aAttF = 1, aRel = 1, aDnS = 1, aDnF = 1, aUp = 1, aMus = 1;
    double upPerStep = 0, idlePerStep = 0, pkDecPerStep = 0, silPerStep = 0;
    int boxN = 1, boxIdx = 0;
    std::vector<double> box = std::vector<double> (1, 0.0);
    double boxSum = 0, ef = 0, mus = 0, peak = -200, fader = 0, levelDb = -200, liftDb = 0;
    int holdLeft = 0; bool active = false, holding = false, first = true;
};

} // namespace rider
