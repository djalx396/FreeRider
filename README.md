# Free Rider v0.1 — automatic vocal gain rider

A clone of Waves Vocal Rider's behaviour: JUCE, VST3 + AU, native on Apple Silicon
(universal binary), **zero latency**, mono or stereo in one plugin, music sidechain.

## Build (GitHub, same as the other plugins)

1. Create a new GitHub repo (e.g. `free-rider`) and push this folder to it.
2. The **Build Free Rider** workflow runs on every push (Actions tab). It fetches JUCE
   8.0.4 itself and builds a universal VST3 + AU.
3. Download the **FreeRider-macOS** artifact from the finished run and unzip it.
4. Copy `Free Rider.vst3` → `~/Library/Audio/Plug-Ins/VST3/` and `Free Rider.component` →
   `~/Library/Audio/Plug-Ins/Components/`.
5. Clear the download quarantine, then rescan in Reaper:
   ```bash
   xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/VST3/Free Rider.vst3" "$HOME/Library/Audio/Plug-Ins/Components/Free Rider.component"
   ```

Local build instead: `cmake -B build -G Xcode && cmake --build build --config Release`.

## Using it in Reaper

- Put it last on the vocal (or vocal bus).
- **Music sidechain**: send the instrumental bus (post-fader) to the vocal
  track's channels **3/4**. The vocal track needs 4 channels. The header shows
  "music sidechain: track ch 3/4", and the Music LED lights when it's getting
  signal.
- **Writing automation**: arm the *Rider Fader* parameter lane, put the track in
  Write/Touch/Latch, set Rider's Automation to **Write** and play. Afterwards
  edit the lane and switch to **Read**. Unlike the Waves plugin, the editor does
  *not* need to be open while writing.

## Controls (ranges match Vocal Rider's manual)

| Control | Range / default | What it does |
|---|---|---|
| Target | 0…−42 dBFS / −21 | Level the vocal is held at; drag the amber "screen" on the meter (Shift = fine, double-click = −21) |
| Vocal Sens | ±12 dB / 0 | Gate threshold = Target − 24 − Vocal Sens. Turn it down until RIDE follows words, not spill |
| Attack | Slow / Fast | Detector and fader speed |
| Music Sens | ±12 dB / 0 | How much a louder music bed lifts the target (dashed line on the meter) |
| Range Min / Max | −12…+11 / −6, −11…+12 / +6 | Fader limits: the amber handles left of the fader |
| Idle | ±12 / 0 | Fader position between phrases: the arrow right of the fader |
| Rider Fader | ±12 dB | The ride itself. Grab it to override; riding resumes on release |
| Automation | Off / Write / Read | As in Vocal Rider |
| Output | ±12 dB / 0 | Trim after riding, with a clip LED (click to reset) |

## How the engine works (`Source/RiderEngine.h`)

- Detector: 100 Hz high-pass plus a +4 dB shelf at 1.5 kHz, power-averaged over
  channels. Calibrated so a sine reads its peak level.
- Ride level: a quick-to-rise, slow-to-fall follower (Slow 30/300 ms, Fast
  8/150 ms). The fader glides at 150 ms (Slow) or 50 ms (Fast). During the first
  60 ms (Slow) or 20 ms (Fast) of a word it may only move down, so loud starts
  get caught and quiet starts aren't pumped up.
- Word endings: a word ends when the level drops 20 dB below the word's peak or
  under the threshold. The fader then **freezes** for the hold time, so breaths
  between syllables aren't ridden up. After that it glides to Idle.
- Music: 300 ms power average of the sidechain, smoothed over 1 s.
  Target lift = ½ × (music + Music Sens − (Target − 6)), between 0 and 12 dB.

**What's measured and what's guessed.** The control set and ranges come from
Waves' manual. The time constants, the gate rule and the music law are first
guesses. The capture kit gives the real Vocal Rider's gain curve directly, and
those numbers get fitted to it.

## Tests / tools

- `tools/gen_signals.py`: generates the capture kit (synthetic vocal + music).
- `tools/extract_gain.py`: render ÷ input → fader curve (any level-only plugin).
- `tests/run_engine.cpp`: runs the engine offline over the kit and writes a fader trace.
