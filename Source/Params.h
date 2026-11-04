#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

namespace ParamIDs
{
// voice mixer
inline constexpr auto squareLevel = "square";
inline constexpr auto subLevel = "subLevel";
inline constexpr auto oscLevel = "oscLevel";
inline constexpr auto dry = "dry";
inline constexpr auto level = "level";
// subharmonic
inline constexpr auto subInterval = "subInterval";
inline constexpr auto subRoot = "subRoot";
inline constexpr auto floor = "floor";
// frequency modulator
inline constexpr auto fmMode = "fmMode";
inline constexpr auto rate = "rate";
// master oscillator
inline constexpr auto oscInterval = "oscInterval";
inline constexpr auto oscRoot = "oscRoot";
// tracking and performance
inline constexpr auto input = "input";
inline constexpr auto corrupt = "corrupt";
inline constexpr auto bypass = "bypass";
inline constexpr auto freeze = "freeze";
inline constexpr auto octUp = "octUp";
// deep page
inline constexpr auto depth = "depth";
inline constexpr auto trackComp = "trackComp";
inline constexpr auto gate = "gate";
inline constexpr auto floorHz = "floorHz";
inline constexpr auto subMix = "subMix";
inline constexpr auto bend = "bend";
// pedal page
inline constexpr auto expTarget = "expTarget";
inline constexpr auto keeper = "keeper";
} // namespace ParamIDs

namespace names
{
// parameter names are plain text for the host; the panel draws its own labels (PluginEditor)
inline const juce::StringArray oscIntervals { "Unison", "+1 oct", "+1 oct 5th", "+2 oct", "+2 oct M3", "+2 oct 5th", "+2 oct m7", "+3 oct" };
inline const juce::StringArray subIntervals { "-1 oct", "-1 oct 5th", "-2 oct", "-2 oct M3", "-2 oct 5th", "-2 oct m7", "-3 oct", "-3 oct M2" };

struct ExpTarget { const char* id; const char* label; };
inline const std::vector<ExpTarget>& expTargets()
{
    static const std::vector<ExpTarget> t {
        { ParamIDs::oscInterval, "Interval" }, { ParamIDs::dry, "Dry" }, { ParamIDs::rate, "Rate" },
        { ParamIDs::corrupt, "Corrupt" }, { ParamIDs::squareLevel, "Square" }, { ParamIDs::subLevel, "Subharmonic" },
        { ParamIDs::oscLevel, "Oscillator" }, { ParamIDs::level, "Level" }, { ParamIDs::bend, "Bend" }
    };
    return t;
}
inline juce::StringArray expTargetNames()
{
    juce::StringArray s;
    for (const auto& t : expTargets()) s.add (t.label);
    return s;
}
} // namespace names

inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto tenth = [] (float v, int) { return String (v * 10.0f, 1); };
    auto knob = [&] (const char* id, const char* name, float def, std::function<String (float, int)> text = {})
    {
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 1.0f), def,
                                                            AudioParameterFloatAttributes().withStringFromValueFunction (text ? text : tenth)));
    };
    auto choice = [&] (const char* id, const char* name, const StringArray& opts, int def)
    {
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, opts, def));
    };
    auto toggle = [&] (const char* id, const char* name, bool def)
    {
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def));
    };

    // ---- voice mixer ----
    knob (ParamIDs::squareLevel, "Square", 0.45f);
    knob (ParamIDs::subLevel, "Subharmonic", 0.6f);
    knob (ParamIDs::oscLevel, "Oscillator", 0.6f);
    knob (ParamIDs::dry, "Dry", 0.0f);
    knob (ParamIDs::level, "Level", 0.6f);

    // ---- subharmonic ----
    choice (ParamIDs::subInterval, "Subharmonic Interval", names::subIntervals, 0);
    choice (ParamIDs::subRoot, "Subharmonic Root", { "Unison", "Oscillator" }, 0);
    toggle (ParamIDs::floor, "Sub Floor", false);

    // ---- frequency modulator ----
    choice (ParamIDs::fmMode, "Glide or Vibrato", { "Glide", "Vibrato" }, 0);
    knob (ParamIDs::rate, "Rate", 0.6f);

    // ---- master oscillator ----
    choice (ParamIDs::oscInterval, "Oscillator Interval", names::oscIntervals, 1);
    choice (ParamIDs::oscRoot, "Oscillator Root", { "Unison", "-1", "-2" }, 0);

    // ---- tracking and performance ----
    choice (ParamIDs::input, "Input", { "Guitar", "Bass", "Synth" }, 0);
    knob (ParamIDs::corrupt, "Corrupt", 0.3f, [] (float v, int) { return v < 0.34f ? String ("Tight") : (v < 0.67f ? String ("Loose") : String ("Wild")); });
    toggle (ParamIDs::bypass, "Bypass", false);
    toggle (ParamIDs::freeze, "Freeze", false);
    toggle (ParamIDs::octUp, "Octave Up", false);

    // ---- deep page ----
    knob (ParamIDs::depth, "Vibrato Depth", 0.4f, [] (float v, int) { return String (0.1f + v * v * 5.9f, 1) + " st"; });
    toggle (ParamIDs::trackComp, "Tracking Compressor", true);
    knob (ParamIDs::gate, "Gate Sensitivity", 0.5f);
    choice (ParamIDs::floorHz, "Floor", { "25 Hz", "30 Hz", "40 Hz" }, 1);
    choice (ParamIDs::subMix, "Oscillator Sub", { "Sum and difference", "Divided" }, 0);
    knob (ParamIDs::bend, "Bend", 0.0f, [] (float v, int) { return v < 0.005f ? String ("Off") : "+" + String (v * 36.0f, 1) + " st"; });

    // ---- pedal page ----
    choice (ParamIDs::expTarget, "Expression Target", names::expTargetNames(), 0);
    toggle (ParamIDs::keeper, "Keeper", false);

    return { p.begin(), p.end() };
}
