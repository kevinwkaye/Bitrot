#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

// Several Bitrot windows can be open at once. Footswitches drive one of them at a time:
// the one whose panel was last touched, or the next one along when C+D are tapped.
namespace
{
SpinLock focusLock;
Array<BitrotProcessor*> instances;
std::atomic<BitrotProcessor*> focused { nullptr };

bool hasFocus (BitrotProcessor* p)
{
    auto* f = focused.load();
    return f == nullptr || f == p;
}

// set while a message from the footswitch device is being handled on the MIDI thread
thread_local bool fromDeviceNow = false;

// footswitch device messages go to the copy that is learning, else the copy in charge
void sendToFocused (const MidiMessage& m)
{
    const SpinLock::ScopedLockType l (focusLock);
    BitrotProcessor* target = nullptr;
    for (auto* p : instances) if (p->isLearning()) target = p;
    if (target == nullptr) target = focused.load();
    if (target == nullptr && ! instances.isEmpty()) target = instances.getLast();
    if (target != nullptr) target->handleDirectMidi (m);
}

double nowSeconds() { return Time::getMillisecondCounterHiRes() * 0.001; }
} // namespace

BitrotProcessor::BitrotProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "BitrotState", createParameterLayout())
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
            raw[rp->getParameterID()] = apvts.getRawParameterValue (rp->getParameterID());
    apvts.addParameterListener (ParamIDs::input, this);
    settings.load();
    settingsStamp = PedalSettings::file().getLastModificationTime();
    direct->setSink (&sendToFocused);
    direct->setWanted (settings.footDevice);
    footswitches.onEvent = [this] (int sw, FootswitchLogic::Event e) { onFootEvent (sw, e); };
    makeFactoryPresets();
    {
        const SpinLock::ScopedLockType l (focusLock);
        instances.add (this);
    }
    focused.store (this);
    startTimerHz (100);
}

BitrotProcessor::~BitrotProcessor()
{
    stopTimer();
    apvts.removeParameterListener (ParamIDs::input, this);
    const SpinLock::ScopedLockType l (focusLock);
    instances.removeAllInstancesOf (this);
    if (focused.load() == this) focused.store (instances.isEmpty() ? nullptr : instances.getLast());
}

void BitrotProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
    keeper.prepare (sampleRate);
}

bool BitrotProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != AudioChannelSet::stereo()) return false;
    return in == AudioChannelSet::stereo() || in == AudioChannelSet::mono();
}

// Picking the Bass input switches the sub floor on; Guitar and Synth switch it off.
// The toggle in the Subharmonic section overrides it either way.
void BitrotProcessor::parameterChanged (const String& id, float value)
{
    if (id != ParamIDs::input || loadingState) return;
    const bool bass = (int) std::lround (value) == 1;
    MessageManager::callAsync ([self = this, bass]
    {
        {
            const SpinLock::ScopedLockType l (focusLock);
            if (! instances.contains (self)) return;   // closed in the meantime
        }
        self->setPlain (ParamIDs::floor, bass ? 1.0f : 0.0f);
    });
}

bit::EngineParams BitrotProcessor::readParams()
{
    using namespace ParamIDs;
    bit::EngineParams p;
    p.square = f (squareLevel); p.sub = f (subLevel); p.osc = f (oscLevel); p.dry = f (dry); p.level = f (level);
    p.subInterval = idx (subInterval); p.subRoot = idx (subRoot); p.floor = on (ParamIDs::floor);
    p.floorHz = idx (floorHz); p.subMix = idx (subMix);
    p.fmMode = idx (fmMode); p.rate = f (rate); p.depth = f (depth);
    p.oscInterval = idx (oscInterval); p.oscRoot = idx (oscRoot); p.bend = f (bend);
    p.input = idx (input); p.corrupt = f (corrupt); p.trackComp = on (trackComp); p.gate = f (gate);
    p.bypass = effectiveBypass();
    p.freeze = effectiveFreeze();
    p.octUp = effectiveOctUp();
    p.freezeBackMs = freezeBackMs.load();
    return p;
}

void BitrotProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n == 0 || getTotalNumOutputChannels() < 2) return;
    if (getTotalNumInputChannels() == 1) buffer.copyFrom (1, 0, buffer, 0, 0, n);

    for (const auto meta : midi) handleMidi (meta.getMessage(), false);
    midi.clear();

    const auto p = readParams();
    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);
    keeper.pushInput (L, R, n);
    engine.process (L, R, n, p);
    keeper.pushOutput (L, R, n);
}

// =====================================================================================
// MIDI: footswitches, expression and presets. The pedal has no MIDI of its own, so
// there is no original MIDI map to collide with the footswitches.
// =====================================================================================
void BitrotProcessor::queue (int kind, int a, float v)
{
    if (fromDeviceNow)
    {
        const auto scope = directFifo.write (1);
        if (scope.blockSize1 > 0) directPending[(size_t) scope.startIndex1] = { kind, a, v };
        return;
    }
    const auto scope = fifo.write (1);
    if (scope.blockSize1 > 0) pending[(size_t) scope.startIndex1] = { kind, a, v };
}

void BitrotProcessor::handleDirectMidi (const MidiMessage& m)
{
    // several MIDI devices can call in on their own threads
    const SpinLock::ScopedLockType l (directWriteLock);
    fromDeviceNow = true;
    handleMidi (m, true);
    fromDeviceNow = false;
}

void BitrotProcessor::setFootDevice (const String& device)
{
    settings.footDevice = device;
    savePedals();
    direct->setWanted (device);
}

void BitrotProcessor::savePedals()
{
    settings.save();
    settingsStamp = PedalSettings::file().getLastModificationTime();
}

void BitrotProcessor::handleMidi (const MidiMessage& m, bool fromDevice)
{
    const int ch = m.getChannel();
    if (m.isProgramChange())
    {
        if (ch == settings.fsChannel[0] && (fromDevice || hasFocus (this)))
            queue (LoadPreset, m.getProgramChangeNumber(), 0.0f);
        return;
    }
    if (! m.isController()) return;
    const int cc = m.getControllerNumber(), val = m.getControllerValue();

    if (learnTarget.load() != LearnNone)
    {
        if (val > 0) queue (Learned, ch * 128 + cc, 0.0f);
        return;
    }

    // Footswitches and expression. Straight from the footswitch's own cable any channel
    // will do; through Ableton the channel must match.
    if (fromDevice || ! pedalsDirect())
    {
        int foot = -1;
        for (int k = 0; k < 4 && foot < 0; ++k)
            if ((fromDevice || ch == settings.fsChannel[(size_t) k]) && cc == settings.fsCC[(size_t) k]) foot = k;
        const bool expression = foot < 0 && (fromDevice || ch == settings.expChannel) && cc == settings.expCC;
        if (foot >= 0 || expression)
        {
            if (! fromDevice)
            {
                hostFeedsPedals.store (true);
                const SpinLock::ScopedLockType l (focusLock);
                auto* fc = focused.load();
                if (fc != this && (fc == nullptr || ! fc->hostFeedsPedals.load())) focused.store (this);
                if (focused.load() != this) return;
            }
            if (foot >= 0) queue (val >= 64 ? FootPress : FootRelease, foot, 0.0f);
            else queue (Expression, val, 0.0f);
        }
    }
}

void BitrotProcessor::setNorm (const String& id, float v01)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (jlimit (0.0f, 1.0f, v01));
        p->endChangeGesture();
    }
}

void BitrotProcessor::setPlain (const String& id, float plain)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
        p->endChangeGesture();
    }
}

void BitrotProcessor::toggle (const char* id) { setPlain (id, on (id) ? 0.0f : 1.0f); }

