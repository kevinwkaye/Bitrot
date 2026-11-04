// Footswitch and expression test. Drives the real processor with plain 127/0 control
// changes on channel 16, as the footswitch sends them, and checks every tap, hold and
// combo does its job and nothing else. Combos never change the sound, and CC 20 to 23
// and 11 never reach anything but their footswitch and expression jobs.
//
// bitrot_foot_test --snapshot <folder> writes the three panel pages as PNGs.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>

using namespace juce;

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

struct Rig
{
    BitrotProcessor proc;
    AudioBuffer<float> buffer { 2, 256 };
    int clock = 0;

    Rig()
    {
        proc.pedals() = PedalSettings();   // defaults, whatever is saved on this computer
        proc.pedals().footDevice = "none";
        proc.prepareToPlay (48000.0, 256);
        proc.setPlayConfigDetails (2, 2, 48000.0, 256);
    }

    // a steady low E, so the engine has a note to freeze
    void fill()
    {
        for (int i = 0; i < 256; ++i)
        {
            const float v = 0.3f * std::sin ((float) (clock + i) * 6.2831853f * 82.41f / 48000.0f);
            buffer.setSample (0, i, v); buffer.setSample (1, i, v);
        }
        clock += 256;
    }
    void block (MidiBuffer& midi) { fill(); proc.processBlock (buffer, midi); }
    void cc (int number, int value, int channel = 16)
    {
        MidiBuffer m;
        m.addEvent (MidiMessage::controllerEvent (channel, number, value), 0);
        block (m);
    }
    // keep audio flowing while the message thread runs the footswitch timer
    void pump (int ms)
    {
        const double end = Time::getMillisecondCounterHiRes() + ms;
        while (Time::getMillisecondCounterHiRes() < end)
        {
            MidiBuffer none;
            block (none);
            MessageManager::getInstance()->runDispatchLoopUntil (5);
        }
    }
    void tapSwitch (int number) { cc (number, 127); pump (60); cc (number, 0); pump (60); }
    void holdSwitch (int number, int ms) { cc (number, 127); pump (ms); }
    void releaseSwitch (int number) { cc (number, 0); pump (60); }

    float value (const char* id) { auto* p = proc.apvts.getParameter (id); return p->convertFrom0to1 (p->getValue()); }

    std::map<String, float> snapshot()
    {
        std::map<String, float> m;
        for (auto* p : proc.getParameters())
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) m[rp->getParameterID()] = rp->getValue();
        return m;
    }
    StringArray changedSince (const std::map<String, float>& before)
    {
        StringArray out;
        for (auto& [id, v] : snapshot()) if (std::abs (before.at (id) - v) > 1.0e-6f) out.add (id);
        return out;
    }
};

static bool only (const StringArray& changed, const StringArray& allowed)
{
    for (auto& c : changed) if (! allowed.contains (c)) return false;
    return true;
}

