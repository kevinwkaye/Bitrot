#pragma once

#include "PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>

// Flip Fella palette, shared across all of Kevin's builds.
namespace ff
{
const juce::Colour bg        { 0xff050505 };
const juce::Colour face      { 0xff0d0d0d };
const juce::Colour housing   { 0xff060606 };
const juce::Colour card      { 0xff1b1b1b };
const juce::Colour cardHi    { 0xff252525 };
const juce::Colour cardLow   { 0xff141414 };
const juce::Colour ink       { 0xfff3f0e8 };
const juce::Colour dim       { 0xffa29e95 };
const juce::Colour faint     { 0xff5a5852 };
const juce::Colour hint      { 0xff8c887f };
const juce::Colour line      { 0xff3a3a3a };
const juce::Colour ring      { 0xff1a1a1a };
const juce::Colour pin       { 0xff3c3c3c };
const juce::Colour red       { 0xffe8392f };
const juce::Colour teal      { 0xff3fd6c8 };
const juce::Colour coral     { 0xffff7a59 };
const juce::Colour amber     { 0xfff2c230 };
const juce::Colour blob      { 0xff4d5bff };
const juce::Colour blobShade { 0xff2329a8 };
const juce::Colour eye       { 0xffffffff };
const juce::Colour faceInk   { 0xff0b0b12 };
const juce::Colour green     { 0xff4fcf6a };
const juce::Colour frost     { 0xffcfeeff };
// the four preset banks
const juce::Colour bankR     { 0xffe8392f };
const juce::Colour bankY     { 0xfff2c230 };
const juce::Colour bankG     { 0xff4fcf6a };
const juce::Colour bankB     { 0xff4d8dff };

juce::Font font (float height, float kerning = 0.04f);
juce::String utf8 (const juce::String&);
void label (juce::Graphics&, const juce::String& text, juce::Rectangle<int> r,
            juce::Justification j = juce::Justification::centredLeft, juce::Colour c = dim, float size = 13.0f);
} // namespace ff

namespace ui
{
// A split-flap card that flips like the Karlsson clock when its value changes.
class FlipCard : public juce::Component
{
public:
    void setValue (const juce::String& v);
    void setTextHeight (float h) { textHeight = h; }
    void advance (double dt);
    void paint (juce::Graphics&) override;
    std::function<void()> onClick;
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }

private:
    void drawHalf (juce::Graphics&, juce::Rectangle<float> card, const juce::String& text, bool top, float scale, bool shade);
    juce::String current, previous;
    float progress = 1.0f;
    float textHeight = 34.0f;
};

// Rounded pill buttons bound to a choice or toggle parameter.
class Pills : public juce::Component
{
public:
    Pills (juce::AudioProcessorValueTreeState&, const juce::String& paramId, juce::StringArray labels, float textSize = 13.0f, float gap = 4.0f);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> cell (int i) const;
    juce::StringArray labels;
    int selected = 0;
    float textSize, gap;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

// Pills not tied to a parameter: tabs, menus and one-shot actions.
class ActionPills : public juce::Component
{
public:
    ActionPills (juce::StringArray labels, float textSize = 12.0f, float gap = 4.0f);
    std::function<void (int)> onClick;
    std::function<int()> selected;        // which one is lit, or -1
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> cell (int i) const;
    juce::StringArray labels;
    float textSize, gap;
};

class FlipLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle, juce::Slider&) override;
};