void BitrotProcessor::say (const String& s)
{
    uiMessage = s;
    uiMessageAt = nowSeconds();
}

void BitrotProcessor::applyExpression (float v)
{
    uiExp.store (v);
    const int t = jlimit (0, (int) names::expTargets().size() - 1, idx (ParamIDs::expTarget));
    const String id = names::expTargets()[(size_t) t].id;
    if (id == ParamIDs::oscInterval)
    {
        // eight harmonic steps from heel to toe, with a little hysteresis so a resting pedal never flickers
        const float pos = v * 8.0f;
        const int cur = idx (ParamIDs::oscInterval);
        if (pos < (float) cur - 0.15f || pos > (float) cur + 1.15f)
            setPlain (ParamIDs::oscInterval, (float) jlimit (0, 7, (int) pos));
        return;
    }
    setNorm (id, v);
}

void BitrotProcessor::timerCallback()
{
    const double now = nowSeconds();
    // settings changed by another copy, or another Flip Fella plugin: pick them up
    if (now >= nextSettingsCheck)
    {
        nextSettingsCheck = now + 1.0;
        const auto stamp = PedalSettings::file().getLastModificationTime();
        if (stamp != settingsStamp && learnTarget.load() == LearnNone && ! calibrating)
        {
            settingsStamp = stamp;
            settings.load();
            direct->setWanted (settings.footDevice);
        }
        const bool d = pedalsDirect();
        if (d != wasDirect)
        {
            wasDirect = d;
            if (d && hasFocus (this)) say ("FOOTSWITCHES READY");
        }
    }

    auto apply = [&] (const std::array<Pending, 512>& from, int start, int size)
    {
        for (int i = start; i < start + size; ++i)
        {
            const auto& q = from[(size_t) i];
            switch (q.kind)
            {
                case FootPress: footswitches.press (q.a, now); break;
                case FootRelease: footswitches.release (q.a, now); break;
                case LoadPreset: if (q.a >= 0 && q.a < kPresets) loadPreset (q.a); break;
                case Expression:
                {
                    const int cc = q.a;
                    expMovedAt = now;
                    if (calibrating)
                    {
                        if (calFirst < 0) calFirst = cc;
                        calLo = jmin (calLo, cc);
                        calHi = jmax (calHi, cc);
                        uiExp.store ((float) cc / 127.0f);
                        break;
                    }
                    applyExpression (settings.expressionValue (cc));
                    break;
                }
                case Learned:
                {
                    const int ch = q.a / 128, cc = q.a % 128;
                    const int what = learnTarget.exchange (LearnNone);
                    if (what >= 0 && what < 4)
                    {
                        settings.fsChannel[(size_t) what] = ch;
                        settings.fsCC[(size_t) what] = cc;
                        say ("FOOTSWITCH " + String::charToString ((juce_wchar) ('A' + what)) + " IS CC " + String (cc));
                    }
                    else if (what == LearnExpression)
                    {
                        settings.expChannel = ch;
                        settings.expCC = cc;
                        say ("EXPRESSION IS CC " + String (cc));
                    }
                    savePedals();
                    break;
                }
                default: break;
            }
        }
    };
    {
        const auto scope = fifo.read (fifo.getNumReady());
        apply (pending, scope.startIndex1, scope.blockSize1);
        apply (pending, scope.startIndex2, scope.blockSize2);
    }
    {
        const auto scope = directFifo.read (directFifo.getNumReady());
        apply (directPending, scope.startIndex1, scope.blockSize1);
        apply (directPending, scope.startIndex2, scope.blockSize2);
    }

    // Keeper as an automatable button
    if (raw.at (ParamIDs::keeper)->load() > 0.5f)
    {
        setPlain (ParamIDs::keeper, 0.0f);
        keepNow();
    }

    footswitches.tick (now);

    // C has been down past the combo window without D: freeze on the pitch from the press
    if (freezePending && now - cDownAt >= FootswitchLogic::kComboWindow)
    {
        freezePending = false;
        freezeBackMs.store ((float) ((now - cDownAt) * 1000.0));
        freezeHold.store (true);
        if (! on (ParamIDs::freeze)) say ("FROZEN");
    }

    int mask = 0;
    for (int k = 0; k < 4; ++k) if (footswitches.isDown (k)) mask |= 1 << k;
    uiFootDown.store (mask);

    if (calibrating && now > calibrateUntil)
    {
        calibrating = false;
        if (calHi - calLo > 20)
        {
            // whichever end the pedal started at is the heel
            const bool startedLow = std::abs (calFirst - calLo) < std::abs (calFirst - calHi);
            settings.heel = startedLow ? calLo : calHi;
            settings.toe = startedLow ? calHi : calLo;
            savePedals();
            say ("EXPRESSION CALIBRATED");
        }
        else
            say ("NO MOVEMENT, TRY AGAIN");
    }
}

