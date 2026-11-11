#include "PluginEditor.h"
#include "BinaryData.h"

using namespace juce;

// =====================================================================================
// Palette helpers
// =====================================================================================
namespace ff
{
Font font (float height, float kerning)
{
    static Typeface::Ptr typeface = Typeface::createSystemTypefaceFor (BinaryData::RobotoCondensedBold_ttf,
                                                                       BinaryData::RobotoCondensedBold_ttfSize);
    return Font (FontOptions (typeface).withHeight (height).withKerningFactor (kerning));
}

// Source literals are UTF-8 but JUCE reads plain C strings as Latin-1, so "·" arrives as
// two characters. Put such text back together before drawing it.
String utf8 (const String& s)
{
    bool suspect = false;
    for (auto p = s.getCharPointer(); ! p.isEmpty() && ! suspect; ++p)
        suspect = *p >= 0xC2 && *p <= 0xF4;
    if (! suspect) return s;
    MemoryBlock bytes;
    for (auto p = s.getCharPointer(); ! p.isEmpty(); ++p)
    {
        if (*p > 0xFF) return s;   // already proper text
        const char b = (char) (uint8) *p;
        bytes.append (&b, 1);
    }
    return String::fromUTF8 ((const char*) bytes.getData(), (int) bytes.getSize());
}

void label (Graphics& g, const String& text, Rectangle<int> r, Justification j, Colour c, float size)
{
    g.setColour (c);
    g.setFont (font (size, 0.08f));
    g.drawText (utf8 (text), r, j, false);
}
} // namespace ff

namespace
{
// the panel's own interval labels: octave, then interval
const char* const oscNames[8] = { "UNISON", "+1 OCT", "+1 OCT · 5", "+2 OCT", "+2 OCT · M3", "+2 OCT · 5", "+2 OCT · m7", "+3 OCT" };
const char* const oscNamesUp[8] = { "+1 OCT", "+2 OCT", "+2 OCT · 5", "+3 OCT", "+3 OCT · M3", "+3 OCT · 5", "+3 OCT · m7", "+4 OCT" };
const char* const subNames[8] = { "−1 OCT", "−1 OCT · 5", "−2 OCT", "−2 OCT · M3", "−2 OCT · 5", "−2 OCT · m7", "−3 OCT", "−3 OCT · M2" };
const char* const footTaps[4] = { "BYPASS", "NEXT SUB", "FREEZE LATCH", "OCT UP LATCH" };
const char* const footHolds[4] = { "HOLD FLIPS", "HOLD PREV SUB", "HOLD FREEZE", "HOLD OCT UP" };

String hz (float v) { return v >= 100.0f ? String (roundToInt (v)) + " HZ" : String (v, 1) + " HZ"; }
} // namespace

namespace ui
{
static double nowSeconds() { return Time::getMillisecondCounterHiRes() * 0.001; }

static void pill (Graphics& g, Rectangle<float> b, const String& text, bool on, float size, Colour onColour = ff::ink)
{
    const float r = b.getHeight() * 0.5f;
    if (on) { g.setColour (onColour); g.fillRoundedRectangle (b, r); }
    else { g.setColour (ff::line); g.drawRoundedRectangle (b.reduced (1.0f), r, 2.0f); }
    g.setColour (on ? ff::bg : ff::ink);
    g.setFont (ff::font (size, 0.06f));
    g.drawFittedText (ff::utf8 (text), b.reduced (4.0f, 0.0f).toNearestInt(), Justification::centred, 1, 0.8f);
}

static String noteName (float midi)
{
    static const char* n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int m = (int) std::lround (midi);
    return String (n[((m % 12) + 12) % 12]) + String (m / 12 - 1);
}

static int choiceOf (BitrotProcessor& p, const char* id, int hi)
{
    return jlimit (0, hi, roundToInt (p.apvts.getRawParameterValue (id)->load()));
}

// A transparent area that does something when clicked
class ClickArea : public Component
{
public:
    std::function<void()> onClick;
    ClickArea() { setMouseCursor (MouseCursor::PointingHandCursor); }
    void mouseDown (const MouseEvent&) override { if (onClick) onClick(); }
};

// =====================================================================================
// FlipCard
// =====================================================================================
void FlipCard::setValue (const String& v)
{
    if (v == current) return;
    previous = current;
    current = v;
    progress = previous.isEmpty() ? 1.0f : 0.0f;
    repaint();
}

void FlipCard::advance (double dt)
{
    if (progress < 1.0f)
    {
        progress = jmin (1.0f, progress + (float) (dt / 0.26));
        repaint();
    }
}

void FlipCard::drawHalf (Graphics& g, Rectangle<float> card, const String& text, bool top, float scale, bool shade)
{
    const float hinge = card.getCentreY();
    const float halfH = card.getHeight() * 0.5f;
    if (scale <= 0.001f) return;

    Graphics::ScopedSaveState save (g);
    auto clip = top ? Rectangle<float> (card.getX(), hinge - halfH * scale, card.getWidth(), halfH * scale)
                    : Rectangle<float> (card.getX(), hinge, card.getWidth(), halfH * scale);
    g.reduceClipRegion (clip.getSmallestIntegerContainer());
    g.addTransform (AffineTransform::scale (1.0f, scale, 0.0f, hinge));

    if (top) g.setGradientFill (ColourGradient (ff::cardHi, 0, card.getY(), ff::card, 0, hinge, false));
    else     g.setGradientFill (ColourGradient (ff::card, 0, hinge, ff::cardLow, 0, card.getBottom(), false));
    g.fillRoundedRectangle (card, 7.0f);
    g.setColour (ff::ink);
    g.setFont (ff::font (textHeight, 0.02f));
    g.drawFittedText (ff::utf8 (text), card.reduced (6.0f, 0.0f).toNearestInt(), Justification::centred, 1, 0.7f);

    if (shade)
    {
        g.setColour (Colours::black.withAlpha (0.45f * (1.0f - scale)));
        g.fillRoundedRectangle (card, 7.0f);
    }
}

void FlipCard::paint (Graphics& g)
{
    auto outer = getLocalBounds().toFloat();
    g.setColour (ff::housing);
    g.fillRoundedRectangle (outer, 10.0f);
    auto card = outer.reduced (6.0f);

    if (progress >= 1.0f)
    {
        drawHalf (g, card, current, true, 1.0f, false);
        drawHalf (g, card, current, false, 1.0f, false);
    }
    else
    {
        drawHalf (g, card, current, true, 1.0f, false);
        drawHalf (g, card, previous, false, 1.0f, false);
        const float eased = 1.0f - std::pow (1.0f - progress, 2.0f);
        if (eased < 0.5f) drawHalf (g, card, previous, true, 1.0f - eased * 2.0f, true);
        else              drawHalf (g, card, current, false, (eased - 0.5f) * 2.0f, true);
    }

    g.setColour (Colours::black);
    g.fillRect (card.getX(), card.getCentreY() - 1.5f, card.getWidth(), 3.0f);
    g.setColour (ff::pin);
    g.fillRoundedRectangle (outer.getX() + 1.0f, card.getCentreY() - 6.0f, 3.0f, 12.0f, 1.0f);
    g.fillRoundedRectangle (outer.getRight() - 4.0f, card.getCentreY() - 6.0f, 3.0f, 12.0f, 1.0f);
}

// =====================================================================================
// Pills
// =====================================================================================
Pills::Pills (AudioProcessorValueTreeState& s, const String& paramId, StringArray l, float size, float g)
    : labels (std::move (l)), textSize (size), gap (g)
{
    setMouseCursor (MouseCursor::PointingHandCursor);
    attachment = std::make_unique<ParameterAttachment> (*s.getParameter (paramId), [this] (float v)
    {
        selected = (int) std::lround (v);
        repaint();
    });
    attachment->sendInitialUpdate();
}

Rectangle<float> Pills::cell (int i) const
{
    const float w = ((float) getWidth() - gap * (float) (labels.size() - 1)) / (float) labels.size();
    return { (float) i * (w + gap), 0.0f, w, (float) getHeight() };
}

void Pills::paint (Graphics& g)
{
    for (int i = 0; i < labels.size(); ++i)
        pill (g, cell (i).reduced (1.0f), labels[i], i == selected, textSize);
}

void Pills::mouseDown (const MouseEvent& e)
{
    for (int i = 0; i < labels.size(); ++i)
        if (cell (i).contains (e.position)) { attachment->setValueAsCompleteGesture ((float) i); return; }
}

ActionPills::ActionPills (StringArray l, float size, float g) : labels (std::move (l)), textSize (size), gap (g)
{
    setMouseCursor (MouseCursor::PointingHandCursor);
}

Rectangle<float> ActionPills::cell (int i) const
{
    const float w = ((float) getWidth() - gap * (float) (labels.size() - 1)) / (float) labels.size();
    return { (float) i * (w + gap), 0.0f, w, (float) getHeight() };
}

void ActionPills::paint (Graphics& g)
{
    const int sel = selected ? selected() : -1;
    for (int i = 0; i < labels.size(); ++i)
        pill (g, cell (i).reduced (1.0f), labels[i], i == sel, textSize);
}

void ActionPills::mouseDown (const MouseEvent& e)
{
    for (int i = 0; i < labels.size(); ++i)
        if (cell (i).contains (e.position)) { if (onClick) onClick (i); repaint(); return; }
}

// =====================================================================================
// Look and feel: clock-face knobs
// =====================================================================================
void FlipLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float start, float end, Slider&)
{
    const float size = (float) jmin (w, h);
    const Point<float> c ((float) x + (float) w * 0.5f, (float) y + (float) h * 0.5f);
    const float r = size * 0.5f - 1.0f;

    g.setColour (ff::ring);
    g.fillEllipse (c.x - r, c.y - r, r * 2, r * 2);
    g.setColour (ff::bg);
    g.fillEllipse (c.x - r * 0.9f, c.y - r * 0.9f, r * 1.8f, r * 1.8f);

    g.setColour (ff::ink);
    for (int i = 0; i <= 10; ++i)
    {
        const float a = start + (end - start) * (float) i / 10.0f;
        const Point<float> p1 (c.x + std::sin (a) * r * 0.80f, c.y - std::cos (a) * r * 0.80f);
        const Point<float> p2 (c.x + std::sin (a) * r * 0.92f, c.y - std::cos (a) * r * 0.92f);
        g.drawLine ({ p1, p2 }, 2.0f);
    }
    const float a = start + pos * (end - start);
    Path hand;
    hand.addRoundedRectangle (-3.0f, -r * 0.68f, 6.0f, r * 0.68f + r * 0.15f, 2.0f);
    g.fillPath (hand, AffineTransform::rotation (a).translated (c));
    g.fillEllipse (c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f);
    g.setColour (ff::bg);
    g.fillEllipse (c.x - 1.8f, c.y - 1.8f, 3.6f, 3.6f);
}

void FlipLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float, float, Slider::SliderStyle, Slider& s)
{
    const float cy = (float) y + (float) h * 0.5f;
    const float left = (float) x + 1.0f, right = (float) (x + w) - 1.0f;
    g.setColour (ff::ring);
    g.fillRoundedRectangle (left, cy - 5.0f, right - left, 10.0f, 5.0f);
    g.setColour (s.findColour (Slider::trackColourId));
    g.fillRoundedRectangle (left, cy - 5.0f, jmax (10.0f, pos - left), 10.0f, 5.0f);
}

Dial::Dial (AudioProcessorValueTreeState& s, const String& paramId, const String& c, int size)
    : caption (c), knobSize (size)
{
    param = s.getParameter (paramId);
    slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
    slider.setMouseCursor (MouseCursor::PointingHandCursor);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);
    attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (s, paramId, slider);
    slider.setDoubleClickReturnValue (true, (double) param->convertFrom0to1 (param->getDefaultValue()));
}

void Dial::resized()
{
    slider.setBounds (getLocalBounds().removeFromTop (18 + knobSize + 6).withTrimmedTop (21).withSizeKeepingCentre (knobSize, knobSize));
}

void Dial::paint (Graphics& g)
{
    auto r = getLocalBounds();
    ff::label (g, caption.toUpperCase(), r.removeFromTop (18), Justification::centred, ff::dim, 12.0f);
    r.removeFromTop (knobSize + 6);
    ff::label (g, param->getCurrentValueAsText().toUpperCase(), r.removeFromTop (20), Justification::centred, ff::ink, 15.0f);
}

BarSlider::BarSlider (AudioProcessorValueTreeState& s, const String& paramId, Colour fill)
{
    slider.setSliderStyle (Slider::LinearHorizontal);
    slider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    slider.setColour (Slider::trackColourId, fill);
    slider.setMouseCursor (MouseCursor::PointingHandCursor);
    addAndMakeVisible (slider);
    attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (s, paramId, slider);
}

// =====================================================================================
// Steps: the rotary switches
// =====================================================================================
Steps::Steps (BitrotProcessor& p, const String& paramId, Style s, Colour c) : proc (p), style (s), colour (c)
{
    param = p.apvts.getParameter (paramId);
    setMouseCursor (MouseCursor::PointingHandCursor);
}

Rectangle<float> Steps::cell (int i) const
{
    const float W = (float) getWidth(), H = (float) getHeight();
    if (style == Dots)
    {
        const float d = jmin (26.0f, H), gap = (W - d * 8.0f) / 7.0f;
        return { (float) i * (d + gap), (H - d) * 0.5f, d, d };
    }
    const float w = 28.0f, gap = (W - w * 8.0f) / 7.0f;
    const float h = 22.0f + (float) i * (H - 22.0f) / 7.0f;
    return { (float) i * (w + gap), H - h, w, h };
}

void Steps::paint (Graphics& g)
{
    const int sel = roundToInt (param->convertFrom0to1 (param->getValue()));
    const bool up = style == Ladder && proc.effectiveOctUp();
    for (int i = 0; i < 8; ++i)
    {
        auto r = cell (i).reduced (1.0f);
        const bool on = i == sel;
        if (style == Dots)
        {
            g.setColour (on ? colour : ff::housing);
            g.fillEllipse (r);
            g.setColour (on ? colour : ff::line);
            g.drawEllipse (r.reduced (1.0f), 2.0f);
        }
        else
        {
            g.setColour (on ? colour : ff::card);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (on ? colour : Colour (0xff2a2a2a));
            g.drawRoundedRectangle (r.reduced (1.0f), 5.0f, 2.0f);
            if (on && up)
            {
                // octave up: a coral cap shows the doubled harmonic
                g.setColour (ff::coral);
                g.fillRoundedRectangle (r.withHeight (8.0f).translated (0.0f, -12.0f), 3.0f);
            }
        }
    }
}

void Steps::mouseDown (const MouseEvent& e)
{
    for (int i = 0; i < 8; ++i)
        if (cell (i).expanded (3.0f).contains (e.position) || (style == Ladder && e.position.x >= cell (i).getX() - 3.0f && e.position.x <= cell (i).getRight() + 3.0f))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 ((float) i));
            param->endChangeGesture();
            repaint();
            return;
        }
}

// =====================================================================================
// Voice scope
// =====================================================================================
void VoiceScope::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (ff::housing);
    g.fillRoundedRectangle (b, 10.0f);
    auto& eng = proc.getEngine();
    const bool bypassed = proc.effectiveBypass();
    const float lock = eng.uiLock.load();
    const bool sounding = ! bypassed && (lock >= 0.0f || eng.uiFrozen.load());
    const float glitch = sounding && ! eng.uiFrozen.load() ? jlimit (0.0f, 1.0f, (0.62f - jmax (0.0f, lock)) / 0.5f) : 0.0f;
    const float note = eng.uiNote.load();
    const float fIn = note > 0.0f ? 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f) : 110.0f;
    const float fOsc = jmax (1.0f, eng.uiOscHz.load()), fSub = jmax (0.5f, eng.uiSubHz.load());
    const float P = 48.0f;
    const float W = b.getWidth(), laneH = b.getHeight() / 3.0f;
    auto& s = proc.apvts;
    const float lv[3] = { s.getRawParameterValue (ParamIDs::squareLevel)->load(), s.getRawParameterValue (ParamIDs::subLevel)->load(),
                          s.getRawParameterValue (ParamIDs::oscLevel)->load() };
    const float periods[3] = { P, jmin (2000.0f, P * fIn / fSub), jmax (3.0f, P * fIn / fOsc) };
    const Colour cols[3] = { ff::amber, ff::coral, ff::teal };
    static const char* names3[3] = { "SQUARE", "SUB", "OSC" };
    for (int k = 0; k < 3; ++k)
    {
        const float yc = laneH * ((float) k + 0.55f), a = laneH * 0.28f;
        Path p;
        if (! sounding) { p.startNewSubPath (0.0f, yc); p.lineTo (W, yc); }
        else
        {
            float x = 0.0f; bool high = true; int guard = 0;
            const float jitter = k == 0 ? glitch * 0.5f : glitch;
            p.startNewSubPath (0.0f, yc - a);
            while (x < W && ++guard < 500)
            {
                float half = periods[k] * 0.5f * (1.0f + (random.nextFloat() - 0.5f) * 1.4f * jitter);
                half = jmax (1.5f, half);
                const float nx = jmin (W, x + half);
                const bool drop = jitter > 0.5f && random.nextFloat() < 0.12f * jitter;
                const float y = drop ? yc : (high ? yc - a : yc + a);
                p.lineTo (x, y); p.lineTo (nx, y);
                high = ! high; x = nx;
            }
        }
        g.setColour (cols[k].withAlpha (sounding ? 0.35f + 0.65f * lv[k] : 0.3f));
        g.strokePath (p, PathStrokeType (2.0f));
        ff::label (g, names3[k], { 8, (int) (laneH * (float) k) + 1, 60, 12 }, Justification::centredLeft, cols[k], 10.0f);
    }
}

