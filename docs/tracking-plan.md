# Guitar tracking: what was wrong, and the fix for every plugin

Written 11 November 2026 from two Data Corrupter demo recordings and a cross-plugin test. Bitrot is fixed in version 0.2.0. Build the Changeling and Divebomb steps in their own threads.

## What the original pedal does

Measured on about 15 minutes of Data Corrupter recordings (two YouTube demos), tracking the strongest partial frame by frame:

| Measure | Data Corrupter |
| --- | --- |
| Time on a steady pitch while sounding | 55 to 77% |
| Wobble on a held note | 3 to 7 cents |
| Level wobble on a held note | about 0.2 dB: a flat square fuzz |
| A note change | 27 to 58 ms typical, 100 to 260 ms for the slow ones |
| Changes that glide rather than jump | about 1 in 10 |
| Glides and dives | straight lines on a keyboard: a steady number of octaves per second, about 2 oct/s when slow |

So the pedal is far tighter than its reputation suggests: single notes land quickly and stay put. The chaos comes from chords, sloppy playing and Glide or Vibrato, not from the tracking itself.

## Why Bitrot sounded unruly

On a realistic guitar phrase at default settings, the first build's oscillator was on the right note **1 to 5% of the time**. Three faults, all fixed:

1. **Glide crept, and was always on.** It was an exponential slide that never quite arrived, at about a quarter of a second, so every note slurred into the next. Glide is now a straight-line slide in octaves per second, as the recordings show. Rate runs from 0.8 to 200 oct/s, and the default lands a note change in under 20 ms.
2. **Notes held an octave low for their whole length.** The tracker sometimes picked double the period at the start of a note. Once a note held, it stayed with the lag nearest that note, and a periodic signal fits double its period just as well, so it never corrected itself. This happened 13% of the time on guitar, where the pickup makes the second harmonic louder than the note. The fix is an up-octave check: if half the period fits nearly as well, the higher octave is the note. It uses one ratio in both directions so it can't fight the existing octave guard.
3. **Quiet inputs went unheard.** The tracker and gate used fixed levels. With an interface peaking around −30 dBFS, notes were chopped or not heard at all. Both now follow your playing level.

After the fix, on the same phrase: on the right note 92 to 95% of the time, an octave out 0.1%, identical at 0, −20 and −30 dB. The default mix measures 87% steady, with note changes taking 43 ms median, 5 cents of wobble and 7% of changes gliding. That's the original's fingerprint. Bitrot's `Tests/RenderTest.cpp` now has a "Real playing" suite that fails on the old engine and passes on the new one.

## The other plugins

Tested on the same guitar and bass phrases, tracker on its own:

| Plugin | Tracker | Normal phrase: right / octave out | First right reading | Fast run (70 to 150 ms notes) | Quiet input (−30 dB) |
| --- | --- | --- | --- | --- | --- |
| **Bitrot 0.2.0** | shared v2 | 98.7% / 0% | 28 ms | 92% / 1.8% | unchanged |
| **Changeling** | v1 (same as Bitrot 0.1) | 87% / 11% guitar, 93% / 5% bass | 25 ms | 78% / 14% | **hears nothing 95% of the time** |
| **Divebomb** | its own YIN | 98% / 0.7% | **58 ms** (79 ms on Clean) | **69% / 7.5%** guitar, 77% / 12% bass | unchanged |
| Oil Slick | none: a fixed-window shifter, warble by design | | | | |
| Speckle, Jetstream, Afterglow, Murmur, Prism Patch | onset detection only, no pitch | | | | |

What that means for each:

- **Changeling** has both of Bitrot's tracker faults. Every few notes sound an octave low, and a quiet input barely plays at all.
- **Divebomb's** pitch shift is always exact, and the tracker only sizes the grains. But its tracker analyses a 71 ms window and medians three readings, so the grains are the wrong size for the first 60 to 80 ms of every note. That's heard as warble or doubling on each attack, and on fast runs it never catches up. Its "ORIGINAL" glitch setting (the default) gets its warble from the tracker's real mistakes, so the warble gets worse exactly when you play fast.

## The fix: one shared tracker

`Source/engine/Tracker.h` in Bitrot is now the **Flip Fella shared pitch tracker, version 2**: one file, copied unchanged into every plugin that tracks pitch, with only the namespace line changed. It holds everything: the low-register McLeod tracker, the octave guard both ways, and the level following. Bitrot's "Real playing" suite is its reference test. A fix to tracking is made once in Bitrot, then copied out.

### Changeling (do first: drop-in)

1. Move `PolyTracker` out of `Source/engine/Tracker.h` into its own `PolyTracker.h`; it's unaffected.
2. Copy Bitrot's `Source/engine/Tracker.h` over Changeling's, changing `namespace bit` to `namespace chg`. The API is identical (`Settings`, `configure`, `push`, `currentNote`, `lastEstimate`, `lockMs`, `wildBoost`).
3. Remove any input level scaling in front of `tracker.push`; the tracker levels its own input now.
4. Copy Bitrot's "Real playing" suite into `Tests/RenderTest.cpp`, checking the synth voice pitch against the phrase. It must pass at 0, −20 and −30 dB, with Mono and Poly. Poly still uses `PolyTracker` for chords; the phrase is single notes, which the mono tracker leads.
5. Listen: the bass line that used to drop an octave every few notes, and playing with the interface gain turned well down.

### Divebomb (second: swap the tracker that sizes the grains)

1. Add the shared tracker as `Source/engine/PitchTracker.h` (namespace `db`), next to the old `Tracker.h`.
2. In `DivebombEngine`, size Classic's grains from the shared tracker: period = sample rate ÷ `mtof (currentNote())`. Until the first `NoteOn` of a note, use the minimum grain, not the last note's: a wrong-sized grain on the attack is the warble.
3. Keep **Glitch** as a deliberate, repeatable character instead of tracker mistakes. CLEAN (0) uses the tracker's period as it is. ORIGINAL (1) adds the old warble on purpose: a small random period error that's re-drawn per grain, plus an occasional octave grain on note decays. 2 does more of both. That way the warble no longer gets worse when you play fast.
4. Delete the old YIN `Tracker.h` once the tests pass.
5. Add the "Real playing" phrases to `Tests/RenderTest.cpp`. Measure the period error at CLEAN (must be under 30 cents 90% of the time after 60 ms, octave errors under 1%) and the time to the first right grain size (under 40 ms on guitar). Update `test-results-*.md`.
6. Listen: fast sixteenth runs through a fixed +12 and a −12 drop, compared with the current build. Attacks should no longer smear or double.

### Afterwards

- **Oil Slick** doesn't track pitch, and its warble is the pedal's own, so leave it.
- Any future plugin that tracks pitch starts from the shared tracker and Bitrot's "Real playing" suite.

## Still to decide for Bitrot

The recordings also show long downward dives after some notes end, about 2 oct/s for half a second or more, while the sound keeps going. That's the loop draining when the string stops. Bitrot only does this with Corrupt at Wild. It could become its own control (a "Tail" amount, off by default) if you want that sound without the rest of Wild.