// A knob drawn as a small clock face, with its name above and value below.
class Dial : public juce::Component
{
public:
    Dial (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& caption, int knobSize = 64);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Slider slider;
    juce::RangedAudioParameter* param = nullptr;
    juce::String caption;
    int knobSize;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// A slim horizontal bar for one parameter.
class BarSlider : public juce::Component
{
public:
    BarSlider (juce::AudioProcessorValueTreeState&, const juce::String& paramId, juce::Colour fill);
    void resized() override { slider.setBounds (getLocalBounds()); }

private:
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// The eight positions of a rotary switch: round dots for the subharmonic, a rising
// ladder for the oscillator's harmonic series. Click one to choose it.
class Steps : public juce::Component
{
public:
    enum Style { Dots, Ladder };
    Steps (BitrotProcessor& p, const juce::String& paramId, Style s, juce::Colour c);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> cell (int i) const;
    BitrotProcessor& proc;
    juce::RangedAudioParameter* param = nullptr;
    Style style;
    juce::Colour colour;
};

// The three voices as square waves, at their real ratios, shaking when the loop slips.
class VoiceScope : public juce::Component
{
public:
    explicit VoiceScope (BitrotProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;

private:
    BitrotProcessor& proc;
    juce::Random random;
};

// What the frequency modulator does: a glide between notes, or the vibrato wave.
class FmCurve : public juce::Component
{
public:
    explicit FmCurve (BitrotProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;

private:
    BitrotProcessor& proc;
};

// The note Bitrot heard, its frequency and how well the loop holds it.
class TrackingStrip : public juce::Component
{
public:
    explicit TrackingStrip (BitrotProcessor& p);
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick (double now, double dt);

private:
    BitrotProcessor& proc;
    FlipCard note;
    juce::String hzText = "WAITING FOR A NOTE", word = "SILENT";
    juce::Colour wordColour = ff::faint;
    float lock = 0.0f;
    double lastHeard = -100.0;
    juce::Random random;
};

// The four footswitches as on-screen buttons: tap and hold work as on the floor.
class FootButton : public juce::Component
{
public:
    FootButton (BitrotProcessor& p, int sw) : proc (p), index (sw) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { proc.footDown (index); repaint(); }
    void mouseUp (const juce::MouseEvent&) override { proc.footUp (index); repaint(); }

private:
    BitrotProcessor& proc;
    int index;
};

class BypassButton : public juce::Component
{
public:
    explicit BypassButton (BitrotProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    BitrotProcessor& proc;
};

// Sixteen presets in red, yellow, green and blue banks. Tap to load, hold to save.
class PresetBanks : public juce::Component, private juce::Timer
{
public:
    explicit PresetBanks (BitrotProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    juce::Rectangle<float> cell (int i) const;
    BitrotProcessor& proc;
    int pressed = -1, flashSlot = -1;
    double downAt = 0, flashUntil = 0;
    bool saved = false;
};

// The expression pedal: where it is and what it sweeps. Click to choose the target.
class ExpressionBar : public juce::Component
{
public:
    explicit ExpressionBar (BitrotProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    BitrotProcessor& proc;
};

// Pebble from Flip Fella. Hops on each new note, taps a foot on each footswitch, leans
// with the expression pedal, and glitches when the loop loses lock: colours split, rows
// tear sideways, the eyes turn to pixels and the picture stutters. Frozen, Pebble holds
// still in a frost outline. Octave up, Pebble stretches tall.
class Pebble : public juce::Component
{
public:
    explicit Pebble (BitrotProcessor& p) : proc (p) {}
    void update (double now, double dt);
    void paint (juce::Graphics&) override;

private:
    struct Tear { float y = 0, h = 0, shift = 0; int colour = 0; bool on = false; };
    BitrotProcessor& proc;
    float look = 0, hop = 0, foot = 0, lean = 0, glitch = 0, stretch = 0, splitX = 0, jx = 0, jy = 0;
    std::array<Tear, 4> tears;
    std::array<juce::Point<float>, 5> pixels;
    std::array<bool, 5> pixelOn {};
    int footSide = 0, lastOnsets = 0, lastFoot = 0, stutter = 0;
    bool asleep = false, blinking = false, frozen = false, squareEyes = false, bodyTeal = false;
    double lastSound = 0, nextBlink = 2.0, blinkUntil = 0, now = 0;
    juce::Random random;
};
} // namespace ui

class BitrotEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BitrotEditor (BitrotProcessor&);
    ~BitrotEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void showPage (int page);
    void tickForSnapshot() { timerCallback(); }

private:
    void timerCallback() override;
    void section (juce::Graphics&, juce::Rectangle<int>) const;
    void footswitchMenu();
    template <typename T, typename... Args> T& make (int page, Args&&... args);

    BitrotProcessor& proc;
    ui::FlipLookAndFeel lnf;
    int page = 0;
    double lastTick = 0;
    juce::String footSay, subHzLine, subFloorLine, oscHzLine, rateLine;
    bool subLifted = false, octLit = false;

    std::vector<std::unique_ptr<juce::Component>> owned;
    std::array<std::vector<juce::Component*>, 3> pages;   // main, deep, pedal

    ui::FlipCard* subCard = nullptr;
    ui::FlipCard* oscCard = nullptr;
    ui::FlipCard* pedalCards[4] {};
    ui::ActionPills* tabs = nullptr;
    ui::VoiceScope* scope = nullptr;
    ui::FmCurve* fmCurve = nullptr;
    ui::Steps* subSteps = nullptr;
    ui::Steps* oscSteps = nullptr;
    ui::TrackingStrip* tracking = nullptr;
    ui::Pebble* pebble = nullptr;
    ui::PresetBanks* banks = nullptr;
    ui::FootButton* feet[4] {};
    ui::BypassButton* bypass = nullptr;
    ui::ExpressionBar* expBar = nullptr;
    ui::ExpressionBar* expBarPedal = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BitrotEditor)
};