// =====================================================================================
// Frequency modulator curve
// =====================================================================================
void FmCurve::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (ff::housing);
    g.fillRoundedRectangle (b, 10.0f);
    const float W = b.getWidth(), H = b.getHeight();
    const bool vib = choiceOf (proc, ParamIDs::fmMode, 1) == 1;
    const float rate = proc.apvts.getRawParameterValue (ParamIDs::rate)->load();
    Path p;
    if (vib)
    {
        const float cyc = 2.0f + rate * 12.0f;
        const float depth = proc.apvts.getRawParameterValue (ParamIDs::depth)->load();
        const float a = (H * 0.15f) + depth * H * 0.25f;
        for (float x = 0.0f; x <= W; x += 2.0f)
        {
            const float ph = std::fmod (x / W * cyc, 1.0f);
            const float tri = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
            const float y = H * 0.5f - tri * a;
            if (x == 0.0f) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
    }
    else
    {
        // five notes over 1.25 s; the slide between them is a straight line at the glide speed,
        // so at slow Rates it runs into the next note before it arrives (full height = 2 octaves)
        const float lv[5] = { 0.72f, 0.3f, 0.55f, 0.2f, 0.64f };
        const float seg = W / 5.0f;
        const float step = bit::BitrotEngine::glideSlew (rate) * (1.25f / W) * 0.5f;   // height per pixel
        float y = lv[0];
        p.startNewSubPath (0.0f, H * y);
        for (float x = 1.0f; x <= W; x += 1.0f)
        {
            const float target = lv[jlimit (0, 4, (int) (x / seg))];
            y += jlimit (-step, step, target - y);
            p.lineTo (x, H * y);
        }
    }
    g.setColour (ff::teal);
    g.strokePath (p, PathStrokeType (2.5f, PathStrokeType::curved, PathStrokeType::rounded));
}

// =====================================================================================
// Tracking strip
// =====================================================================================
TrackingStrip::TrackingStrip (BitrotProcessor& p) : proc (p)
{
    note.setTextHeight (30.0f);
    note.setValue (String::fromUTF8 ("—"));
    addAndMakeVisible (note);
}

void TrackingStrip::resized() { note.setBounds (0, 20, 92, 60); }

void TrackingStrip::tick (double now, double dt)
{
    auto& eng = proc.getEngine();
    note.advance (dt);
    const float l = eng.uiLock.load();
    const float n = eng.uiNote.load();
    String newHz = hzText, newWord = word;
    Colour wc = wordColour;

    if (proc.effectiveBypass())
    {
        newWord = "BYPASSED"; wc = ff::faint; newHz = "BITROT IS OFF";
        note.setValue (String::fromUTF8 ("·"));
        lock = 0.0f;
    }
    else if (eng.uiFrozen.load())
    {
        newWord = "FROZEN"; wc = ff::frost; lock = 1.0f;
        newHz = "OSC " + hz (eng.uiOscHz.load()) + String::fromUTF8 (" · HELD");
    }
    else if (l >= 0.0f && n > 0.0f)
    {
        lastHeard = now;
        lock = l;
        const float glitch = jlimit (0.0f, 1.0f, (0.62f - l) / 0.5f);
        if (glitch > 0.55f)
        {
            static const char* g[7] = { "E?", "#1", "--", "??", "G#0", "A1?", "B?" };
            note.setValue (g[random.nextInt (7)]);
            newHz = String (20.0f + random.nextFloat() * 400.0f, 1) + " HZ · LOOP HUNTING";
        }
        else
        {
            note.setValue (noteName (n));
            const float c = (n - (float) std::lround (n)) * 100.0f;
            newHz = hz (440.0f * std::pow (2.0f, (n - 69.0f) / 12.0f)) + String::fromUTF8 (" · ") + (c >= 0.0f ? "+" : "") + String (roundToInt (c)) + " CENTS";
        }
        if (l > 0.8f) { newWord = "LOCKED"; wc = ff::teal; }
        else if (l > 0.35f) { newWord = "HUNTING"; wc = ff::amber; }
        else { newWord = "LOST"; wc = ff::coral; }
    }
    else if (now - lastHeard > 0.6)
    {
        newWord = "SILENT"; wc = ff::faint; newHz = "WAITING FOR A NOTE"; lock = 0.0f;
        note.setValue (String::fromUTF8 ("—"));
    }
    if (newHz != hzText || newWord != word || wc != wordColour) { hzText = newHz; word = newWord; wordColour = wc; }
    repaint();
}

void TrackingStrip::paint (Graphics& g)
{
    static const char* inputs[3] = { "GUITAR", "BASS", "SYNTH" };
    ff::label (g, String ("TRACKING · ") + inputs[choiceOf (proc, ParamIDs::input, 2)], { 0, 0, 250, 16 });
    ff::label (g, hzText, { 102, 28, 160, 16 }, Justification::centredLeft, ff::ink, 12.0f);
    Rectangle<float> bar (102.0f, 52.0f, 86.0f, 8.0f);
    g.setColour (ff::ring);
    g.fillRoundedRectangle (bar, 4.0f);
    if (lock > 0.01f)
    {
        g.setColour (wordColour);
        g.fillRoundedRectangle (bar.withWidth (jmax (8.0f, bar.getWidth() * lock)), 4.0f);
    }
    ff::label (g, word, { 192, 48, 70, 16 }, Justification::centredRight, wordColour, 11.0f);
    ff::label (g, "LOCK", { 102, 64, 160, 14 }, Justification::centredLeft, ff::hint, 11.0f);
}

// =====================================================================================
// Buttons
// =====================================================================================
void FootButton::paint (Graphics& g)
{
    static const char* letters[4] = { "A", "B", "C", "D" };
    bool lit = false;
    if (index == 0) lit = ! proc.effectiveBypass();
    if (index == 2) lit = proc.effectiveFreeze();
    if (index == 3) lit = proc.effectiveOctUp();
    const bool down = (proc.uiFootDown.load() >> index) & 1;

    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (ff::housing);
    g.fillRoundedRectangle (r, 12.0f);
    g.setColour (down ? ff::coral : ff::line);
    g.drawRoundedRectangle (r.reduced (1.0f), 12.0f, 2.0f);
    const Point<float> led (r.getX() + 13.0f, r.getY() + 17.0f);
    if (lit) { g.setColour (ff::coral.withAlpha (0.22f)); g.fillEllipse (led.x - 9.0f, led.y - 9.0f, 18.0f, 18.0f); }
    g.setColour (lit ? ff::coral : ff::pin);
    g.fillEllipse (led.x - 5.0f, led.y - 5.0f, 10.0f, 10.0f);
    g.setColour (ff::ink);
    g.setFont (ff::font (19.0f, 0.06f));
    g.drawText (letters[index], Rectangle<float> (r.getX() + 24.0f, r.getY() + 5.0f, 30.0f, 24.0f), Justification::centredLeft, false);
    ff::label (g, footTaps[index], { (int) r.getX() + 7, (int) r.getY() + 30, (int) r.getWidth() - 10, 12 }, Justification::centredLeft, ff::dim, 10.0f);
    ff::label (g, footHolds[index], { (int) r.getX() + 7, (int) r.getY() + 44, (int) r.getWidth() - 10, 12 }, Justification::centredLeft, ff::hint, 10.0f);
}

