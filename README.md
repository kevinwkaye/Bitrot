# Bitrot

A VST3 for Ableton Live recreating the EarthQuaker Devices Data Corrupter, a modulated monophonic harmonising PLL, in the Flip Fella look. All DSP is original code written from the operation manual and public descriptions of the pedal.

Three square-wave voices from one note:

| Voice | What it is |
| --- | --- |
| Square | Your input squared up into a fuzz. Full level at once, endless sustain, then a sudden stop |
| Subharmonic | The square (or the oscillator) divided by 2 to 9: −1 U, −1 5, −2 U, −2 M3, −2 5, −2 m7, −3 U, −3 M2 |
| Oscillator | A phase-locked loop multiplying your note by 1 to 8: U, +1 U, +1 5, +2 U, +2 M3, +2 5, +2 m7, +3 U |

## Install

**Windows**

1. Open the **builds** branch of this repo and download `Bitrot-Windows.zip`.
2. Unzip it and copy the `Bitrot.vst3` folder to `C:\Program Files\Common Files\VST3`.
3. In Ableton, rescan plugins (Preferences, Plug-Ins, Rescan). Bitrot appears under Audio Effects.

**Linux ARM64 (the Pi pedalboard)**

1. Download `Bitrot-Linux-ARM64.zip` from the **builds** branch.
2. Unzip it and copy `Bitrot.vst3` into `~/.vst3`.
3. Or run `Bitrot-standalone` directly and pick your audio interface in its Options menu.

## The panel

- **Voice Mixer:** Square, Subharmonic and Oscillator levels, Level, and **Dry** (new, 0 sounds like the pedal; turn it up on bass to keep your clean low end). The scope shows the three voices at their real ratios and shakes when the loop slips.
- **Subharmonic:** the eight-step switch (tap the card or a dot), Root (Unison divides the square; Oscillator divides the oscillator and mixes their sum and difference, with glide and vibrato on both), and **Floor**.
- **Frequency Modulator:** Glide or Vibrato and Rate. **Corrupt** runs from Tight (lock assist) to Wild (the raw loop hunts, chirps and stutters).
- **Master Oscillator:** the harmonic ladder (tap a bar), Root (Unison, −1, −2), the oscillator's pitch and the Octave Up light, and the expression bar.
- **Deep:** tracking compressor, gate sensitivity, floor frequency, sum and difference or plain divide, vibrato depth and Bend.
- **Pedal:** footswitches, expression and Keeper.

## Bass

- Pick **BASS** under Input. Tracking reaches low B on a five-string; the loop locks in about 45 ms on open E.
- **Floor** switches on with Bass: any subharmonic below 30 Hz moves up by octaves, keeping its note name, so −3 M2 on low E is a note instead of a 4.6 Hz flutter. Switch it off for the pedal's flutter.
- Bass leans tight at the same Corrupt setting. Turn Corrupt up when you want the glitches.

## Footswitches and expression (optional)

Everything works from the screen, so you don't need any pedals. If you have a MIDI footswitch:

1. Set its first four switches to send **momentary CC 20, 21, 22 and 23 on MIDI channel 16** (127 when pressed, 0 when released). Or use **Learn** on the Pedal page to use whatever yours sends.
2. Set an expression pedal input to **CC 11 on channel 16**, or click the expression bar on the Pedal page and choose Learn.
3. On the Pedal page choose **Calibrate**, then rock the pedal from heel to toe once.

These settings are shared by every Flip Fella plugin on your computer, so you set them up once.

| Footswitch | Tap | Hold |
| --- | --- | --- |
| A | Bypass | Flips on or off while held (the pedal's Flexi-Switch) |
| B | Next subharmonic interval | Previous subharmonic interval |
| C | Freeze on or off | Freeze while held |
| D | Octave up on or off | Octave up while held |
| A+B | Next preset | |
| C+D | Hand the footswitches to the next Bitrot | Hold a second: save a Keeper |

Freeze holds the oscillator and subharmonic on the note you were playing when C went down, and the Square keeps following you, so you can play over your own drone. Pressing C and D together never freezes or jumps an octave, so a Keeper can't change the sound.

The expression pedal sweeps the **Interval**: heel is unison, toe is +3 octaves, stepping through the harmonic series. Click the expression bar to sweep Dry, Rate, Corrupt, any level or Bend instead.

### Getting footswitch MIDI to the plugin in Ableton

Bitrot reads your footswitch straight from its USB cable, so every copy you load works with no MIDI track. The dot next to **FOOTSWITCHES** in the bottom strip turns green when it's connected.

1. In Ableton's Preferences, Link Tempo MIDI, turn **Track**, **Sync** and **Remote** off for your footswitch's input. Windows lets only one program use a MIDI input at a time, so Ableton has to let go of it.
2. Load Bitrot. That's it.

If the dot stays grey, click **FOOTSWITCHES**. The menu says what's wrong, and you can pick your footswitch from the list. With several copies loaded, the footswitches drive the one you opened or clicked last. Tap C+D to hand them to the next copy.

### Kevin's setup (M-Vave Chocolate Plus and M-Audio EX-P)

In CubeSuite: footswitches A to D as momentary CC 20 to 23, the pedal jack set to expression input on CC 11. It always sends on channel 1 whatever CubeSuite says; read directly from its cable, Bitrot accepts any channel. Put the Chocolate Plus switch on **U** and connect with the USB cable, not Bluetooth. The EX-P needs a 3.5 mm to 6.35 mm TRS adapter and reads backwards on the Chocolate Plus, which Calibrate fixes.

The pedal has no MIDI of its own, so there is no original MIDI map to clash with the footswitches.

## Presets

Sixteen slots in red, yellow, green and blue banks: tap to load, hold to save. The red bank starts with Equinox and Fisted Fifth Riffs (after the manual's drawings), Bass Hole (after EarthQuaker's bass column) and Laser.

## Keeper

Hold C+D for a second, or press Keep This Moment on the Pedal page. Bitrot saves the last 8 seconds in and out, plus every setting and what the loop was doing, to Documents / Flip Fella Keepers.

## Building

GitHub Actions builds the Windows VST3 and the Linux ARM64 VST3 on every push, runs the engine and footswitch tests on both, validates the Windows build with pluginval, and publishes both zips to the `builds` branch. To build locally you need CMake 3.22+ and a C++17 compiler; JUCE downloads automatically.

```
cmake -S . -B build
cmake --build build --config Release
```

`Tests/RenderTest.cpp` needs no JUCE. Pass a folder to write a WAV of each scenario. `bitrot_foot_test --snapshot <folder>` writes the panel pages as PNGs.

## Project layout

| Path | What it holds |
| --- | --- |
| `Source/engine/` | The PLL engine and tracker, plain C++17 with no JUCE |
| `Source/Params.h` | Every parameter, its range and default |
| `Source/PluginProcessor.*` | Host wrapper, footswitch jobs, presets, state |
| `Source/PluginEditor.*` | The Main, Deep and Pedal pages and Pebble |
| `Source/PedalControl.*`, `Source/Keeper.*` | Shared with every Flip Fella plugin |
| `docs/feature-map.md` | The original's controls and how Bitrot does each |
