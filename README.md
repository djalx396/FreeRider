# Free Rider v0.3 — automatic vocal gain rider (The Ecosystem)

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

## Ecosystem: ducker + send (new in v0.2)

Free Rider is part of The Ecosystem and sits on the **Ecosystem Bus** (channels 01-24, 0 = off),
the same background channels Console and FreeSpacer use. No routing needed.

- **Duck From** (Off / 01-24): when a channel is picked, a ducker runs **right after the rider**.
  Both work together: the rider keeps riding exactly as before, and the ducker pulls the vocal
  down while whatever is sending on that channel (kick, snare, ...) is above the Threshold.
  - **Speed follows the rider's Attack switch**: Slow ducks in ~30 ms and lets go over ~300 ms,
    Fast ducks in ~8 ms and lets go over ~150 ms (the rider's own detector times).
  - **Depth** 0-24 dB (default 6): the most it will duck.
  - **Threshold** −60…0 dBFS (default −30): 1 dB of duck per dB the key is above it, up to Depth.
  - The ducker isn't written into Rider Fader automation; it stays live in Read mode too.
- **Send To** (Off / 01-24): publishes the sound Free Rider made (after rider, ducker and Output)
  on a channel, so a vocal can be the key for FreeSpacer or Console ducks.
- **NAME**: renames the channel for every Ecosystem plug-in in the project; the names are saved
  with the project.
- **Align** (while ducking): Short / Normal (default) / Safe / Max = 1024 / 4096 / 8192 / 16384
  samples at 44.1/48 kHz (doubled at 88.2/96, ×4 at 176.4/192), the same buffer as Console's.
  - **Duck Off = zero latency** (Free Rider as in v0.1, bit-identical).
  - **Duck on** = Free Rider delays itself by Align and reports it, so Reaper's delay
    compensation keeps everything in time. The key is read at the exact timeline sample it
    belongs to, so ducking is sample-accurate whatever order Reaper processes the tracks in.
  - If the status line warns that the key arrives too late, raise Align (Max covers Reaper's
    200 ms anticipative render-ahead), or disable anticipative FX on those tracks.
- The status line under the Duck meter shows how many senders were found, the current duck,
  the latency, and the send channel.
- Ducking pauses while the transport is stopped (the timeline is the clock), like Console.
- All Ecosystem plug-ins must run in the same Reaper process (normal for native plug-ins).

## Controls (ranges match Vocal Rider's manual)

| Control | Range / default | What it does |
|---|---|---|
| Target | 0…−42 dBFS / −21 | Level the vocal is held at; drag the amber "screen" on the meter (Shift = fine, double-click = −21) |
| Vocal Sens | ±12 dB / 0 | How much of each phrase counts as vocal. Lower = rides only the strongest parts and ignores spill; higher = rides more of the phrase |
| Attack | Slow / Fast | How fast loud starts are pulled down (~125 ms vs ~47 ms) |
| Music Sens | ±12 dB / 0 | How much a louder music bed lifts the target (dashed line on the meter) |
| Range Min / Max | −12…+11 / −6, −11…+12 / +6 | Fader limits (the fader eases into them): the amber handles left of the fader |
| Idle | ±12 / 0 | Where the fader rests between phrases, as a position in the range (−12 = Min, 0 = middle, +12 = Max): the arrow right of the fader |
| Rider Fader | ±12 dB | The ride itself. Grab it to override; riding resumes on release |
| Automation | Off / Write / Read | As in Vocal Rider |
| Output | ±12 dB / 0 | Trim after riding, with a clip LED (click to reset) |

## How close it is to Waves Vocal Rider (v0.3)

v0.3's riding law is fitted to captures of the real Vocal Rider: 15 settings × a 3 min 53 s test
vocal, compared millisecond by millisecond (`Free_Rider_Capture.lua`).

| Setting | typical difference | 95 % of the time within |
|---|---|---|
| Defaults (and Music Sens −12…+12, no sidechain) | 0.4 dB | 1.8 dB |
| Attack Fast | 0.5 dB | 2.6 dB |
| Range ±12 / −12…+3 / ±3 | 0.8 / 0.7 / 0.4 dB | 2.6 / 2.4 / 1.8 dB |
| Vocal Sens −12 / +12 | 0.5 / 0.5 dB | 2.7 / 1.8 dB |
| Target −30 / −12 | 0.4 / 0.1 dB | 2.5 / 1.4 dB |
| Idle +6 | 0.4 dB | 2.0 dB |

(v0.2 was 1–1.5 dB typical, 5–8 dB at the 95 % mark.)

## How the engine works (`Source/RiderModel.h`, `Source/RiderEngine.h`)

Measured from Vocal Rider and built into the model:
- **Partial levelling**: the fader moves about 0.86 dB per dB of level difference, then eases softly
  into Range Min / Range Max. Each side follows its own limit.
- **Peak-based riding**: the fader only moves while the vocal is near its recent peak. Vocal Sens
  widens or narrows that window, so at −12 quiet phrases are ridden only a little.
- **Speed**: it rises at most about 5 dB/s; it falls with a ~125 ms (Slow) / ~47 ms (Fast) time
  constant. Between phrases it holds, then drifts to Idle.
- **Idle is a position inside the range**, as in Vocal Rider: −12 = Range Min, 0 = the middle,
  +12 = Range Max. With Range −12/+3 and Idle 0 the fader rests at −4.5.
- **Digital silence**: the detectors pause and the fader drifts slowly toward Idle.
- **Music**: lift = a soft-knee curve of (music level + Music Sens − Target), the same curve for
  every setting. Music Sens +6 acts like 6 dB louder music.
- It runs a control step every ~1 ms (whatever the sample rate), and within each step the gain
  glides smoothly. Zero latency; 44.1–192 kHz and any buffer size give the same ride (48 vs 96 kHz
  within 0.06 dB).

## Code layout
- `Source/RiderModel.h`: the fitted riding law (shared with the fitting tools).
- `Source/RiderEngine.h`: detector filters, the 1 ms control loop, gain, and the ducker (no JUCE).
- `Ecosystem/EcoBus.h`: the shared Ecosystem Bus, layout v1. Copy this folder unchanged into
  every Ecosystem plug-in (Console, FreeSpacer, ...) so they all share one bus.

## Tests / tools

- `tools/gen_signals.py`: generates the capture kit (synthetic vocal + music).
- `tools/extract_gain.py`: render ÷ input → fader curve (any level-only plugin).
- `tests/run_engine.cpp`: runs the engine offline over the kit and writes a fader trace.