void BypassButton::paint (Graphics& g)
{
    const bool bypassed = proc.effectiveBypass();
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (ff::housing);
    g.fillRoundedRectangle (r, 12.0f);
    g.setColour (bypassed ? ff::line : ff::ink);
    g.drawRoundedRectangle (r.reduced (1.0f), 12.0f, 2.0f);
    const Point<float> p (r.getX() + 24.0f, r.getCentreY());
    if (! bypassed) { g.setColour (ff::red.withAlpha (0.22f)); g.fillEllipse (p.x - 12.0f, p.y - 12.0f, 24.0f, 24.0f); }
    g.setColour (bypassed ? ff::pin : ff::red);
    g.fillEllipse (p.x - 7.0f, p.y - 7.0f, 14.0f, 14.0f);
    auto text = r.withTrimmedLeft (46.0f);
    g.setColour (ff::ink);
    g.setFont (ff::font (19.0f, 0.08f));
    g.drawText (bypassed ? "BYPASSED" : "BITROT ON", text.removeFromTop (r.getHeight() * 0.55f).withTrimmedTop (6.0f), Justification::bottomLeft, false);
    ff::label (g, "TAP TO BYPASS", text.toNearestInt(), Justification::topLeft, ff::dim, 11.0f);
}

void BypassButton::mouseDown (const MouseEvent&)
{
    auto* p = proc.apvts.getParameter (ParamIDs::bypass);
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
    p->endChangeGesture();
    repaint();
}

// =====================================================================================
// Presets
// =====================================================================================
Rectangle<float> PresetBanks::cell (int i) const
{
    return { (float) (i % 8) * 34.0f, (float) (i / 8) * 26.0f, 30.0f, 22.0f };
}

void PresetBanks::paint (Graphics& g)
{
    static const Colour bankCols[4] = { ff::bankR, ff::bankY, ff::bankG, ff::bankB };
    const int sel = proc.selectedPreset();
    for (int i = 0; i < BitrotProcessor::kPresets; ++i)
    {
        auto c = cell (i);
        const Colour col = bankCols[i / BitrotProcessor::kPerBank];
        const bool flash = i == flashSlot && nowSeconds() < flashUntil;
        const bool full = proc.hasPreset (i);
        if (i == sel || flash) { g.setColour (flash ? ff::ink : col); g.fillRoundedRectangle (c, 5.0f); }
        else { g.setColour (col.withAlpha (full ? 1.0f : 0.4f)); g.drawRoundedRectangle (c.reduced (1.0f), 5.0f, 2.0f); }
        g.setColour (i == sel || flash ? ff::bg : (full ? ff::ink : ff::faint));
        g.setFont (ff::font (11.0f, 0.04f));
        g.drawText (String (i + 1), c, Justification::centred, false);
    }
}

void PresetBanks::mouseDown (const MouseEvent& e)
{
    pressed = -1;
    for (int i = 0; i < BitrotProcessor::kPresets; ++i)
        if (cell (i).expanded (2.0f).contains (e.position)) pressed = i;
    downAt = nowSeconds();
    saved = false;
    startTimer (40);
}

void PresetBanks::timerCallback()
{
    if (pressed >= 0 && ! saved && nowSeconds() - downAt > 0.7)
    {
        proc.savePreset (pressed);
        saved = true;
        flashSlot = pressed;
        flashUntil = nowSeconds() + 0.9;
    }
    if (pressed < 0 && nowSeconds() > flashUntil) stopTimer();
    repaint();
}

void PresetBanks::mouseUp (const MouseEvent&)
{
    if (pressed >= 0 && ! saved) proc.loadPreset (pressed);
    pressed = -1;
    repaint();
}

// =====================================================================================
// Expression bar
// =====================================================================================
void ExpressionBar::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (ff::housing);
    g.fillRoundedRectangle (r, 10.0f);
    const float v = proc.expressionLevel();
    g.setColour (ff::blob.withAlpha (0.35f));
    g.fillRoundedRectangle (r.withWidth (jmax (0.0f, r.getWidth() * v)), 10.0f);
    g.setColour (ff::line);
    g.drawRoundedRectangle (r.reduced (1.0f), 10.0f, 2.0f);
    const int t = choiceOf (proc, ParamIDs::expTarget, (int) names::expTargets().size() - 1);
    const String target = String (names::expTargets()[(size_t) t].label).toUpperCase();
    ff::label (g, target, r.reduced (12.0f, 0.0f).toNearestInt(), Justification::centredLeft, ff::ink, 13.0f);
    const String pos = proc.isCalibrating() ? String ("ROCK HEEL TO TOE") : (v < 0.02f ? String ("HEEL") : (v > 0.98f ? String ("TOE") : String (roundToInt (v * 100.0f)) + "%"));
    ff::label (g, pos, r.reduced (12.0f, 0.0f).toNearestInt(), Justification::centredRight, ff::ink, 13.0f);
}

void ExpressionBar::mouseDown (const MouseEvent&)
{
    PopupMenu m;
    const int cur = choiceOf (proc, ParamIDs::expTarget, (int) names::expTargets().size() - 1);
    const auto& targets = names::expTargets();
    m.addSectionHeader ("The expression pedal sweeps");
    for (int i = 0; i < (int) targets.size(); ++i) m.addItem (100 + i, targets[(size_t) i].label, true, i == cur);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (this), [this] (int r)
    {
        if (r >= 100)
            if (auto* p = proc.apvts.getParameter (ParamIDs::expTarget))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->convertTo0to1 ((float) (r - 100)));
                p->endChangeGesture();
            }
    });
}

// =====================================================================================
// Pebble
// =====================================================================================
void Pebble::update (double t, double dt)
{
    now = t;
    auto& eng = proc.getEngine();
    const bool bypassed = proc.effectiveBypass();
    frozen = eng.uiFrozen.load() && ! bypassed;
    const float lock = eng.uiLock.load();
    const bool sounding = lock >= 0.0f || frozen;
    if (sounding) lastSound = t;
    const bool expMoving = t - proc.lastExpressionMove() < 2.0;
    if (expMoving) lastSound = t;
    asleep = bypassed || t - lastSound > 6.0;

    const int onsets = eng.uiOnsets.load();
    if (onsets != lastOnsets) { lastOnsets = onsets; if (! frozen) hop = 1.0f; }
    const int fp = proc.uiFootPulse.load();
    if (fp != lastFoot) { lastFoot = fp; foot = 1.0f; footSide = jmax (0, proc.lastFootSwitch()) % 2; }
    hop *= (float) std::exp (-dt / 0.12);
    foot *= (float) std::exp (-dt / 0.1);

    const float glitchTarget = asleep || frozen || lock < 0.0f ? 0.0f : jlimit (0.0f, 1.0f, (0.62f - lock) / 0.5f);
    glitch += (glitchTarget - glitch) * (float) (1.0 - std::exp (-dt / 0.05));
    stretch += ((proc.effectiveOctUp() && ! asleep ? 1.0f : 0.0f) - stretch) * (float) (1.0 - std::exp (-dt / 0.08));
    const float leanTarget = expMoving ? (proc.expressionLevel() - 0.5f) * 0.35f : 0.0f;
    lean += (leanTarget - lean) * (float) (1.0 - std::exp (-dt / 0.1));

    if (t > nextBlink)
    {
        blinkUntil = t + 0.13;
        nextBlink = t + 2.5 + random.nextDouble() * 3.0;
    }
    blinking = t < blinkUntil;

    // stutter: when the loop has lost hold, sometimes repeat the last frame for a few ticks
    if (stutter > 0) { --stutter; return; }
    if (glitch > 0.55f && random.nextFloat() < 0.2f) stutter = 2 + random.nextInt (4);

    const float gl = glitch;
    jx = gl > 0.1f ? (random.nextFloat() - 0.5f) * 7.0f * gl : 0.0f;
    jy = gl > 0.1f ? (random.nextFloat() - 0.5f) * 4.0f * gl : 0.0f;
    splitX = gl > 0.15f ? (4.0f + 6.0f * random.nextFloat()) * gl * (random.nextBool() ? 1.0f : -1.0f) : 0.0f;
    for (auto& tr : tears)
    {
        tr.on = gl > 0.2f && random.nextFloat() < gl * 0.8f;
        tr.y = 58.0f + random.nextFloat() * 86.0f;
        tr.h = 3.0f + random.nextFloat() * 7.0f * gl;
        tr.shift = (8.0f + random.nextFloat() * 26.0f) * gl * (random.nextBool() ? 1.0f : -1.0f);
        tr.colour = random.nextFloat() < 0.25f ? 1 : (random.nextFloat() < 0.15f ? 2 : 0);
    }
    for (size_t i = 0; i < pixels.size(); ++i)
    {
        pixels[i] = { 30.0f + random.nextFloat() * 140.0f, 40.0f + random.nextFloat() * 110.0f };
        pixelOn[i] = gl > 0.45f && random.nextFloat() < gl;
    }
    squareEyes = gl > 0.35f;
    bodyTeal = gl > 0.6f && random.nextFloat() < 0.12f;
    look += (float) ((gl > 0.4f ? (random.nextFloat() - 0.5f) * 2.0f : std::sin (t * 0.7) * 0.5) - look) * 0.3f;
    repaint();
}