static int snapshot (const File& dir)
{
    Rig r;
    dir.createDirectory();
    std::unique_ptr<AudioProcessorEditor> ed (r.proc.createEditor());
    auto* editor = dynamic_cast<BitrotEditor*> (ed.get());
    r.pump (300);
    for (int pg = 0; pg < 3; ++pg)
    {
        editor->showPage (pg);
        for (int i = 0; i < 30; ++i) { editor->tickForSnapshot(); r.pump (5); }
        auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        static const char* names[3] = { "main", "deep", "pedal" };
        FileOutputStream out (dir.getChildFile (String (names[pg]) + ".png"));
        out.setPosition (0); out.truncate();
        PNGImageFormat().writeImageToStream (img, out);
    }
    // Pebble and the tracking strip when the loop has lost lock, with octave up on
    editor->showPage (0);
    r.proc.apvts.getParameter (ParamIDs::octUp)->setValueNotifyingHost (1.0f);
    for (int i = 0; i < 40; ++i)
    {
        r.proc.getEngine().uiLock.store (0.05f);
        editor->tickForSnapshot();
        MessageManager::getInstance()->runDispatchLoopUntil (16);
    }
    {
        auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        FileOutputStream out (dir.getChildFile ("glitch.png"));
        out.setPosition (0); out.truncate();
        PNGImageFormat().writeImageToStream (img, out);
    }
    std::printf ("Snapshots written to %s\n", dir.getFullPathName().toRawUTF8());
    return 0;
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI juce;
    if (argc > 2 && String (argv[1]) == "--snapshot") return snapshot (File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    Rig r;
    using namespace ParamIDs;
    r.pump (300);   // let the loop lock on the low E

    std::printf ("Taps\n");
    {
        auto before = r.snapshot();
        r.tapSwitch (20);
        check (r.value (bypass) > 0.5f && r.proc.effectiveBypass(), "A tap bypasses");
        check (only (r.changedSince (before), { bypass }), "A tap changes nothing else: " + r.changedSince (before).joinIntoString (", "));
        r.tapSwitch (20);
        check (r.value (bypass) < 0.5f && ! r.proc.effectiveBypass(), "A tap again turns it back on");

        before = r.snapshot();
        const int s0 = (int) r.value (subInterval);
        r.tapSwitch (21);
        check ((int) r.value (subInterval) == (s0 + 1) % 8, "B tap steps the subharmonic on");
        check (only (r.changedSince (before), { subInterval }), "B tap changes only the subharmonic");
        for (int k = 0; k < 7; ++k) r.tapSwitch (21);
        check ((int) r.value (subInterval) == s0, "eight B taps come back round");

        before = r.snapshot();
        r.tapSwitch (22);
        check (r.value (freeze) > 0.5f && r.proc.getEngine().uiFrozen.load(), "C tap latches freeze");
        check (only (r.changedSince (before), { freeze }), "C tap changes only freeze");
        r.tapSwitch (22);
        check (r.value (freeze) < 0.5f && ! r.proc.getEngine().uiFrozen.load(), "C tap again releases it");

        before = r.snapshot();
        r.tapSwitch (23);
        check (r.value (octUp) > 0.5f && r.proc.effectiveOctUp(), "D tap latches octave up");
        check (only (r.changedSince (before), { octUp }), "D tap changes only octave up");
        r.tapSwitch (23);
        check (r.value (octUp) < 0.5f && ! r.proc.effectiveOctUp(), "D tap again turns it off");
    }

    std::printf ("Holds\n");
    {
        auto before = r.snapshot();
        r.holdSwitch (20, 600);
        check (r.proc.effectiveBypass(), "A hold bypasses while held (the Flexi-Switch)");
        r.releaseSwitch (20);
        check (! r.proc.effectiveBypass() && r.changedSince (before).isEmpty(), "letting go of A brings it back, nothing saved");

        r.tapSwitch (20);   // bypassed, then hold plays it
        r.holdSwitch (20, 600);
        check (! r.proc.effectiveBypass(), "A hold from bypass plays Bitrot while held");
        r.releaseSwitch (20);
        check (r.proc.effectiveBypass(), "letting go bypasses again");
        r.tapSwitch (20);

        before = r.snapshot();
        const int s0 = (int) r.value (subInterval);
        r.holdSwitch (21, 600); r.releaseSwitch (21);
        check ((int) r.value (subInterval) == (s0 + 7) % 8, "B hold steps the subharmonic back");
        check (only (r.changedSince (before), { subInterval }), "B hold changes only the subharmonic");
        r.tapSwitch (21);

        before = r.snapshot();
        r.holdSwitch (22, 600);
        check (r.proc.getEngine().uiFrozen.load(), "C hold freezes while held");
        const float frozenHz = r.proc.getEngine().uiOscHz.load();
        r.releaseSwitch (22);
        check (! r.proc.getEngine().uiFrozen.load() && r.changedSince (before).isEmpty(), "letting go of C unfreezes, freeze latch untouched");
        check (frozenHz > 150.0f && frozenHz < 180.0f, "the frozen note is the one playing (" + String (frozenHz, 1) + " Hz, +1 oct of low E)");

        r.holdSwitch (23, 600);
        check (r.proc.effectiveOctUp(), "D hold: octave up while held");
        r.releaseSwitch (23);
        check (! r.proc.effectiveOctUp() && r.value (octUp) < 0.5f, "letting go of D drops back, latch untouched");
    }

    std::printf ("Combos\n");
    {
        auto before = r.snapshot();
        r.cc (20, 127); r.pump (30); r.cc (21, 127); r.pump (80); r.cc (20, 0); r.cc (21, 0); r.pump (80);
        check (r.proc.selectedPreset() == 0, "A+B steps to the next preset");
        check (r.value (bypass) < 0.5f && ! r.proc.effectiveBypass(), "A+B doesn't bypass");

        before = r.snapshot();
        const int keeps = r.proc.uiKeepPulse.load();
        r.cc (22, 127); r.pump (30); r.cc (23, 127); r.pump (80); r.cc (22, 0); r.cc (23, 0); r.pump (80);
        check (r.changedSince (before).isEmpty() && r.proc.uiKeepPulse.load() == keeps, "C+D tap only hands the footswitches on");
        check (! r.proc.effectiveOctUp() && ! r.proc.getEngine().uiFrozen.load(), "C+D tap leaves no freeze or octave behind");

        r.cc (23, 127); r.pump (30); r.cc (22, 127); r.pump (1250);
        check (r.proc.uiKeepPulse.load() == keeps + 1, "C+D held 1 second saves a Keeper");
        check (! r.proc.effectiveOctUp() && ! r.proc.getEngine().uiFrozen.load(), "C+D hold never fires freeze or octave up");
        r.cc (22, 0); r.cc (23, 0); r.pump (80);
        check (r.changedSince (before).isEmpty(), "C+D hold changes no settings");
    }

    std::printf ("Expression\n");
    {
        auto before = r.snapshot();
        r.cc (11, 0); r.pump (40);
        check ((int) r.value (oscInterval) == 0, "heel: unison");
        r.cc (11, 127); r.pump (40);
        check ((int) r.value (oscInterval) == 7, "toe: +3 octaves");
        r.cc (11, 64); r.pump (40);
        const int mid = (int) r.value (oscInterval);
        check (mid == 4 || mid == 3, "half way: the middle of the harmonic series");
        r.cc (11, 65); r.pump (20); r.cc (11, 63); r.pump (20); r.cc (11, 64); r.pump (20);
        check ((int) r.value (oscInterval) == mid, "a pedal resting on a step boundary doesn't flicker");
        check (only (r.changedSince (before), { oscInterval }), "expression moves only the interval");
    }

    std::printf ("Clashes\n");
    {
        auto before = r.snapshot();
        for (int ch : { 1, 2, 15 })
            for (int number : { 20, 21, 22, 23, 11, 1, 64 })
                r.cc (number, 127, ch), r.cc (number, 0, ch);
        r.pump (100);
        check (r.changedSince (before).isEmpty(), "CC 20 to 23, 11, 1 and 64 on other channels change nothing");
    }

    std::printf (failures == 0 ? "All passed\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
