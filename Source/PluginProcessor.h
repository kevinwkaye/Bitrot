#pragma once

#include "Params.h"
#include "PedalControl.h"
#include "Keeper.h"
#include "engine/BitrotEngine.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>

class BitrotProcessor : public juce::AudioProcessor,
                        private juce::Timer,
                        private juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr int kPresets = 16;          // red, yellow, green and blue banks of four
    static constexpr int kPerBank = 4;

    BitrotProcessor();
    ~BitrotProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Bitrot"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.2; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    bit::BitrotEngine& getEngine() { return engine; }

    // ---- called from the panel (message thread) ----
    void footDown (int sw);
    void footUp (int sw);
    void stepSub (int direction);
    void stepOsc (int direction);

    void savePreset (int slot);
    void loadPreset (int slot);
    bool hasPreset (int slot) const;
    void stepPreset (int direction);
    int selectedPreset() const { return selected; }

    // footswitch and expression setup
    PedalSettings& pedals() { return settings; }
    void savePedals();
    void learnFootswitch (int sw);
    void learnExpression();
    void calibrateExpression();
    void resetFootswitches();

    // Keeper: log this moment, the last 8 seconds of audio included
    void keepNow();
    juce::File exportKeepers();
    void openKeepersFolder();
    void setKeeperLive (bool live);

    // footswitch MIDI read straight from the device, so every copy works without a MIDI track
    void handleDirectMidi (const juce::MidiMessage&);   // MIDI thread
    bool pedalsDirect() const { return direct->connected(); }
    juce::String footInputStatus() const { return direct->status(); }
    juce::String footDevice() const { return settings.footDevice; }
    void setFootDevice (const juce::String& device);
    juce::StringArray footDeviceNames() const { return DirectMidiInput::deviceNames(); }
    std::atomic<bool> hostFeedsPedals { false };
    bool isLearning() const { return learnTarget != LearnNone; }
    int learningWhat() const { return learnTarget; }
    bool isCalibrating() const { return calibrating; }
    enum { LearnNone = -1, LearnExpression = 4 };

    // momentary footswitch jobs, also on screen
    bool bypassFlipped() const { return bypassFlip.load(); }
    bool freezeHeld() const { return freezeHold.load(); }
    bool octHeld() const { return octHold.load(); }
    bool effectiveBypass() const { return on (ParamIDs::bypass) != bypassFlip.load(); }
    bool effectiveFreeze() const { return on (ParamIDs::freeze) || freezeHold.load(); }
    bool effectiveOctUp() const { return on (ParamIDs::octUp) || octHold.load(); }

    // ---- what the panel and Pebble show ----
    float expressionLevel() const { return uiExp.load(); }
    double lastExpressionMove() const { return expMovedAt; }
    std::atomic<int> uiFootPulse { 0 };       // counts footswitch presses, Pebble taps its foot
    std::atomic<int> uiFootDown { 0 };        // bitmask of footswitches held now
    std::atomic<int> uiKeepPulse { 0 };
    juce::String uiMessage;                   // a short line for the footswitch status
    double uiMessageAt = 0.0;
    int lastFootSwitch() const { return lastFoot; }

private:
    void timerCallback() override;
    void parameterChanged (const juce::String& id, float value) override;
    bit::EngineParams readParams();
    void handleMidi (const juce::MidiMessage&, bool fromDevice);
    void onFootEvent (int sw, FootswitchLogic::Event);
    void setNorm (const juce::String& id, float v01);
    void setPlain (const juce::String& id, float plain);
    void toggle (const char* id);
    void say (const juce::String& s);
    void applyExpression (float v);

    juce::ValueTree snapshotOf();
    void applySnapshot (const juce::ValueTree&);
    static bool inSnapshot (const juce::String& id);
    void makeFactoryPresets();

    bit::BitrotEngine engine;
    Keeper keeper;

    std::map<juce::String, std::atomic<float>*> raw;
    float f (const char* id) const { return raw.at (id)->load (std::memory_order_relaxed); }
    bool on (const char* id) const { return f (id) > 0.5f; }
    int idx (const char* id) const { return (int) std::lround (f (id)); }

    // MIDI handed to the message thread
    struct Pending { int kind = 0; int a = 0; float v = 0; };
    enum PendingKind { FootPress, FootRelease, Expression, LoadPreset, Learned };
    juce::AbstractFifo fifo { 512 };
    std::array<Pending, 512> pending;
    juce::AbstractFifo directFifo { 512 };    // the same, written from the MIDI thread
    std::array<Pending, 512> directPending;
    juce::SpinLock directWriteLock;
    juce::SharedResourcePointer<DirectMidiInput> direct;
    juce::Time settingsStamp;
    bool wasDirect = false;
    double nextSettingsCheck = 0.0;
    void queue (int kind, int a, float v);

    std::array<juce::ValueTree, kPresets> slots;
    int selected = -1;

    PedalSettings settings;
    FootswitchLogic footswitches;
    std::atomic<int> learnTarget { LearnNone };
    bool calibrating = false;
    double calibrateUntil = 0.0;
    int calLo = 127, calHi = 0, calFirst = -1;
    double expMovedAt = -100.0;
    int lastFoot = -1;
    bool loadingState = false;

    // footswitch C: freeze waits out the combo window, then freezes the pitch from the press
    double cDownAt = -1.0;
    bool freezePending = false;
    std::atomic<bool> bypassFlip { false }, freezeHold { false }, octHold { false };
    std::atomic<float> freezeBackMs { 0.0f };
    std::atomic<float> uiExp { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BitrotProcessor)
};