void Pebble::paint (Graphics& g)
{
    auto a = getLocalBounds().toFloat();
    const float s = a.getWidth() / 150.0f;
    const bool still = frozen || asleep;
    const float bob = still ? 0.0f : 1.5f * (float) std::sin (now * 2.4);
    const float breathe = asleep ? 1.0f + 0.025f * (float) std::sin (now * 1.6) : 1.0f;
    const float dy = bob - hop * 10.0f + jy;
    const float sy = 1.0f + 0.14f * stretch;
    const auto tf = AffineTransform::translation (-100.0f, -150.0f).scaled (1.0f, sy).translated (0.0f, 50.0f)
                        .translated (0.0f, 0.0f)
                        .scaled (s * breathe, s / breathe)
                        .rotated (lean).translated (a.getCentreX() + jx * s, a.getCentreY() + 2.0f);
    auto ell = [&] (float cx, float cy, float rx, float ry, Colour col)
    {
        Path p; p.addEllipse (cx - rx, cy - ry, rx * 2, ry * 2);
        g.setColour (col); g.fillPath (p, tf);
    };
    auto rect = [&] (float x, float y, float w, float h, Colour col)
    {
        Path p; p.addRectangle (x, y, w, h);
        g.setColour (col); g.fillPath (p, tf);
    };
    auto stroke = [&] (Path& p, float w, Colour col = ff::faceInk)
    {
        g.setColour (col);
        g.strokePath (p, PathStrokeType (w, PathStrokeType::curved, PathStrokeType::rounded), tf);
    };

    // feet: one taps on a footswitch press (drawn without the stretch)
    {
        const auto ft = AffineTransform::translation (-100.0f, -100.0f).scaled (s, s).rotated (lean).translated (a.getCentreX(), a.getCentreY() + 2.0f);
        const float lift = foot * 9.0f;
        Path f1; f1.addEllipse (78.0f - 13.0f, 150.0f - (footSide == 0 ? lift : 0.0f) - 6.0f, 26.0f, 12.0f);
        Path f2; f2.addEllipse (122.0f - 13.0f, 150.0f - (footSide == 1 ? lift : 0.0f) - 6.0f, 26.0f, 12.0f);
        g.setColour (ff::blobShade); g.fillPath (f1, ft); g.fillPath (f2, ft);
    }

    // glow: teal when locked, frost when frozen, flickering coral and teal when lost
    if (! asleep)
    {
        Colour c = frozen ? ff::frost : (glitch > 0.3f ? (random.nextBool() ? ff::coral : ff::teal) : ff::teal);
        const float alpha = frozen ? 0.7f : (glitch > 0.3f ? 0.5f * random.nextFloat() : 0.2f + 0.4f * hop);
        Path ringPath; ringPath.addEllipse (100 - 66, 100 - 56 + dy, 132, 112);
        stroke (ringPath, 5.0f, c.withAlpha (alpha));
    }

    // colour split: red and cyan ghosts either side
    if (glitch > 0.15f)
    {
        ell (100 + splitX, 100 + dy, 58, 48, Colour (0xffff2d6f).withAlpha (0.55f * glitch));
        ell (100 - splitX, 100 + dy, 58, 48, Colour (0xff22e6ff).withAlpha (0.55f * glitch));
    }
    ell (107, 109 + dy, 58, 48, ff::blobShade);
    ell (100, 100 + dy, 58, 48, bodyTeal ? ff::teal : ff::blob);
    if (frozen) ell (100, 100 + dy, 58, 48, ff::frost.withAlpha (0.16f));

    // torn rows: a gap and a shifted strip
    for (const auto& tr : tears)
    {
        if (! tr.on) continue;
        const float rel = jmin (1.0f, std::abs (tr.y - 100.0f) / 48.0f);
        const float w = 116.0f * std::sqrt (jmax (0.05f, 1.0f - rel * rel));
        const Colour c = tr.colour == 1 ? ff::teal : (tr.colour == 2 ? ff::coral : ff::blob);
        rect (100.0f - w * 0.5f, tr.y + dy, w, tr.h, ff::bg);
        rect (100.0f - w * 0.5f + tr.shift, tr.y + dy, w, tr.h, c);
    }

    const float eyeY = 92.0f + dy;
    if (asleep || blinking)
    {
        for (float ex : { 80.0f, 120.0f })
        {
            Path lid; lid.startNewSubPath (ex - 12, eyeY + 2); lid.quadraticTo (ex, eyeY + 10, ex + 12, eyeY + 2);
            stroke (lid, 6.0f);
        }
    }
    else if (squareEyes)
    {
        // pixel eyes
        for (float ex : { 80.0f, 120.0f })
        {
            const float off = random.nextFloat() < 0.3f ? 4.0f : 0.0f;
            rect (ex - 12.0f, eyeY - 12.0f + off, 24.0f, 24.0f, ff::eye);
            rect (ex - 5.0f + look * 5.0f, eyeY - 3.0f + off, 10.0f, 10.0f, ff::faceInk);
        }
    }
    else
    {
        int k = 0;
        for (float ex : { 80.0f, 120.0f })
        {
            const float r = glitch > 0.2f ? (k++ ? 16.0f : 13.0f) : 14.0f;
            ell (ex, eyeY, r, r, ff::eye);
            ell (ex + look * 5.0f, eyeY + 2.0f, 7.0f, 7.0f, ff::faceInk);
        }
    }

    if (asleep)
    {
        Path m; m.startNewSubPath (94, 124 + dy); m.lineTo (106, 124 + dy);
        stroke (m, 6.0f);
    }
    else if (glitch > 0.5f)
    {
        Path m; m.startNewSubPath (86, 124 + dy);
        for (int i = 1; i <= 6; ++i) m.lineTo (86.0f + (float) i * 5.0f, 124.0f + dy + (i % 2 ? -4.0f : 4.0f));
        stroke (m, 5.0f);
    }
    else if (frozen || glitch > 0.15f) ell (100, 124 + dy, 6.0f, 7.0f, ff::faceInk);
    else
    {
        Path m; m.startNewSubPath (90, 120 + dy); m.quadraticTo (100, 130 + dy, 110, 120 + dy);
        stroke (m, 6.0f);
    }

    if (frozen)
    {
        Path ring; ring.addEllipse (100 - 61, 100 - 51 + dy, 122, 102);
        PathStrokeType st (3.0f);
        Path dashed;
        const float dashes[2] = { 6.0f, 5.0f };
        st.createDashedStroke (dashed, ring, dashes, 2);
        g.setColour (ff::frost);
        g.fillPath (dashed, tf);
    }

    // stray pixels
    {
        const auto pt = AffineTransform::translation (-100.0f, -100.0f).scaled (s, s).translated (a.getCentreX(), a.getCentreY() + 2.0f);
        const Colour cols[5] = { ff::teal, ff::coral, ff::ink, ff::blob, ff::amber };
        for (size_t i = 0; i < pixels.size(); ++i)
            if (pixelOn[i])
            {
                Path p; p.addRectangle (pixels[i].x, pixels[i].y, 6.0f, 6.0f);
                g.setColour (cols[i]); g.fillPath (p, pt);
            }
    }

    if (asleep)
    {
        g.setFont (ff::font (15.0f, 0.0f));
        for (int k = 0; k < 3; ++k)
        {
            const float ph = (float) std::fmod (now * 0.5 + k / 3.0, 1.0);
            g.setColour (ff::ink.withAlpha (1.0f - ph));
            g.drawText ("z", Rectangle<float> (a.getRight() - 16.0f + ph * 8.0f, a.getY() + 8.0f - ph * 14.0f, 16.0f, 16.0f), Justification::centred, false);
        }
    }
}
} // namespace ui

