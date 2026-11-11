# Bitrot feature map

Written 4 November 2026. Bitrot is a VST3 recreating the EarthQuaker Devices Data Corrupter, a modulated monophonic harmonising PLL, in the Flip Fella family look (Speckle, Jetstream, Murmur). Prism Patch is the reference for foot control, Keeper and the Pedal page. Changeling is the reference for low-register pitch tracking.

## Which version?

There is only one Data Corrupter. It came out in September 2017 and EarthQuaker still sells it (US $229). I found no v2, mini, reissue or firmware revision; retailers list 2017 to 2026 with the same controls throughout. The manual has had several print revisions (R3 EU, R4 web) but no new features.

**Recommendation: recreate the Data Corrupter as it is, and add only what bass, footswitches and a DAW need.** Its whole charm is the crude analogue PLL, so the plan is to model that loop, not tidy it away.

One correction to the starting pointers: the three voices are **Square** (your input squared up into a fuzz), **Subharmonic** (that square, or the oscillator, divided down) and **Oscillator** (the PLL's square-wave master oscillator, set to an interval above). The "interval oscillator" and the "master oscillator" are the same voice.

## Feature map

Control names and behaviour come from the operation manual (EU R3, 2020), EarthQuaker's blog posts and reviews. Interval names are the panel's own: octave, then interval (U unison, 5 fifth, M3 major third, m7 minor seventh, M2 major second).

### Voice mixer

| Original | What it does | Bitrot |
| --- | --- | --- |
| Square | Level of the square-wave fuzz made from your input. Endless sustain, then the note cuts off suddenly | Square knob. Band-limited square so it doesn't alias, with the same full-level start and sudden stop |
| Subharmonic | Level of the subharmonic voice | Subharmonic knob |
| Oscillator | Level of the master oscillator voice | Oscillator knob |
| Level | Master output volume of the effect | Level knob |
| (no dry knob) | The original has no clean blend | **Dry** knob on the main page, 0 by default so it sounds like the pedal. Keeps the clean low end under the synth on bass. Also an expression target, so the pedal can fade from clean to corrupted |

### Master Oscillator

| Original | What it does | Bitrot |
| --- | --- | --- |
| Interval (8-way rotary) | The PLL multiplies the root by 1 to 8, so the oscillator plays the harmonic series. Panel: U, +1 U, +1 5, +2 U, +2 M3, +2 5, +2 m7, +3 U | Eight-step flip card. Same ×1 to ×8 maths. Footswitch D and the expression pedal can push it to ×16 |
| Root (3-way: Unison, −1, −2) | Divides your input by 1, 2 or 4 before the PLL. The manual: some frequencies are too high for the oscillator, so shifting them down brings them back into range. On bass, Unison is the stable setting | Same three positions. Output pitch is input × interval ÷ root, so −2 with Unison puts the oscillator two octaves down |

### Subharmonic

| Original | What it does | Bitrot |
| --- | --- | --- |
| Interval (8-way rotary) | Divides by 2 to 9, mirroring the oscillator's harmonics downwards. Panel: −1 U, −1 5, −2 U, −2 M3, −2 5, −2 m7, −3 U, −3 M2 | Eight-step flip card. Real edge-counting dividers, so a missed or doubled edge stutters the sub the way the pedal does |
| Root (Unison or Oscillator) | Unison divides the Square voice: steadier, and the Frequency Modulator doesn't touch it. Oscillator divides the master oscillator, and the manual says the result is "the sum and difference of both the Master Oscillator and Subharmonic", with glide and vibrato on both | Same two positions. In Oscillator, the oscillator and sub squares are also combined the way digital logic combines them (exclusive-or), which makes those sum and difference tones. That's my reading of the manual's description, and the render test will compare both versions by ear |

### Frequency Modulator

| Original | What it does | Bitrot |
| --- | --- | --- |
| Glide or Vibrato (toggle) | Glide slides the oscillator between notes; at fast settings the manual says it turns into a phase-shifting effect. Vibrato wobbles it, "a retro sci-fi laser effect" | Same toggle. Vibrato is a triangle LFO on the PLL's control voltage, the same place the pedal applies it |
| Rate | Glide time in Glide, vibrato speed in Vibrato | Same knob. Vibrato depth isn't adjustable on the original; Bitrot keeps it fixed on the panel with a Depth control on the Deep page |

### Footswitch and hardware

| Original | Bitrot |
| --- | --- |
| One Flexi-Switch: tap latches, hold plays the effect only while held | On-screen bypass, plus footswitch A with the same tap and hold |
| Mono in and out, 9 V, 26 mA, 1 MΩ input, no MIDI, no expression jack, no gain control (fixed for tracking) | Mono in, the same signal on both outputs. No original MIDI map to hide, so the Pedal page has no "Original pedal MIDI map" setting |

## The tracking, and the quirks people love

What owners and reviewers describe, and how Bitrot does it:

| The pedal | Bitrot |
| --- | --- |
| A crude analogue PLL: a phase comparator, a loop filter and a square-wave VCO with a divider in its feedback path | Modelled sample by sample in the same order: input → square-up → Root divider → phase comparator → loop filter → VCO → ÷ interval back to the comparator |
| Notes start at full level with no envelope and sustain endlessly, then break into glitches and clicks or stop dead | The square-up has a gate with a little hysteresis. As a note decays the comparator chatters before the gate shuts, which gives the same crackle and then silence |
| Chords, double stops and sloppy playing make the oscillator and sub glitch out | Two strings feed the comparator extra edges, so the dividers miscount and the loop hunts. Not filtered out |
| The loop locks to a harmonic instead of the note, jumping an octave or a fifth | The loop's capture range lets it lock to harmonics at Wild settings |
| Hammer-ons come out "powerful, smooth and sputtery at the same time" | The loop filter slews between notes, so a fast legato change chirps through the gap |
| The manual: neck pickup tracks best, precise single notes track cleanly, "chords and sloppy playing will result in chaos", and weak signals (guitar volume backed off) cause poor tracking and gating | The comparator has a fixed gain, as on the pedal, with the gate and chaos that follow from it. The Bass input chip and the tracking compressor exist to rescue weak signals |
| Tracking is weaker on bass. The square trims the low end. EarthQuaker suggest a compressor first. Root at Unison is the stable setting on bass | See below |

### Tight low end for bass

The pedal's loop is the source of both the fun and the problems on bass, so Bitrot gives the loop help it can switch off.

- **Input chips: Bass, Guitar, Synth.** They set the range and the loop's speed. Bass goes down to 28 Hz (low B on a 5-string is 30.9 Hz).
- **Lock assist.** Changeling's low-register tracker (McLeod on a decimated signal, octave guard, hysteresis on decay) runs alongside the loop. It tells the loop where to land and lines its phase up on the next edge. Measured in the render test: open E on bass locks in about 45 ms, every guitar note from E2 to E5 within 25 ms, and the lowest bass notes within three and a half cycles (about 100 ms on low B, where one cycle is already 32 ms). Held notes stay within 2 cents, with no octave hopping when the second harmonic is louder than the fundamental.
- **Corrupt: Tight to Wild** (on the panel). Tight leans on lock assist. Wild takes it away and lets the raw loop hunt, chirp, lock to harmonics and stutter. The default sits a little towards Wild on guitar and Tight on bass.
- **Built-in tracking compressor.** EarthQuaker's own advice, built in before the comparator. It only feeds the tracker, never the sound.
- **Sub floor, so the subharmonic doesn't turn to mud.** On low E, −3 M2 is 4.6 Hz: a flutter, not a note. With Floor on, any subharmonic that would fall below 30 Hz moves up by octaves until it doesn't. The interval stays the same note name, just higher. Every sub voice is a clean band-limited square, and a gentle 25 Hz high-pass keeps DC and rumble out. **Floor is a toggle in the Subharmonic section**, on by default with the Bass input chip and off with Guitar and Synth. Off brings back the pedal's sub-audio flutter.
- **Tracking readout.** Note, cents and a lock bar in the strip, so you can see when the loop has let go.

## Foot control

Defaults follow the house rules: footswitches A to D send momentary CC 20, 21, 22, 23 and expression sends CC 11, all on channel 16, read straight from the footswitch's USB cable. The shared `PedalControl` module and `Flip Fella Pedals.xml` settings come from Prism Patch unchanged, so the M-Vave Chocolate Plus and M-Audio EX-P work with no mapping.

| Footswitch | Tap | Hold |
| --- | --- | --- |
| A | Bypass | On only while held (the original's Flexi-Switch) |
| B | Next subharmonic interval | Previous subharmonic interval |
| C | Freeze latches on or off | Freeze while held |
| D | Octave up latches on or off | Octave up while held |
| A+B | Next preset | |
| C+D | Hand the footswitches to the next Bitrot | Keeper (hold 1 second) |

Why these:

- **A is the original's only switch**, and it keeps the original's Flexi-Switch behaviour: tap to latch, hold to play Bitrot only while your foot is down, for stabs of glitch in a clean part.
- **B steps the subharmonic**, as you suggested. With eight steps, a forward-only cycle means up to seven taps to go back one, so holding B steps backwards. The flip card and Pebble show where you are.
- **C freezes**, as you suggested. Tap latches and hold is momentary, so you can do both. Freeze holds the oscillator and subharmonic on the note playing at the moment you pressed (Bitrot keeps a short pitch history, so the pitch from before the combo check is the one that freezes). The Square voice keeps following your playing, so you can play over your own drone.
- **D jumps the oscillator up an octave**, as you suggested, by doubling the interval (×3 becomes ×6, ×8 becomes ×16). Same tap-latches, hold-is-momentary rule, so it works as a riff accent or for a whole section.
- **Keeper stays on C+D held 1 second.** No clash: when C and D go down together, neither freeze nor octave up fires, so a Keeper can't change the sound.

**Expression: Interval.** Heel is unison, toe is +3 octaves, stepping through the eight harmonics as you rock. That's a bugle-call arpeggio over every note, the most chiptune thing the pedal can do, and no other plugin in the family does it. With Glide on, the steps slide into each other. Steps have hysteresis so a pedal resting between two doesn't flicker. On the Pedal page it can target **Dry** (heel clean, toe corrupted), Rate (glide time or vibrato speed), any level, Corrupt, or a smooth, unstepped Bend of the interval instead.

Pebble taps its foot on every press, leans with the expression pedal, and glitches when the loop loses lock (see Look).

## Look

Flip Fella dark family: the same black housing, cards, ink and accent colours as Speckle, Jetstream and Murmur, Roboto Condensed Bold.

- **Main page** in the pedal's four sections, left to right: Voice Mixer, Subharmonic, Frequency Modulator, Master Oscillator. Rotaries are flip cards showing the interval name.
- **Strip** under the panel: input chips, tracking readout and lock bar, Corrupt, presets, footswitches A to D as on-screen buttons, expression bar, bypass.
- **Deep page:** vibrato Depth, tracking compressor, gate sensitivity, floor frequency, sum and difference or plain divide, Bend.
- **Pedal page** copied from Prism Patch.
- **Pebble** hops on each new note. When the loop loses lock, Pebble glitches: RGB split, sliced rows shifting sideways, square pixel eyes, and a stutter that repeats the last frame. Frozen, Pebble holds still with a frost outline. Octave up, Pebble stretches tall.

## Decisions (4 November 2026)

- Recreate the Data Corrupter as it is; there is only one version.
- Interval names confirmed from the manual: the oscillator plays harmonics 1 to 8 and the subharmonic divides by 2 to 9.
- Level knob added, as on the pedal. Dry knob on the main page, 0 by default, and available as an expression target.
- Sub floor is a toggle, on by default for Bass.
- Freeze holds the oscillator and subharmonic; the Square voice keeps following your playing.
- Hold B steps the subharmonic back.
- Vibrato depth fixed on the panel, Depth on the Deep page.

## Tracking fix (11 November 2026)

Version 0.2.0 fixes the unruly tracking found against two demo recordings: glide is now a steady octaves-per-second slide, the tracker no longer holds notes an octave low, and quiet inputs track as well as hot ones. Details and the plan for Changeling and Divebomb: `docs/tracking-plan.md`.

## Portability

- Windows x64 VST3 and standalone from GitHub Actions on every push, published to the `builds` branch like the other plugins.
- Linux ARM64 VST3 and standalone for the Pi pedalboard, built natively on GitHub's ARM64 runner and published to the same branch.
- The engine is plain C++17 with no JUCE, so the render test (bass from low B, guitar phrases, chords, decays, footswitch and expression events) runs on both platforms in CI.
- One PLL, one tracker and three square voices are light. It should sit well under 1% of a desktop core, leaving plenty of room on a Pi 5.

## Sources

- EarthQuaker Devices: Data Corrupter operation manual EU R3 (2020, supplied by Kevin), product page, "Aaron's Bass Hole: Decoding the Data Corrupter", "6 Super-Usable Data Corrupter Settings"
- Reviews: Premier Guitar, Guitar World, MusicRadar, Harmony Central, Guitar.com, Bonedo, Gearphoria, Perfect Circuit
- Owners: TalkBass Data Corrupter thread, Sweetwater and Thomann customer reviews, Get Offset podcast bass and guitar demo