// =====================================================================================
// Footswitches. A is the pedal's own Flexi-Switch: tap to latch, hold to flip only
// while held. B steps the subharmonic. C freezes, D jumps the oscillator an octave up.
// C and D latch on a tap and work only while held on a hold.
//
//   A  tap: bypass              hold: flips on or off while held
//   B  tap: next sub interval   hold: previous sub interval
//   C  tap: freeze latch        hold: freeze while held
//   D  tap: octave up latch     hold: octave up while held
//   A+B: next preset    C+D tap: footswitches to the next Bitrot    C+D held 1 s: Keeper
//
// A and D answer the moment they go down. If the partner switch joins within the combo
// window, they are undone, so a combo never changes the sound. C waits out the window
// instead, then freezes the pitch from the moment it went down.
// =====================================================================================
void BitrotProcessor::onFootEvent (int sw, FootswitchLogic::Event e)
{
    using E = FootswitchLogic;
    const double now = nowSeconds();
    if (e == E::Down || e == E::ComboAB || e == E::ComboCD) { uiFootPulse.fetch_add (1); lastFoot = sw; }

    switch (e)
    {
        case E::ComboHoldCD:
            focused.store (this);   // Keeper on the plugin you were playing
            keepNow();
            return;
        case E::ComboCD:
        {
            octHold.store (false);
            freezePending = false;
            const SpinLock::ScopedLockType l (focusLock);
            const int i = instances.indexOf (this);
            auto* next = instances.isEmpty() ? nullptr : instances[(i + 1) % instances.size()];
            focused.store (next);
            if (next != nullptr) next->say ("FOOTSWITCHES HERE");
            return;
        }
        case E::ComboAB:
            bypassFlip.store (false);
            stepPreset (1);
            return;
        case E::ComboHoldAB:
            return;
        case E::Down:
            if (sw == 0) bypassFlip.store (true);
            if (sw == 2) { cDownAt = now; freezePending = true; }
            if (sw == 3) octHold.store (true);
            return;
        default: break;
    }

    if (e == E::Tap)
    {
        if (sw == 0)
        {
            toggle (ParamIDs::bypass);
            bypassFlip.store (false);
            say (on (ParamIDs::bypass) ? "BYPASSED" : "ON");
        }
        if (sw == 1) stepSub (1);
        if (sw == 2)
        {
            if (freezePending) { freezePending = false; freezeBackMs.store ((float) ((now - cDownAt) * 1000.0)); }
            toggle (ParamIDs::freeze);
            freezeHold.store (false);
            say (on (ParamIDs::freeze) ? "FREEZE LATCHED" : "FREEZE OFF");
        }
        if (sw == 3)
        {
            toggle (ParamIDs::octUp);
            octHold.store (false);
            say (on (ParamIDs::octUp) ? "OCTAVE UP LATCHED" : "OCTAVE UP OFF");
        }
        return;
    }
    if (e == E::HoldStart)
    {
        if (sw == 0) say (effectiveBypass() ? "OFF WHILE HELD" : "ON WHILE HELD");
        if (sw == 1) stepSub (-1);
        if (sw == 3) say ("OCTAVE UP WHILE HELD");
        return;
    }
    if (e == E::HoldEnd)
    {
        if (sw == 0) bypassFlip.store (false);
        if (sw == 2) { freezePending = false; freezeHold.store (false); }
        if (sw == 3) octHold.store (false);
    }
}