// =====================================================================================
// Editor
// =====================================================================================
using namespace ui;

template <typename T, typename... Args>
T& BitrotEditor::make (int pg, Args&&... args)
{
    auto c = std::make_unique<T> (std::forward<Args> (args)...);
    T& ref = *c;
    addAndMakeVisible (ref);
    if (pg >= 0) pages[(size_t) pg].push_back (&ref);
    owned.push_back (std::move (c));
    return ref;
}

BitrotEditor::BitrotEditor (BitrotProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    auto& s = p.apvts;
    using namespace ParamIDs;

    // ---- header ----
    make<Pills> (-1, s, input, StringArray { "GUITAR", "BASS", "SYNTH" }, 13.0f, 2.0f).setBounds (430, 24, 230, 32);
    tabs = &make<ActionPills> (-1, StringArray { "MAIN", "DEEP", "PEDAL" }, 13.0f, 2.0f);
    tabs->setBounds (1060, 24, 180, 32);
    tabs->selected = [this] { return page; };
    tabs->onClick = [this] (int i) { showPage (i); };

    auto dial = [&] (int pg, const char* id, const char* caption, int x, int y, int w, int knob = 60) -> Dial&
    {
        auto& d = make<Dial> (pg, s, id, caption, knob);
        d.setBounds (x, y, w, 18 + knob + 6 + 20);
        return d;
    };

    // ---- MAIN: voice mixer ----
    dial (0, squareLevel, "Square", 32, 110, 92);
    dial (0, subLevel, "Subharmonic", 124, 110, 92);
    dial (0, oscLevel, "Oscillator", 216, 110, 92);
    dial (0, dry, "Dry", 32, 222, 92);
    dial (0, level, "Level", 124, 222, 92);
    scope = &make<VoiceScope> (0, p);
    scope->setBounds (36, 338, 268, 118);

    // ---- MAIN: subharmonic ----
    subCard = &make<FlipCard> (0);
    subCard->setTextHeight (26.0f);
    subCard->setBounds (356, 116, 258, 72);
    subCard->onClick = [this] { proc.stepSub (1); };
    subSteps = &make<Steps> (0, p, subInterval, Steps::Dots, ff::coral);
    subSteps->setBounds (356, 198, 258, 28);
    make<Pills> (0, s, subRoot, StringArray { "UNISON", "OSCILLATOR" }, 12.0f, 2.0f).setBounds (356, 272, 258, 30);
    make<Pills> (0, s, ParamIDs::floor, StringArray { "OFF", String::fromUTF8 ("ON · FLOOR") }, 12.0f, 2.0f).setBounds (414, 356, 200, 30);

    // ---- MAIN: frequency modulator ----
    make<Pills> (0, s, fmMode, StringArray { "GLIDE", "VIBRATO" }, 13.0f, 2.0f).setBounds (666, 116, 238, 32);
    dial (0, rate, "Rate", 666, 158, 238, 96);
    fmCurve = &make<FmCurve> (0, p);
    fmCurve->setBounds (666, 306, 238, 70);
    make<BarSlider> (0, s, corrupt, ff::coral).setBounds (666, 416, 238, 16);

    // ---- MAIN: master oscillator ----
    oscCard = &make<FlipCard> (0);
    oscCard->setTextHeight (26.0f);
    oscCard->setBounds (956, 116, 268, 72);
    oscCard->onClick = [this] { proc.stepOsc (1); };
    oscSteps = &make<Steps> (0, p, oscInterval, Steps::Ladder, ff::teal);
    oscSteps->setBounds (956, 200, 268, 80);
    make<Pills> (0, s, oscRoot, StringArray { "UNISON", String::fromUTF8 ("−1"), String::fromUTF8 ("−2") }, 12.0f, 2.0f).setBounds (1010, 308, 214, 30);
    expBar = &make<ExpressionBar> (0, p);
    expBar->setBounds (956, 444, 268, 34);

    // ---- DEEP ----
    make<Pills> (1, s, trackComp, StringArray { "OFF", "ON" }, 12.0f, 2.0f).setBounds (36, 140, 268, 30);
    dial (1, gate, "Gate sensitivity", 36, 210, 268, 76);
    make<BarSlider> (1, s, corrupt, ff::coral).setBounds (36, 420, 268, 16);
    make<Pills> (1, s, floorHz, StringArray { "25 HZ", "30 HZ", "40 HZ" }, 12.0f, 2.0f).setBounds (356, 140, 258, 30);
    make<Pills> (1, s, subMix, StringArray { String::fromUTF8 ("SUM + DIFFERENCE"), "DIVIDED" }, 12.0f, 2.0f).setBounds (356, 270, 258, 30);
    dial (1, depth, "Vibrato depth", 666, 130, 238, 96);
    dial (1, bend, "Bend", 956, 130, 268, 96);

    // ---- PEDAL ----
    for (int i = 0; i < 4; ++i)
    {
        const int x = 36 + i * 300;
        auto& card = make<FlipCard> (2);
        card.setTextHeight (40.0f);
        card.setValue (String::charToString ((juce_wchar) ('A' + i)));
        card.setBounds (x, 116, 286, 76);
        pedalCards[i] = &card;
        auto& lp = make<ActionPills> (2, StringArray { "LEARN", "DEFAULTS" }, 12.0f, 4.0f);
        lp.setBounds (x, 274, 286, 28);
        lp.selected = [this, i] { return proc.learningWhat() == i ? 0 : -1; };
        lp.onClick = [this, i] (int k) { if (k == 0) proc.learnFootswitch (i); else proc.resetFootswitches(); };
    }
    expBarPedal = &make<ExpressionBar> (2, p);
    expBarPedal->setBounds (36, 430, 580, 40);
    auto& expActions = make<ActionPills> (2, StringArray { "CALIBRATE", "LEARN", "CURVE" }, 12.0f, 4.0f);
    expActions.setBounds (36, 478, 580, 28);
    expActions.selected = [this] { return proc.isCalibrating() ? 0 : (proc.learningWhat() == BitrotProcessor::LearnExpression ? 1 : -1); };
    expActions.onClick = [this, &expActions] (int k)
    {
        if (k == 0) proc.calibrateExpression();
        if (k == 1) proc.learnExpression();
        if (k == 2)
        {
            PopupMenu m;
            static const char* curves[3] = { "Linear", "Slow start", "Fast start" };
            for (int c = 0; c < 3; ++c) m.addItem (1 + c, curves[c], true, proc.pedals().curve == c);
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (&expActions), [this] (int r)
            {
                if (r > 0) { proc.pedals().curve = r - 1; proc.savePedals(); }
            });
        }
    };
    auto& keeperPills = make<ActionPills> (2, StringArray { "KEEP THIS MOMENT", "LIVE", "STUDIO", "EXPORT" }, 13.0f, 4.0f);
    keeperPills.setBounds (648, 430, 576, 40);
    keeperPills.selected = [this] { return proc.pedals().keeperLive ? 1 : 2; };
    keeperPills.onClick = [this] (int k)
    {
        if (k == 0) proc.keepNow();
        if (k == 1 || k == 2) proc.setKeeperLive (k == 1);
        if (k == 3) proc.exportKeepers();
    };

    // ---- bottom strip ----
    pebble = &make<Pebble> (-1, p);
    pebble->setBounds (26, 536, 108, 104);
    tracking = &make<TrackingStrip> (-1, p);
    tracking->setBounds (140, 548, 262, 84);
    banks = &make<PresetBanks> (-1, p);
    banks->setBounds (410, 570, 272, 48);
    auto& footArea = make<ClickArea> (-1);
    footArea.setBounds (700, 544, 340, 22);
    footArea.onClick = [this] { footswitchMenu(); };
    for (int i = 0; i < 4; ++i)
    {
        feet[i] = &make<FootButton> (-1, p, i);
        feet[i]->setBounds (700 + i * 86, 570, 82, 64);
    }
    bypass = &make<BypassButton> (-1, p);
    bypass->setBounds (1054, 556, 170, 72);

    showPage (0);
    setSize (1260, 660);
    lastTick = Time::getMillisecondCounterHiRes() * 0.001;
    startTimerHz (60);
}

