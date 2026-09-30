# GrainLock

GrainLock is a VST3 effect for Cubase on Windows. Put it on an audio track, play notes into it
from a MIDI track, and every note freezes a tiny slice of whatever the track is playing into a
tone at that note's pitch. A voice becomes a robot, a drum loop becomes a pitched stutter, a pad
becomes a drone you can play like a synth.

---

## 1. Download the latest build

**Easiest: the Releases page**

1. Open <https://github.com/kalanhattonre-bit/GrainLock/releases>.
2. Under the newest release, click the file named `GrainLock-v…-Windows-x64.zip` to download it.
   (If the page shows no releases yet, use the Actions tab below.)

**Newest test build: the Actions tab** (you need to be signed in to GitHub)

1. Open <https://github.com/kalanhattonre-bit/GrainLock/actions>.
2. Click the top run that has a green tick.
3. Scroll to **Artifacts** and click **GrainLock-VST3-Windows** to download it.

## 2. Install it

1. Right-click the downloaded zip and choose **Extract All…**.
2. Inside you will find a folder called **`GrainLock.vst3`**. Copy that whole folder.
3. Paste it into **`C:\Program Files\Common Files\VST3`**. Windows will ask for permission; click **Continue**.
4. Open Cubase and go to **Studio → VST Plug-in Manager**, then click **Rescan All** (or just restart Cubase).
   GrainLock appears under the vendor name **Kalan Hatton**.

## 3. Set it up in Cubase

1. **Insert GrainLock on an audio track** (the vocal, drum loop or whatever you want to freeze).
2. **Create a MIDI track.**
3. In the MIDI track's Inspector, set its **output** to **GrainLock** (it is listed with the name of
   the audio track it sits on).
4. **Record-enable or monitor the MIDI track** and play notes. Play the audio track at the same time:
   GrainLock can only freeze sound that is actually coming in.

Tips:

- With **Dry When Idle** on (the default), the original sound passes through untouched until you hold
  a key, then Mix decides how much of the frozen tone you hear.
- Note names in GrainLock follow Cubase: middle C is **C3**.
- Hold a chord for up to 8 notes at once. A 9th note takes over the oldest one.

## 4. What each control does

**Top bar**

| Control | What it does |
| --- | --- |
| Preset menu, ◀ ▶ | Loads a factory sound. ◀ ▶ step through them. |
| Capture: HOLD / LIVE | HOLD grabs the sound once when you press a key and freezes it. LIVE keeps re-grabbing, so the tone follows the source. |

**FREEZE**

| Control | What it does |
| --- | --- |
| Grain | How many wavelengths of the note each loop holds. More cycles give a smoother, purer tone. |
| Smooth | How long the crossfade is where the loop wraps around. More means softer; zero means buzzy and clicky. |
| Offset | Grabs sound from this far back in time, so a key pressed late can still catch a hit that just happened. |
| Refresh | LIVE only: how often the loop grabs fresh sound. Short keeps words understandable; long gives slow, evolving drones. |
| Lock (Pitch Lock) | On: the note's pitch is always exact. Off: the loop really is Grain wavelengths long, which is grittier and can sound lower than the note. |

**VOICE**

| Control | What it does |
| --- | --- |
| Tune | Shifts every note up or down in semitones. |
| Fine | Fine-tunes every note in cents. |
| Formant | Changes the character (bigger or smaller, darker or brighter) without changing the pitch. |
| Glide | Mono only: how long the pitch slides from one note to the next. |
| Mono | One note at a time; the last key you pressed wins, and releasing it returns to a key still held. |

**OUTPUT**

| Control | What it does |
| --- | --- |
| Mix | Balance between the original sound and the frozen tone while keys are held. |
| Gain | Overall output level. A built-in limiter keeps big chords from clipping. |
| Dry Idle (Dry When Idle) | On: the original sound plays at full level whenever no key is held. |

**ENVELOPE**

| Control | What it does |
| --- | --- |
| A | Attack: how fast each note fades in. Zero gives hard stutters. |
| D | Decay: how fast it falls to the sustain level. |
| S | Sustain: the level while the key stays down. |
| R | Release: how long the note rings after you let go. |
| Vel | How much playing harder makes a note louder. At zero, every note is full volume. |

**LFO**: three separate wobbles, one each for PITCH, FORMANT and GRAIN. Any or all of them can run
at the same time, each with its own speed, sync, shape and depth. Click a tab to see that LFO's
controls; its light glows while it is on. A white dot rides on the Fine, Formant and Grain knobs to
show each LFO moving.

| Control | What it does |
| --- | --- |
| On | Switches this LFO on or off. |
| Rate | Wobble speed in Hz (used when Sync is Free). |
| Depth | How strong the wobble is. At 100%: PITCH swings ±100 cents, FORMANT ±12 semitones, GRAIN ±8 cycles. |
| Sync | Free, or locked to Cubase's tempo, from 1/1 down to 1/32 (T = triplet). |
| Shape | Sine, triangle, square, or S&H (a new random step every cycle). |

**Display and keyboard**: the big display shows one loop of the most recent note, with the
crossfade (seam) shaded on the right and the notes you are holding as chips. The strip at the
bottom lights up the keys you are holding.

**Knobs**: hover to see the value, drag up or down to change it, hold **Ctrl** while dragging for
fine steps, and double-click to reset.

## 5. Factory presets

| Preset | Sound |
| --- | --- |
| Robot Voice | Live mode, 1 cycle, medium smoothing. Talk or sing into it for a robot voice. |
| Stutter Gate | Hold mode, instant attack, short release. Tight, gated stutters on each note. |
| Drone Pad | Live mode, 8 cycles, slow fade in and out, a gentle pitch drift plus a slower formant swell. |
| Glitch Drums | Hold mode, grabs from well before the key, random steps on Grain (1/16) and pitch (1/8). Made for drum loops. |
| Formant Choir | Live mode, 4 cycles, formant lowered by 5 semitones, vibrato plus a slow vowel drift. Play 4-note chords. |

## 6. Something wrong? Tell me

Open an issue at <https://github.com/kalanhattonre-bit/GrainLock/issues/new> and include:

- your **Cubase version** (Help → About Cubase),
- your **sample rate** and **buffer size** (Studio → Studio Setup → Audio System),
- which **GrainLock version** you installed (the release name),
- **what you heard**, and what you expected to hear. Say which preset or settings you used, and what
  kind of audio was on the track.

---

## Licences and credits

- GrainLock is built with [JUCE](https://juce.com). JUCE has its own licence terms (a free
  open-source AGPLv3 option and commercial licences). **Check JUCE's licence before you share or
  sell builds of this plugin.**
- VST is a registered trademark of Steinberg Media Technologies GmbH.

## For developers

Builds need CMake 3.22+ and a C++20 compiler; JUCE 8.0.15 is fetched automatically.

```bash
cmake -S . -B build -DGRAINLOCK_BUILD_TESTS=ON
cmake --build build --config Release
build/GrainLockTests_artefacts/Release/GrainLockTests
```

The test runner checks pitch accuracy, staccato safety, dry pass-through, state round-trips and
allocation-free processing. CI (`.github/workflows/build.yml`) builds on Windows and Linux, runs the
tests, validates the VST3 with pluginval at strictness 10, and attaches a zip to a GitHub release
for every `v*` tag.