void BitrotProcessor::footDown (int sw)
{
    focused.store (this);
    footswitches.press (sw, nowSeconds());
}

void BitrotProcessor::footUp (int sw) { footswitches.release (sw, nowSeconds()); }

void BitrotProcessor::stepSub (int direction)
{
    const int s = (idx (ParamIDs::subInterval) + direction + 8) % 8;
    setPlain (ParamIDs::subInterval, (float) s);
    say (String ("SUB ") + names::subIntervals[s].toUpperCase());
}

void BitrotProcessor::stepOsc (int direction)
{
    const int s = (idx (ParamIDs::oscInterval) + direction + 8) % 8;
    setPlain (ParamIDs::oscInterval, (float) s);
}

void BitrotProcessor::learnFootswitch (int sw)
{
    learnTarget.store (sw);
    say ("PRESS FOOTSWITCH " + String::charToString ((juce_wchar) ('A' + sw)));
}

void BitrotProcessor::learnExpression()
{
    learnTarget.store (LearnExpression);
    say ("MOVE THE EXPRESSION PEDAL");
}

void BitrotProcessor::calibrateExpression()
{
    calibrating = true;
    calibrateUntil = nowSeconds() + 6.0;
    calLo = 127; calHi = 0; calFirst = -1;
    say ("HEEL DOWN, THEN ROCK TO TOE");
}

void BitrotProcessor::resetFootswitches()
{
    settings.resetFootswitches();
    savePedals();
    say ("FOOTSWITCH DEFAULTS RESTORED");
}

// =====================================================================================
// Keeper
// =====================================================================================
void BitrotProcessor::keepNow()
{
    DynamicObject::Ptr e = new DynamicObject();
    e->setProperty ("plugin", "Bitrot");
   #ifdef JucePlugin_VersionString
    e->setProperty ("version", JucePlugin_VersionString);
   #endif
    e->setProperty ("recreates", "EarthQuaker Devices Data Corrupter");
    e->setProperty ("time", Time::getCurrentTime().toISO8601 (true));
    e->setProperty ("context", settings.keeperLive ? "Live" : "Studio");
    e->setProperty ("preset", selected < 0 ? String ("none") : String (selected + 1));

    DynamicObject::Ptr tracking = new DynamicObject();
    tracking->setProperty ("note", engine.uiNote.load());
    tracking->setProperty ("lock", engine.uiLock.load());
    tracking->setProperty ("lastLockMs", engine.uiLockMs.load());
    tracking->setProperty ("lockLosses", engine.uiLockLosses.load());
    tracking->setProperty ("oscillatorHz", engine.uiOscHz.load());
    tracking->setProperty ("subHz", engine.uiSubHz.load());
    tracking->setProperty ("subFloored", engine.uiFloored.load());
    e->setProperty ("tracking", var (tracking.get()));

    DynamicObject::Ptr feet = new DynamicObject();
    feet->setProperty ("frozen", effectiveFreeze());
    feet->setProperty ("freezeHeld", freezeHold.load());
    feet->setProperty ("octaveUp", effectiveOctUp());
    feet->setProperty ("octaveHeld", octHold.load());
    feet->setProperty ("bypassFlipped", bypassFlip.load());
    e->setProperty ("footswitches", var (feet.get()));

    DynamicObject::Ptr exp = new DynamicObject();
    exp->setProperty ("target", names::expTargets()[(size_t) jlimit (0, (int) names::expTargets().size() - 1, idx (ParamIDs::expTarget))].label);
    exp->setProperty ("position", uiExp.load());
    exp->setProperty ("movedSecondsAgo", nowSeconds() - expMovedAt);
    e->setProperty ("expression", var (exp.get()));

    // every parameter, hidden ones included, as value and as shown on the panel
    DynamicObject::Ptr params = new DynamicObject();
    for (auto* prm : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (prm))
        {
            if (rp->getParameterID() == ParamIDs::keeper) continue;
            DynamicObject::Ptr pv = new DynamicObject();
            pv->setProperty ("value", rp->convertFrom0to1 (rp->getValue()));
            pv->setProperty ("shown", rp->getCurrentValueAsText());
            params->setProperty (rp->getParameterID(), var (pv.get()));
        }
    e->setProperty ("parameters", var (params.get()));

    keeper.keep (e);
    uiKeepPulse.fetch_add (1);
    say ("KEPT");
}