BitrotEditor::~BitrotEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void BitrotEditor::showPage (int pg)
{
    page = pg;
    for (int i = 0; i < 3; ++i)
        for (auto* c : pages[(size_t) i]) c->setVisible (i == pg);
    repaint();
}

void BitrotEditor::section (Graphics& g, Rectangle<int> r) const
{
    g.setColour (ff::face);
    g.fillRoundedRectangle (r.toFloat(), 12.0f);
}

void BitrotEditor::footswitchMenu()
{
    PopupMenu m;
    m.addSectionHeader (proc.footInputStatus());
    const auto want = proc.footDevice();
    m.addItem (1, "Find my footswitch automatically", true, want.isEmpty());
    const auto devices = proc.footDeviceNames();
    for (int i = 0; i < devices.size(); ++i) m.addItem (100 + i, devices[i], true, want == devices[i]);
    m.addSeparator();
    m.addItem (2, "Only from a MIDI track in Ableton", true, want == "none");
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (feet[0]), [this, devices] (int r)
    {
        if (r == 1) proc.setFootDevice ({});
        if (r == 2) proc.setFootDevice ("none");
        if (r >= 100 && r - 100 < devices.size()) proc.setFootDevice (devices[r - 100]);
    });
}

void BitrotEditor::paint (Graphics& g)
{
    g.fillAll (ff::bg);
    g.setColour (ff::ink);
    g.setFont (ff::font (26.0f, 0.06f));
    g.drawText ("BITROT", 20, 20, 120, 40, Justification::centredLeft, false);
    ff::label (g, "MODULATED MONOPHONIC PLL", { 130, 20, 240, 40 });
    ff::label (g, "INPUT", { 380, 20, 50, 40 });

    auto hint = [&] (const String& t, Rectangle<int> r, Justification j = Justification::centredLeft) { ff::label (g, t, r, j, ff::hint, 11.0f); };
    auto& s = proc.apvts;

    if (page == 0)
    {
        section (g, { 20, 76, 300, 440 });
        section (g, { 340, 76, 290, 440 });
        section (g, { 650, 76, 270, 440 });
        section (g, { 940, 76, 300, 440 });

        ff::label (g, "VOICE MIXER", { 36, 88, 150, 16 });
        ff::label (g, proc.effectiveBypass() ? "OFF" : "3 VOICES", { 200, 88, 104, 16 }, Justification::centredRight, ff::ink);
        hint ("DRY IS NEW:", { 222, 258, 90, 14 });
        hint ("0 SOUNDS LIKE", { 222, 274, 90, 14 });
        hint ("THE PEDAL", { 222, 290, 90, 14 });
        hint ("NO ENVELOPE: FULL LEVEL, ENDLESS", { 36, 466, 268, 14 });
        hint ("SUSTAIN, THEN A SUDDEN STOP", { 36, 482, 268, 14 });

        const int si = choiceOf (proc, ParamIDs::subInterval, 7);
        ff::label (g, "SUBHARMONIC", { 356, 88, 150, 16 });
        ff::label (g, String::fromUTF8 ("÷") + String (si + 2), { 520, 88, 94, 16 }, Justification::centredRight, ff::coral);
        hint (String::fromUTF8 ("−1 OCT"), { 356, 230, 100, 14 });
        hint (String::fromUTF8 ("−3 OCT · M2"), { 500, 230, 114, 14 }, Justification::centredRight);
        ff::label (g, "ROOT", { 356, 252, 100, 16 });
        const bool fromOsc = choiceOf (proc, ParamIDs::subRoot, 1) == 1;
        hint (fromOsc ? "DIVIDES THE OSCILLATOR: SUM AND" : "DIVIDES THE SQUARE: STEADIER,", { 356, 308, 258, 14 });
        hint (fromOsc ? "DIFFERENCE, GLIDE AND VIBRATO ON BOTH" : "GLIDE AND VIBRATO LEAVE IT ALONE", { 356, 324, 258, 14 });
        ff::label (g, "FLOOR", { 356, 362, 60, 16 });
        g.setColour (ff::housing);
        g.fillRoundedRectangle (356.0f, 396.0f, 258.0f, 50.0f, 10.0f);
        ff::label (g, "SUB OUT", { 368, 402, 100, 16 }, Justification::centredLeft, ff::dim, 12.0f);
        ff::label (g, subHzLine, { 470, 402, 132, 16 }, Justification::centredRight, ff::ink, 12.0f);
        ff::label (g, subFloorLine, { 368, 422, 240, 16 }, Justification::centredLeft, subLifted ? ff::coral : ff::hint, 11.0f);
        hint ("TAP B NEXT · HOLD B BACK", { 356, 488, 258, 14 });

        ff::label (g, "FREQUENCY MODULATOR", { 666, 88, 238, 16 });
        ff::label (g, "CORRUPT", { 666, 392, 120, 16 });
        const float c = s.getRawParameterValue (ParamIDs::corrupt)->load();
        ff::label (g, c < 0.34f ? "TIGHT" : (c < 0.67f ? "LOOSE" : "WILD"), { 784, 392, 120, 16 }, Justification::centredRight, ff::ink);
        hint ("TIGHT · LOCK ASSIST", { 666, 438, 120, 14 });
        hint ("WILD · RAW LOOP", { 790, 438, 114, 14 }, Justification::centredRight);
        hint ("WILD LETS THE LOOP HUNT,", { 666, 466, 238, 14 });
        hint ("CHIRP AND STUTTER", { 666, 482, 238, 14 });

        const int oi = choiceOf (proc, ParamIDs::oscInterval, 7);
        ff::label (g, "MASTER OSCILLATOR", { 956, 88, 200, 16 });
        ff::label (g, octLit ? "x" + String (oi + 1) + " > x" + String ((oi + 1) * 2) : "x" + String (oi + 1),
                   { 1124, 88, 100, 16 }, Justification::centredRight, ff::teal);
        hint ("UNISON", { 956, 284, 80, 14 });
        hint ("THE HARMONIC SERIES", { 1030, 284, 120, 14 }, Justification::centred);
        hint ("+3 OCT", { 1150, 284, 74, 14 }, Justification::centredRight);
        ff::label (g, "ROOT", { 956, 314, 50, 16 });
        g.setColour (ff::housing);
        g.fillRoundedRectangle (956.0f, 348.0f, 268.0f, 50.0f, 10.0f);
        ff::label (g, "OSC OUT", { 968, 354, 100, 16 }, Justification::centredLeft, ff::dim, 12.0f);
        ff::label (g, oscHzLine, { 968, 372, 140, 18 }, Justification::centredLeft, ff::ink, 15.0f);
        {
            Rectangle<float> chip (1140.0f, 360.0f, 72.0f, 26.0f);
            g.setColour (octLit ? ff::coral : ff::cardLow);
            g.fillRoundedRectangle (chip, 8.0f);
            ff::label (g, "OCT UP", chip.toNearestInt(), Justification::centred, octLit ? ff::faceInk : ff::faint, 12.0f);
        }
        ff::label (g, String::fromUTF8 ("EXPRESSION · CLICK TO CHOOSE"), { 956, 422, 268, 16 }, Justification::centredLeft, ff::dim, 12.0f);
        hint ("TAP D OCT UP · HOLD D WHILE HELD", { 956, 488, 268, 14 });
    }
    else if (page == 1)
    {
        section (g, { 20, 76, 300, 440 });
        section (g, { 340, 76, 290, 440 });
        section (g, { 650, 76, 270, 440 });
        section (g, { 940, 76, 300, 440 });
        ff::label (g, "TRACKING", { 36, 88, 200, 16 });
        ff::label (g, "TRACKING COMPRESSOR", { 36, 118, 268, 16 });
        hint ("LEVELS WHAT THE LOOP HEARS, NEVER THE SOUND", { 36, 176, 268, 14 });
        hint ("THE PEDAL GATES WEAK SIGNALS; HIGHER", { 36, 330, 268, 14 });
        hint ("SENSITIVITY HEARS QUIETER PLAYING", { 36, 346, 268, 14 });
        ff::label (g, "CORRUPT", { 36, 396, 100, 16 });
        hint ("BASS LEANS TIGHT AT THE SAME SETTING", { 36, 446, 268, 14 });

        ff::label (g, "SUBHARMONIC", { 356, 88, 200, 16 });
        ff::label (g, "FLOOR FREQUENCY", { 356, 118, 258, 16 });
        hint ("SUBS BELOW IT MOVE UP BY OCTAVES", { 356, 176, 258, 14 });
        ff::label (g, "OSCILLATOR ROOT SUB", { 356, 248, 258, 16 });
        hint ("SUM + DIFFERENCE IS THE MANUAL'S", { 356, 306, 258, 14 });
        hint ("\"CRACKED MACHINE\"; DIVIDED IS CLEANER", { 356, 322, 258, 14 });

        ff::label (g, "VIBRATO", { 666, 88, 200, 16 });
        hint ("FIXED ON THE PEDAL. 1 SEMITONE IS ABOUT", { 666, 276, 238, 14 });
        hint ("THE ORIGINAL'S LASER", { 666, 292, 238, 14 });

        ff::label (g, "BEND", { 956, 88, 200, 16 });
        hint ("AN UNSTEPPED SWEEP UP TO 3 OCTAVES", { 956, 276, 268, 14 });
        hint ("ON TOP OF THE INTERVAL. AN EXPRESSION TARGET", { 956, 292, 268, 14 });
    }
    else
    {
        section (g, { 20, 76, 1220, 440 });
        ff::label (g, "FOOTSWITCHES", { 36, 92, 200, 16 });
        hint (String::fromUTF8 ("READ STRAIGHT FROM THE USB CABLE · NO MAPPING NEEDED"), { 580, 93, 644, 14 }, Justification::centredRight);
        static const char* tapJobs[4] = { "BYPASS", "NEXT SUB INTERVAL", "FREEZE ON OR OFF", "OCTAVE UP ON OR OFF" };
        static const char* holdJobs[4] = { "FLIPS WHILE HELD", "PREVIOUS SUB INTERVAL", "FREEZE WHILE HELD", "OCTAVE UP WHILE HELD" };
        auto& ps = proc.pedals();
        for (int i = 0; i < 4; ++i)
        {
            const int x = 40 + i * 300;
            auto row = [&] (const String& k, const String& v, int y)
            {
                ff::label (g, k, { x, y, 140, 18 }, Justification::centredLeft, ff::dim, 13.0f);
                ff::label (g, v, { x + 60, y, 218, 18 }, Justification::centredRight, ff::ink, 13.0f);
            };
            row ("TAP", tapJobs[i], 204);
            row ("HOLD", holdJobs[i], 226);
            row ("LISTENS FOR", "CC " + String (ps.fsCC[(size_t) i]) + (proc.pedalsDirect() ? String::fromUTF8 (" · USB") : String::fromUTF8 (" · CH ") + String (ps.fsChannel[(size_t) i])), 248);
        }
        auto combo = [&] (const String& k, const String& v, int y)
        {
            ff::label (g, k, { 36, y, 200, 18 }, Justification::centredLeft, ff::dim, 13.0f);
            ff::label (g, v, { 236, y, 380, 18 }, Justification::centredRight, ff::ink, 13.0f);
        };
        combo ("A + B", "NEXT PRESET", 322);
        combo ("C + D", "HAND FOOTSWITCHES TO THE NEXT BITROT", 346);
        combo ("C + D HELD 1 SECOND", "KEEPER", 370);
        ff::label (g, "EXPRESSION", { 36, 408, 140, 16 });
        hint ("CC " + String (ps.expCC) + (proc.pedalsDirect() ? String::fromUTF8 (" · USB · CLICK THE BAR TO CHOOSE WHAT IT SWEEPS") : String::fromUTF8 (" · CH ") + String (ps.expChannel) + String::fromUTF8 (" · CLICK THE BAR TO CHOOSE")), { 150, 409, 466, 14 });
        ff::label (g, "NO ORIGINAL MIDI MAP", { 648, 322, 300, 16 });
        hint ("THE PEDAL HAS NO MIDI, SO NOTHING CAN CLASH WITH CC 20 TO 23 OR 11", { 648, 346, 576, 14 });
        ff::label (g, "KEEPER", { 648, 408, 120, 16 });
        hint ("SAVES THE LAST 8 SECONDS IN AND OUT, PLUS EVERY SETTING", { 740, 409, 484, 14 });
        hint ("TO DOCUMENTS / FLIP FELLA KEEPERS", { 648, 482, 576, 14 });
    }

    // ---- bottom strip ----
    section (g, { 20, 532, 1220, 112 });
    ff::label (g, "PRESETS", { 410, 548, 120, 16 });
    hint (String::fromUTF8 ("TAP TO LOAD · HOLD TO SAVE"), { 486, 550, 196, 14 });
    ff::label (g, "FOOTSWITCHES", { 700, 548, 120, 16 });
    const bool directOk = proc.pedalsDirect();
    const Colour dot = directOk || proc.hostFeedsPedals.load() ? ff::green : ff::pin;
    g.setColour (dot.withAlpha (0.25f));
    g.fillEllipse (824.0f, 547.0f, 18.0f, 18.0f);
    g.setColour (dot);
    g.fillEllipse (828.0f, 551.0f, 10.0f, 10.0f);
    ff::label (g, footSay, { 848, 548, 200, 16 }, Justification::centredLeft, footSay == "READY" || footSay == "NOT CONNECTED" ? ff::dim : ff::ink, 12.0f);
    hint (String::fromUTF8 ("C+D HOLD 1 S · KEEPER"), { 1054, 538, 170, 14 }, Justification::centredRight);
}

