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

- With **Dry Idle** on (the default), the original sound passes through untouched until you hold
  a key, then Mix decides how much of the frozen tone you hear.
- Note names in GrainLock follow Cubase: middle C is **C3**.
- Hold a chord of up to 8 notes (fewer if you turn **Voices** down). One note too many takes over
  a note that is already fading out if there is one, otherwise the oldest.

## 4. What each control does

The window has a top bar, the display with the **OUTPUT** controls beside it, and five pages of
controls: **FREEZE**, **PLAY**, **MOTION**, **KEYS** and **TONE**. Click a page name to open it.
A control that is faded does nothing with the other settings as they are (for example Refresh in
HOLD); it still works, and turns solid again when it matters.

**Knobs**: hover to see the value, drag up or down to change it, hold **Ctrl** while dragging for
fine steps, and double-click to reset.

### Top bar and OUTPUT (always in view)

| Control | What it does |
| --- | --- |
| Preset menu, ◀ ▶ | Loads a factory sound. ◀ ▶ step through them. |
| Capture: HOLD / LIVE | HOLD grabs the sound once when you press a key and freezes it. LIVE keeps re-grabbing, so the tone follows the source. |
| Mix | Balance between the original sound and the frozen tone while keys are held. |
| Gain | Overall output level. A built-in limiter keeps big chords from clipping. |
| Dry Idle | On: the original sound plays at full level whenever no note is held. |
| Auto Gain | On: a note stays about as loud whatever Grain is set to. (Without it, a source already at the note's pitch gets much louder as Grain goes up.) |

### FREEZE page: what a note grabs, and when

| Control | What it does |
| --- | --- |
| Grain | How many wavelengths of the note each loop holds. More cycles give a smoother, purer tone. |
| Smooth | How long the crossfade is where the loop wraps around. More means softer; zero means buzzy. |
| Lock (Pitch Lock) | On: the note's pitch is always exact. Off: the loop really is Grain wavelengths long, which is grittier and can sound lower than the note. |
| Offset | Grabs sound from this far back in time, so a key pressed late can still catch a hit that just happened. **Offset Sync** sets it as a note value at Cubase's tempo instead. A synced Offset reaches back 1.2 seconds at most: a longer note value is halved, more than once if needed, until it fits (at 120 bpm, 1/1 becomes 1/2). |
| Refresh | LIVE only: how often the loop grabs fresh sound. **Refresh Sync** makes it a note value. |
| On Grid | LIVE with Refresh Sync, while the song is playing: re-grabs land on the song's own beat lines instead of counting from when you pressed the key. With the song stopped it changes nothing. |
| Skip | LIVE: the chance that a re-grab is left out, so the note keeps what it had a little longer. |
| Feedback Amount | Puts the frozen sound back into what gets grabbed next, so a LIVE note keeps ringing and changing after the source has moved on. It always dies away by itself. Leave Auto Gain on with it. |
| Grab | **Before Key** grabs the sound just before you pressed. **At Key** waits and grabs the sound that starts when you press. |
| Wait | Waits this long after the key before grabbing. (A short tap still plays for as long as you held it, just later.) **Wait Sync** makes it a note value, 2 seconds at most: a longer one is halved, more than once if needed, until it fits (at 100 bpm, 1/1 becomes 1/2). |
| Snap | Looks for the nearest hit (a drum, a consonant) within this many ms of the key and starts the loop exactly on it. With no hit nearby, the note simply starts that much later. |
| Thresh (Threshold) | A key pressed while the input is quieter than this waits for sound instead of freezing silence. In LIVE, the note keeps its last good grab while the input is quiet. |
| Max Wait | The longest a key waits for sound before it grabs whatever is there. A short tap that you have already let go by then plays nothing (in Latch or Fixed, or with the sustain pedal down, it still plays). |
| Skip Hiss | LIVE: a held note never re-grabs breath or "sss" sounds. With the Threshold on as well, a key pressed on one waits for a sung sound (for as long as Max Wait allows; a hit that Snap finds is still taken as it is). |
| Gate | The frozen sound is heard only while the input is above the Threshold. |

### PLAY page: the note itself

| Control | What it does |
| --- | --- |
| Tune / Fine | Shifts every note in semitones / cents. |
| Formant | Changes the character (bigger or smaller, darker or brighter) without changing the pitch. |
| Key Follow | On: the character follows the key, so high notes are brighter and low notes darker, like a sampler. Off: every key has the same character. |
| Glide | How long the pitch slides from one note to the next (in Mono, or with Poly on). |
| Legato | Glide only when you play the new key before letting go of the old one. |
| Per Oct | Glide is the time for one octave, so big jumps take longer. |
| Poly | Glide in chords too: each new note slides in from the last key you played. |
| Mono | One note at a time; the last key you pressed wins, and releasing it returns to a key still held. |
| Voices | How many notes can sound at once (1 to 8). |
| A / D / S / R | The volume envelope: fade-in, fall to the sustain level, sustain level, fade-out. |
| Vel | How much playing harder makes a note louder. |

### MOTION page: what moves by itself

Three LFOs (wobbles), one each for **PITCH**, **FORMANT** and **GRAIN**; click a tab to see that
one. Its light glows while it is on, and a white dot rides the Fine, Formant and Grain knobs.

| Control | What it does |
| --- | --- |
| On / Rate / Depth | Switches the LFO on, sets its speed (when Sync is Free) and its strength. |
| Sync | Free, or a note value at Cubase's tempo. |
| Shape | Sine, triangle, square, S&H (a random step each cycle), saw, or random (a smooth wander). |
| Starts | **Free**: runs with the song. **Note**: starts again at every key. **Voice**: every note has its own. **Once**: one cycle per note, then it stops. |
| Fade In | The LFO comes in gradually after the note starts (delayed vibrato). |
| Phase | Where in its cycle the LFO starts. With Starts and Sync both at Free there is no start to move, so it only sets how far this LFO runs ahead of the other two. |
| Invert | Turns the shape upside down. |
| Note Envelope A / D | A second envelope per note: it rises over A and falls back over D. |
| Pitch / Formant / Grain | How far that envelope bends each of them (for example a pitch swoop into every note). |
| Tape Stop | When you let go, the note slows down to a stop over the Release time. |
| Vibrato Rate / Depth | The vibrato that the mod wheel, aftertouch or expression pedal brings in (see KEYS). |

### KEYS page: the rest of the keyboard

| Control | What it does |
| --- | --- |
| Bend Up / Down | How far the pitch wheel bends, in semitones. |
| Mod Wheel, Aftertouch, Expression | What each one moves (vibrato, Formant, Grain or level) and by how much. **Expression** is an expression pedal (MIDI controller 11): fully down is its resting place and does nothing; easing it back brings its target in. **Level** with a positive Amount turns the note down as you push the wheel or press harder (silent at 100%), and makes the pedal work like a volume pedal. With a negative Amount it is the other way round: the note is quiet until you push the wheel or press, a swell. (On Expression, a negative Amount turns the note down while the pedal is fully down, and also when no pedal is connected: silent at -100%. Easing the pedal back brings the note up.) |
| Key Up: Mode | **Normal**: a note ends when you let go. **Latch**: a chord stays until you play the next one (to stop it, switch back to Normal or stop the song). **To Grid**: letting go takes effect on the next Length line of the song (with the song stopped it works like Normal). **Fixed**: every note lasts exactly one Length. |
| Length | The note value used by To Grid and Fixed. |
| Sus Pedal | On: the sustain pedal holds notes (in Normal and To Grid). |

### TONE page: the frozen sound only (the original sound is never touched)

| Control | What it does |
| --- | --- |
| Low Cut / High Cut | Remove lows / highs. Fully left / right is off. |
| Tilt | Tips the balance towards bass or treble. |
| Drive | Warm overdrive. It is cleaned up internally so it does not add harsh whistles. |
| Hollow | Thins the sound towards a hollow, clarinet-like tone. |
| Diffuse | Smears the sound into a soft, wide cloud. |
| Spread / Spread Mode | Places notes left and right. **Alternate**: one left, the next right. **By Pitch**: low notes left, high notes right. **Random**: anywhere. |
| Width | Opens a single note out in stereo. In mono it sounds exactly as it did. The further to one side Spread or Drift has put a note, the less Width can do: a note hard left or hard right gets none (for example every note with Spread at 100% in Alternate). |
| Drift | Each note wanders slightly in pitch and position. |

**Display and keyboard**: the big display shows one loop of the most recent note, with the
crossfade (seam) shaded on the right and the notes you are holding as chips. While a key is
waiting to grab (Wait, At Key, Snap, Threshold) and nothing else is sounding, the display says what
it is waiting for. The strip at the bottom lights up the keys you are holding; keys that are still
waiting are dimmer.

## 5. Factory presets

| Preset | Sound |
| --- | --- |
| Init | The plain starting sound: every control back at its default. Choose it to start again from scratch. |
| Robot Voice | Live mode, 1 cycle, medium smoothing. Talk or sing into it for a robot voice. |
| Stutter Gate | Hold mode, instant attack, short release. Tight, gated stutters on each note. |
| Drone Pad | Live mode, 8 cycles, slow fade in and out, a gentle pitch drift plus a slower formant swell. |
| Glitch Drums | Hold mode, grabs from well before the key, random steps on Grain (1/16) and pitch (1/8). Made for drum loops. |
| Formant Choir | Live mode, 4 cycles, formant lowered by 5 semitones, vibrato plus a slow vowel drift. Play 4-note chords. |
| Beat Catcher | For drum loops: At Key with Snap, so each note starts its loop on the nearest hit. |
| Grid Slicer | While the song plays: re-grabs on every eighth note of the song, skips a quarter of them, and ends each note on the next sixteenth. |
| Feedback Bloom | Feedback and Diffuse: a chord keeps blooming after the source has moved on. |
| Tape Choir | A choir whose tone follows the key, with a late vibrato on every voice and a tape-stop release. |
| Latch Drone | Play a chord and let go: it stays, wide and slowly wandering, until the next chord. |
| Breath Guard | For a voice: a key waits for a sung sound instead of freezing silence, a breath or an "sss" (for up to 0.3 seconds), and a held note stays on the last sung sound through them. |

## 6. Opening a project made with an older version

A project saved with version 0.2 opens with its own settings. Three of the new things are switched
off for it, because 0.2 did not have them: **Auto Gain**, the **sustain pedal**, and the **mod
wheel** (which now adds vibrato by default). Switch them on if you want them.

Three things are deliberately different from 0.2:

- **The loop seam is a little cleaner.** Where the seam fade is very short (Smooth near zero, or
  notes from about an octave above middle C upward at the default Smooth), a note may now freeze a
  moment up to 10 ms earlier, so the loop joins without a tick. On a steady pitched sound such a
  note can come out a little louder or quieter than it did (in the tests about 2 dB over a whole
  note, and about 6 dB for the first 20 ms of one; mainly with Hold presets such as Stutter Gate and
  Glitch Drums).
- **Which note gives way.** When more than 8 notes overlap, the note that gives way is now one that
  is already fading out if there is one, not simply the oldest, so a long held note is no longer cut
  off by short notes played over it.
- **LFO Shape automation.** If you drew automation for an LFO's **Shape**, draw it again: the list
  grew from four shapes to six, so old automation points now pick different shapes (Triangle plays
  as Square, Square as S&H, S&H as Random). The Shape saved in the project itself is not affected.

Picking a preset from the menu or with the arrows, even the one the project already shows, sets
everything as for new work, so Auto Gain, the sustain pedal and the mod wheel come back on. Stutter
Gate and Drone Pad can then sound quieter than they did in 0.2; switch Auto Gain off to get the old
level.

## 7. Something wrong? Tell me

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
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGRAINLOCK_BUILD_TESTS=ON
cmake --build build --config Release
build/GrainLockTests_artefacts/Release/GrainLockTests
```

The test runner checks pitch accuracy, staccato safety, dry pass-through, state round-trips,
allocation-free processing, every 0.3 feature, and two reference sounds (the 0.2 sound and the 0.3
default sound) that must not drift. CI (`.github/workflows/build.yml`) builds on Windows and Linux,
runs the tests, validates the VST3 with pluginval at strictness 10, renders pictures of every page
of the interface, and attaches a zip to a GitHub release for every `v*` tag.