File BitrotProcessor::exportKeepers()
{
    const auto f = keeper.exportZip();
    if (f.existsAsFile()) { say ("EXPORTED TO DOCUMENTS"); f.revealToUser(); }
    else say ("NOTHING KEPT YET");
    return f;
}

void BitrotProcessor::openKeepersFolder()
{
    Keeper::folder().createDirectory();
    Keeper::folder().startAsProcess();
}

void BitrotProcessor::setKeeperLive (bool live)
{
    settings.keeperLive = live;
    savePedals();
}

// =====================================================================================
// Presets: 16 slots in four banks. Each saves the sound. Your instrument settings,
// bypass and the footswitch latches stay as they are.
// =====================================================================================
bool BitrotProcessor::inSnapshot (const String& id)
{
    static const StringArray excluded { ParamIDs::bypass, ParamIDs::input, ParamIDs::keeper, ParamIDs::freeze,
                                        ParamIDs::octUp, ParamIDs::gate, ParamIDs::trackComp, ParamIDs::expTarget,
                                        ParamIDs::floor, ParamIDs::floorHz };
    return ! excluded.contains (id);
}

ValueTree BitrotProcessor::snapshotOf()
{
    ValueTree t ("Snapshot");
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
            if (inSnapshot (rp->getParameterID()))
                t.setProperty (Identifier (rp->getParameterID()), rp->convertFrom0to1 (rp->getValue()), nullptr);
    return t;
}

void BitrotProcessor::applySnapshot (const ValueTree& t)
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
        {
            const Identifier id (rp->getParameterID());
            if (inSnapshot (rp->getParameterID()) && t.hasProperty (id))
                setPlain (rp->getParameterID(), (float) t.getProperty (id));
        }
}