void BitrotEditor::resized() {}

void BitrotEditor::timerCallback()
{
    const double now = Time::getMillisecondCounterHiRes() * 0.001;
    const double dt = jlimit (0.0, 0.1, now - lastTick);
    lastTick = now;
    auto& eng = proc.getEngine();

    const int si = choiceOf (proc, ParamIDs::subInterval, 7), oi = choiceOf (proc, ParamIDs::oscInterval, 7);
    const bool up = proc.effectiveOctUp();
    subCard->advance (dt);
    subCard->setValue (String::fromUTF8 (subNames[si]));
    oscCard->advance (dt);
    oscCard->setValue (String::fromUTF8 (up ? oscNamesUp[oi] : oscNames[oi]));
    for (auto* c : pedalCards) c->advance (dt);

    const bool floorOn = proc.apvts.getRawParameterValue (ParamIDs::floor)->load() > 0.5f;
    const float subRaw = eng.uiSubRawHz.load(), subOut = eng.uiSubHz.load();
    const bool sounding = eng.uiGateOpen.load() || eng.uiFrozen.load();
    String sh = sounding ? hz (subOut) : String::fromUTF8 ("—");
    String sf;
    const bool lifted = floorOn && eng.uiFloored.load() && sounding;
    if (! floorOn) sf = "FLOOR OFF: SUBS CAN FLUTTER";
    else if (lifted) sf = hz (subRaw) + String::fromUTF8 (" IS BELOW THE FLOOR · LIFTED");
    else sf = "ABOVE THE FLOOR: NO CHANGE";
    const String oh = sounding ? hz (eng.uiOscHz.load()) : String::fromUTF8 ("—");

    String fs = proc.pedalsDirect() || proc.hostFeedsPedals.load() ? "READY" : "NOT CONNECTED";
    if (now - proc.uiMessageAt < 2.5 && proc.uiMessage.isNotEmpty()) fs = proc.uiMessage;
    if (sh != subHzLine || sf != subFloorLine || oh != oscHzLine || fs != footSay || lifted != subLifted || up != octLit)
    {
        subHzLine = sh; subFloorLine = sf; oscHzLine = oh; footSay = fs; subLifted = lifted; octLit = up;
        repaint();
    }
    if (page == 0) { repaint (340, 76, 290, 30); repaint (650, 380, 270, 30); }
    if (page == 2) repaint (20, 76, 1220, 440);

    pebble->update (now, dt);
    tracking->tick (now, dt);
    if (page == 0) { scope->repaint(); fmCurve->repaint(); subSteps->repaint(); oscSteps->repaint(); expBar->repaint(); }
    for (auto* f : feet) f->repaint();
    bypass->repaint();
    banks->repaint();
    if (page == 2) expBarPedal->repaint();
}