void BitrotProcessor::makeFactoryPresets()
{
    // four starting sounds in the red bank; the rest are yours
    auto make = [&] (int slot, const std::map<String, float>& values)
    {
        ValueTree t ("Snapshot");
        for (auto* p : getParameters())
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
                if (inSnapshot (rp->getParameterID()))
                    t.setProperty (Identifier (rp->getParameterID()), rp->convertFrom0to1 (rp->getDefaultValue()), nullptr);
        for (const auto& [id, v] : values) t.setProperty (Identifier (id), v, nullptr);
        slots[(size_t) slot] = t;
    };
    using namespace ParamIDs;
    // 1 Equinox, after the manual's drawing: high melodies, three octaves of sub, +2 oct and a 5th, slow glide
    make (0, { { squareLevel, 0.5f }, { subLevel, 0.75f }, { oscLevel, 0.45f }, { level, 0.4f }, { subInterval, 6 }, { subRoot, 0 },
               { fmMode, 0 }, { rate, 0.85f }, { oscInterval, 5 }, { oscRoot, 1 } });
    // 2 Fisted Fifth Riffs, after the manual's drawing: fifths above and below
    make (1, { { squareLevel, 0.5f }, { subLevel, 0.6f }, { oscLevel, 0.5f }, { level, 0.75f }, { subInterval, 4 }, { subRoot, 0 },
               { fmMode, 0 }, { rate, 0.8f }, { oscInterval, 2 }, { oscRoot, 0 } });
    // 3 Bass Hole, from EarthQuaker's bass column: Unison root, +2 oct, sub from the oscillator, square off, some dry
    make (2, { { squareLevel, 0.0f }, { subLevel, 0.6f }, { oscLevel, 0.5f }, { dry, 0.45f }, { level, 0.6f }, { subInterval, 0 },
               { subRoot, 1 }, { subMix, 1 }, { fmMode, 0 }, { rate, 0.7f }, { oscInterval, 3 }, { oscRoot, 0 }, { corrupt, 0.15f } });
    // 4 Laser: vibrato at speed on +3 oct, a loose loop
    make (3, { { squareLevel, 0.3f }, { subLevel, 0.4f }, { oscLevel, 0.7f }, { level, 0.6f }, { subInterval, 0 }, { subRoot, 1 },
               { fmMode, 1 }, { rate, 0.78f }, { depth, 0.55f }, { oscInterval, 7 }, { oscRoot, 0 }, { corrupt, 0.6f } });
}

void BitrotProcessor::savePreset (int slot)
{
    if (slot < 0 || slot >= kPresets) return;
    slots[(size_t) slot] = snapshotOf();
    selected = slot;
    say ("SAVED " + String (slot + 1));
}

void BitrotProcessor::loadPreset (int slot)
{
    if (! hasPreset (slot)) return;
    applySnapshot (slots[(size_t) slot]);
    selected = slot;
    say ("PRESET " + String (slot + 1));
}

bool BitrotProcessor::hasPreset (int slot) const
{
    return slot >= 0 && slot < kPresets && slots[(size_t) slot].isValid();
}

void BitrotProcessor::stepPreset (int direction)
{
    for (int k = 1; k <= kPresets; ++k)
    {
        const int s = ((selected < 0 ? (direction > 0 ? -1 : 0) : selected) + direction * k + kPresets * 2) % kPresets;
        if (hasPreset (s)) { loadPreset (s); return; }
    }
    say ("NO PRESETS SAVED YET");
}

// =====================================================================================
// Saving with the set: parameters and presets
// =====================================================================================
void BitrotProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();
    for (int i = state.getNumChildren(); --i >= 0;)
        if (state.getChild (i).getType().toString() == "Presets") state.removeChild (i, nullptr);
    ValueTree presets ("Presets");
    presets.setProperty ("selected", selected, nullptr);
    for (int i = 0; i < kPresets; ++i)
    {
        ValueTree slot ("Slot");
        if (slots[(size_t) i].isValid()) slot.appendChild (slots[(size_t) i].createCopy(), nullptr);
        presets.appendChild (slot, nullptr);
    }
    state.appendChild (presets, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void BitrotProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto tree = ValueTree::fromXml (*xml);

    const auto presets = tree.getChildWithName ("Presets");
    if (presets.isValid())
    {
        selected = (int) presets.getProperty ("selected", -1);
        for (int i = 0; i < kPresets && i < presets.getNumChildren(); ++i)
        {
            const auto slot = presets.getChild (i);
            slots[(size_t) i] = slot.getNumChildren() > 0 ? slot.getChild (0).createCopy() : ValueTree();
        }
        tree.removeChild (presets, nullptr);
    }
    loadingState = true;   // a saved set keeps its own floor setting
    apvts.replaceState (tree);
    loadingState = false;
}

AudioProcessorEditor* BitrotProcessor::createEditor()
{
    focused.store (this); // opening a copy's window gives it the footswitches
    return new BitrotEditor (*this);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BitrotProcessor();
}
