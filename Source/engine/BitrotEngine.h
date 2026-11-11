#pragma once

// Bitrot engine: a sample-by-sample model of a CMOS phase-locked loop harmoniser.
// Plain C++17 with no JUCE, so the engine and its render test build anywhere.
//
//   input -> square-up (comparator with hysteresis, gated)         -> SQUARE voice
//         -> Root divider (1, 2 or 4) -> phase-frequency detector <- VCO divided by the interval
//                                         -> loop filter -> VCO
//   VCO control -> glide or vibrato -> output oscillator             -> OSCILLATOR voice
//   square or oscillator edges -> divide by 2 to 9                   -> SUBHARMONIC voice
//
// Every voice is a square wave built from edges with sub-sample timing (two-point BLEP),
// so they stay clean at high intervals. Lock assist borrows the low-register tracker:
// it tells the loop where the note is so it locks in about a cycle and a half. Corrupt
// takes that help away and lets the raw loop hunt, chirp and stutter.

#include "DspUtil.h"
#include "Tracker.h"
#include <array>
#include <atomic>

namespace bit
{
struct EngineParams
{
    // voice mixer
    float square = 0.5f, sub = 0.5f, osc = 0.6f, dry = 0.0f, level = 0.6f;
    // subharmonic
    int subInterval = 0;     // 0..7: divide by 2..9
    int subRoot = 0;         // 0 unison (the square), 1 oscillator
    bool floor = false;      // lift subs below floorHz by octaves
    int floorHz = 1;         // 0: 25 Hz, 1: 30 Hz, 2: 40 Hz
    int subMix = 0;          // oscillator root: 0 sum and difference, 1 plain divide
    // frequency modulator
    int fmMode = 0;          // 0 glide, 1 vibrato
    float rate = 0.6f, depth = 0.4f;
    // master oscillator
    int oscInterval = 1;     // 0..7: harmonic 1..8
    int oscRoot = 0;         // 0 unison, 1 -1 oct, 2 -2 oct
    float bend = 0.0f;       // 0..1: up to three octaves, unstepped
    // tracking
    int input = 0;           // 0 guitar, 1 bass, 2 synth
    float corrupt = 0.3f;    // tight .. wild
    bool trackComp = true;
    float gate = 0.5f;       // sensitivity
    // performance
    bool bypass = false, freeze = false, octUp = false;
    float freezeBackMs = 0.0f;   // freeze the pitch from this long ago (the moment the switch went down)
};

// A square wave built from edges at fractional sample times, with a two-point BLEP.
// Output is one sample late so the sample before each edge can be corrected too.
struct EdgeSquare
{
    float level = -1.0f, held = -1.0f, cur = 0.0f;
    void reset (float l) { level = l; held = l; cur = 0.0f; }
    void begin() { cur = 0.0f; }
    // d: time since the edge, in samples, at the current sample point (0..1]
    void edge (float d)
    {
        d = std::min (1.0f, std::max (0.0f, d));
        const float h = -2.0f * level;
        held += h * 0.5f * d * d;
        cur -= h * 0.5f * (1.0f - d) * (1.0f - d);
        level = -level;
    }
    float end()
    {
        const float out = held;
        held = level + cur;
        return out;
    }
};

class BitrotEngine
{
public:
    // ---- what the panel, Pebble and Keeper read ----
    std::atomic<float> uiNote { -1.0f };      // input note (midi, with cents) or -1
    std::atomic<float> uiLock { -1.0f };      // 0 lost .. 1 locked, -1 when silent
    std::atomic<float> uiOscHz { 0.0f }, uiSubHz { 0.0f }, uiSubRawHz { 0.0f };
    std::atomic<float> uiLevel { 0.0f };
    std::atomic<int> uiOnsets { 0 };          // counts new notes
    std::atomic<int> uiLockLosses { 0 };      // counts the moments the loop lost hold
    std::atomic<bool> uiFrozen { false }, uiFloored { false }, uiGateOpen { false };
    std::atomic<float> uiLockMs { 0.0f };

    void prepare (double sampleRate)
    {
        sr = (float) std::max (8000.0, sampleRate);
        tracker.prepare (sr);
        hp.highpass (20.0f, 0.707f, sr); hp.reset();
        outDc.highpass (12.0f, 0.707f, sr); outDc.reset();
        subHp.highpass (25.0f, 0.6f, sr); subHp.reset();
        for (auto& b : lp) b.reset();
        lastLpHz = -1.0f;
        envA = std::exp (-1.0f / (0.0005f * sr)); envR = std::exp (-1.0f / (0.030f * sr));
        compA = std::exp (-1.0f / (0.002f * sr)); compR = std::exp (-1.0f / (0.080f * sr));
        env = compEnv = playLevel = 0.0f;
        playUp = 1.0f - std::exp (-1.0f / (0.010f * sr)); playDown = 1.0f - std::exp (-1.0f / (4.0f * sr));
        prevX = 0.0f; cmpHigh = false; gateOpen = false; gateHold = 0;
        sq.reset (-1.0f); oscSq.reset (-1.0f); subSq.reset (-1.0f);
        rootCount = 0; fbCount = 0; up = down = false;
        integ = std::log2 (110.0f); vcoPhase = 0.0f; outPhase = 0.0f;
        outCv = integ; glideCv = integ; slewCv = integ;
        subEdgeCount = 0; subState = false; subPhase = 0.0f; oscState = false;
        sinceRise = 0; srcPeriod = 0.0f; lastPeriods = { 0, 0, 0 };
        lfoPhase = 0.0f; amp = 0.0f; voiceAmp = 0.0f; bypassMix = 0.0f;
        lockErr = 1.0f; pulseSamples = 0; refPeriodSamples = 0; sinceRef = 0; lockedFlag = false;
        frozen = false; frozenCv = 0.0f; frozenSrc = 0.0f;
        hist.fill ({ 0.0f, 0.0f }); histPos = 0; histCount = 0;
        sqSmooth.setTime (0.01f, sr); subSmooth.setTime (0.01f, sr); oscSmooth.setTime (0.01f, sr);
        drySmooth.setTime (0.01f, sr); levelSmooth.setTime (0.01f, sr);
        noteOnAt = 0; sampleClock = 0;
        configured = false;
    }

    // stereo in, stereo out; the synth is mono and sits in the middle
    void process (float* L, float* R, int n, const EngineParams& p)
    {
        configure (p);
        const float c = corruptEff (p);
        const int N = p.oscInterval + 1;
        const float rootDiv = (float) (1 << std::max (0, std::min (2, p.oscRoot)));
        const int D = p.subInterval + 2;
        const float floorHz = p.floorHz == 0 ? 25.0f : (p.floorHz == 2 ? 40.0f : 30.0f);

        // loop gains: a charge-pump PLL with a proportional kick for damping
        // Per reference cycle: tight settles in two or three cycles; wild rings and hunts.
        const float kp = 0.95f - 0.5f * c;        // octaves while a phase pulse is on
        const float ki = 0.70f - 0.55f * c;       // per unit phase error
        const float assist = clamp01 (1.0f - c / 0.66f);
        const float pull = assist * assist * 120.0f / sr;
        const float snapOct = 0.04f + 0.4f * c * c;
        const float spike = c > 0.5f ? 0.0f : 0.00025f * sr;   // smooth the proportional kick in tight

        // frequency modulator
        // Glide is a slide at a steady number of octaves per second, a straight line on a
        // keyboard, as the pedal's recordings show: Rate runs from 0.8 oct/s (a slow swoop)
        // to 200 oct/s (a jump). A 1 ms smoother takes the edge off the steps.
        const float slewOctPerSec = p.fmMode == 0 ? glideSlew (p.rate) : 400.0f;
        const float slewPerSample = slewOctPerSec / sr;
        const float glideCoef = std::exp (-1.0f / (0.001f * sr));
        const float spikeCoef = spike > 0.0f ? std::exp (-1.0f / spike) : 0.0f;
        const float lfoHz = 0.5f * std::pow (28.0f, p.rate);
        const float vibOct = p.fmMode == 1 ? (0.1f + p.depth * p.depth * 5.9f) / 12.0f : 0.0f;
        const float extraCv = (p.octUp ? 1.0f : 0.0f) + 3.0f * clamp01 (p.bend);

        // gate, as the pedal: weak signals stop the synth
        // The gate follows how loud you play rather than sitting at a fixed level, so a quiet
        // interface input tracks as well as a hot one. It opens 24 to 40 dB under your playing
        // level (Gate sensitivity), never lower than just above amp hiss.
        const float gFloor = 0.0004f + 0.004f * (1.0f - p.gate) * (1.0f - p.gate);
        const float gRel = std::pow (10.0f, -(24.0f + 16.0f * p.gate) / 20.0f);
        const float g = std::max (gFloor, playLevel * gRel);
        const float hyst = 0.10f - 0.08f * c;

        // freeze: hold the oscillator and sub on the note from the moment the switch went down
        if (p.freeze && ! frozen)
        {
            frozen = true;
            const auto h = historyAt (p.freezeBackMs);
            frozenCv = h.cv; frozenSrc = h.src;
        }
        if (! p.freeze) frozen = false;
        uiFrozen.store (frozen);

        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            ++sampleClock;
            const float inL = L[i], inR = R[i];
            const float raw = hp.process (0.5f * (inL + inR));

            // ---- lock assist: the tracker hears the note first ----
            const auto ev = tracker.push (raw);   // the tracker levels its own input
            if (ev == MonoTracker::Event::NoteOn || ev == MonoTracker::Event::NoteChange)
            {
                const float tHz = mtof (tracker.currentNote());
                if (assist > 0.0f)
                {
                    const float target = std::log2 (tHz * (float) N / rootDiv);
                    if (std::abs (target - integ) > 0.25f) { integ = target; up = down = false; }
                    resync = true; lockErr = std::max (lockErr, 0.5f);
                }
                if (ev == MonoTracker::Event::NoteOn) uiLockMs.store (tracker.lockMs());
            }
            // the tracker keeps its last note while it re-checks a decaying string
            const bool heardNote = tracker.currentNote() > 0.0f && gateOpen;
            if (assist > 0.0f && heardNote)
            {
                const float target = std::log2 (mtof (tracker.currentNote()) * (float) N / rootDiv);
                const float diff = target - integ;
                if (std::abs (diff) > snapOct) { integ = target; up = down = false; resync = true; lockErr = std::max (lockErr, 0.5f); }
                else if (std::abs (diff) > kDeadband) integ += pull * (diff - std::copysign (kDeadband, diff));   // the loop owns the last few cents
            }

            // ---- envelope and gate ----
            const float ax = std::abs (raw);
            env = ax > env ? ax + envA * (env - ax) : ax + envR * (env - ax);
            // your playing level: follows loud playing in about 10 ms, lets go over seconds
            playLevel += (env - playLevel) * (env > playLevel ? playUp : playDown);
            const bool wasOpen = gateOpen;
            if (! gateOpen && env > g) gateOpen = true;
            else if (gateOpen && env < g * 0.6f) gateOpen = false;
            if (gateOpen && ! wasOpen) { uiOnsets.fetch_add (1); noteOnAt = sampleClock; }

            // ---- square-up: compressor for the tracking path, fundamental emphasis, comparator ----
            float x = lp[1].process (lp[0].process (raw));
            if (assisting) x = bp.process (x) * 2.0f;
            const float afx = std::abs (x);
            compEnv = afx > compEnv ? afx + compA * (compEnv - afx) : afx + compR * (compEnv - afx);
            if (p.trackComp) x = x / std::max (compEnv, g * 0.05f) * 0.5f;   // what the comparator sees, levelled
            else x *= 8.0f;
            const float th = hyst * (p.trackComp ? 0.5f : std::max (compEnv * 8.0f, g * 2.0f));

            sq.begin();
            bool rise = false, fall = false; float edgeD = 0.0f;
            if (gateOpen)
            {
                if (! cmpHigh && x > th && prevX <= th)
                {
                    rise = true;
                    edgeD = 1.0f - (th - prevX) / std::max (1.0e-9f, x - prevX);
                }
                else if (cmpHigh && x < -th && prevX >= -th)
                {
                    fall = true;
                    edgeD = 1.0f - (-th - prevX) / std::min (-1.0e-9f, x - prevX);
                }
                // wild: near the end of a note the comparator drops or doubles edges
                if ((rise || fall) && c > 0.6f && env < g * 4.0f && rng.uni() < (c - 0.6f) * 0.25f)
                {
                    if (rng.uni() < 0.5f) rise = fall = false;
                }
            }
            prevX = x;
            if (rise || fall)
            {
                cmpHigh = rise;
                sq.edge (edgeD);
                if (rise)
                {
                    // the input's own period, for the floor and the readout
                    if (sinceRise > 0)
                    {
                        lastPeriods[2] = lastPeriods[1]; lastPeriods[1] = lastPeriods[0]; lastPeriods[0] = sinceRise;
                        const int a = lastPeriods[0], b = lastPeriods[1], d3 = lastPeriods[2];
                        const int med = std::max (std::min (a, b), std::min (std::max (a, b), d3));
                        if (med > 0) srcPeriod = (float) med;
                    }
                    sinceRise = 0;
                }
            }
            ++sinceRise;
            const float sqOut = sq.end();

            // ---- PLL: Root divider, phase-frequency detector, loop filter, VCO ----
            bool refEdge = false;
            if (rise && ++rootCount >= (int) rootDiv) { rootCount = 0; refEdge = true; }
            const float ePrev = (up ? 1.0f : 0.0f) - (down ? 1.0f : 0.0f);
            const float fVco = std::exp2 (integ + kp * ePrev);   // the proportional kick damps the loop
            const float fRef = std::exp2 (integ) / (float) N;
            vcoPhase += fVco / sr;
            bool fbEdge = false;
            if (vcoPhase >= 1.0f)
            {
                vcoPhase -= std::floor (vcoPhase);
                if (++fbCount >= N) { fbCount = 0; fbEdge = true; }
            }
            if (refEdge) ++dbgRef;
            if (fbEdge) ++dbgFb;
            if (refEdge)
            {
                // lock quality: how much of the last reference period the detector was pulsing
                if (sinceRef > 0) refPeriodSamples = sinceRef;
                const float q = refPeriodSamples > 0 ? (float) pulseSamples / (float) refPeriodSamples : 1.0f;
                lockErr += 0.25f * (std::min (1.0f, q) - lockErr);
                pulseSamples = 0; sinceRef = 0;
                if (resync)
                {
                    // lock assist: line the loop's phase up with this edge instead of waiting cycles for it
                    resync = false;
                    vcoPhase = 0.0f; fbCount = 0; up = down = false;
                }
                else if (down) down = false; else up = true;
            }
            ++sinceRef;
            if (fbEdge) { if (up) up = false; else down = true; }
            const float e = (up ? 1.0f : 0.0f) - (down ? 1.0f : 0.0f);
            if (e != 0.0f) ++pulseSamples;
            integ += ki * fRef * e / sr;
            integ = std::max (kMinCv, std::min (std::log2 (sr * 0.45f), integ));
            // nothing to compare against: tight holds the pitch, wild lets the loop dive
            if (! gateOpen && c <= 0.5f) up = down = false;
            // the control the output oscillator hears: clean in tight, with the loop's ripple in wild
            const float target = integ + kp * e * c * c;
            spikeCv = spikeCoef > 0.0f ? target + spikeCoef * (spikeCv - target) : target;

            // ---- frequency modulator and the output oscillator ----
            // tight: while the loop settles its phase, the oscillator leans on the tracked pitch
            float heard = spikeCv;
            if (assisting && heardNote)
            {
                const float tCv = std::log2 (mtof (tracker.currentNote()) * (float) N / rootDiv);
                const float settled = clamp01 (1.0f - (lockErr - 0.04f) * 4.0f);
                if (std::abs (tCv - integ) < 0.5f) heard += (tCv - spikeCv) * std::sqrt (assist) * (1.0f - settled);
            }
            slewCv += std::max (-slewPerSample, std::min (slewPerSample, heard - slewCv));
            glideCv = slewCv + glideCoef * (glideCv - slewCv);
            float baseCv = glideCv;
            if (frozen) baseCv = frozenCv;
            if (++histTick >= kHistHop) { histTick = 0; pushHistory (glideCv, currentSrcHz (p, N, rootDiv)); }
            lfoPhase += lfoHz / sr; if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
            const float tri = lfoPhase < 0.5f ? 4.0f * lfoPhase - 1.0f : 3.0f - 4.0f * lfoPhase;
            outCv = baseCv + vibOct * tri + extraCv;
            const float fOut = std::min (sr * 0.45f, std::exp2 (std::max (kMinCv, outCv)));
            const float inc = fOut / sr;

            oscSq.begin(); subSq.begin();
            const float before = outPhase;
            outPhase += inc;
            bool oscEdge = false; float oscD = 0.0f;
            if (before < 0.5f && outPhase >= 0.5f) { oscEdge = true; oscD = (outPhase - 0.5f) / inc; }
            else if (outPhase >= 1.0f) { outPhase -= 1.0f; oscEdge = true; oscD = outPhase / inc; }
            if (oscEdge) { oscSq.edge (oscD); oscState = ! oscState; }

            // ---- subharmonic: divide the square or the oscillator ----
            const bool fromOsc = p.subRoot == 1;
            const bool xorMix = fromOsc && p.subMix == 0;
            const float srcHz = frozen && ! fromOsc ? frozenSrc : (fromOsc ? fOut : currentSrcHz (p, N, rootDiv));
            const float subRaw = srcHz / (float) D;
            float subHz = subRaw;
            bool floored = false;
            if (p.floor && subRaw > 1.0f) while (subHz < floorHz) { subHz *= 2.0f; floored = true; }
            const bool useOscillator = floored || (frozen && ! fromOsc);
            if (useOscillator)
            {
                // a steady oscillator at the lifted pitch (or the frozen one)
                const float sInc = std::min (0.45f, subHz / sr);
                const float b0 = subPhase;
                subPhase += sInc;
                if (b0 < 0.5f && subPhase >= 0.5f) { subSq.edge ((subPhase - 0.5f) / sInc); subState = ! subState; }
                else if (subPhase >= 1.0f) { subPhase -= 1.0f; subSq.edge (subPhase / sInc); subState = ! subState; }
            }
            else
            {
                // true edge counting: missed or doubled edges stutter, as on the pedal
                const bool srcEdge = fromOsc ? oscEdge : (rise || fall);
                const float d = fromOsc ? oscD : edgeD;
                if (srcEdge && ++subEdgeCount >= D) { subEdgeCount = 0; subSq.edge (d); subState = ! subState; }
            }
            if (xorMix && oscEdge) subSq.edge (oscD);   // sum and difference: the oscillator toggles the sub too
            const float oscOut = oscSq.end();
            const float subOut = subHp.process (subSq.end());

            // ---- levels: full level at once, a sudden stop, as the pedal ----
            const bool sounding = gateOpen;
            const float ampTarget = sounding ? 1.0f : 0.0f;
            amp += (ampTarget - amp) * (ampTarget > amp ? 0.2f : 0.008f);
            // the oscillator and sub hang on a little in wild, so you hear the loop dive
            if (sounding) gateHold = (int) (c * c * 0.12f * sr);
            else if (gateHold > 0) --gateHold;
            const float vTarget = frozen ? 1.0f : (sounding || gateHold > 0 ? 1.0f : 0.0f);
            voiceAmp += (vTarget - voiceAmp) * (vTarget > voiceAmp ? 0.2f : 0.008f);

            const float kSq = sqSmooth.process (p.square * p.square);
            const float kSub = subSmooth.process (p.sub * p.sub);
            const float kOsc = oscSmooth.process (p.osc * p.osc);
            const float kDry = drySmooth.process (p.dry);
            const float kLvl = levelSmooth.process (2.0f * p.level * p.level);

            float wet = 0.42f * (kSq * sqOut * amp + kSub * subOut * voiceAmp + kOsc * oscOut * voiceAmp);
            wet = outDc.process (wet) * kLvl;
            wet = softClip (wet * 1.1f) * 0.9f;

            bypassMix += ((p.bypass ? 1.0f : 0.0f) - bypassMix) * 0.0025f;
            const float oL = (wet + kDry * inL) * (1.0f - bypassMix) + inL * bypassMix;
            const float oR = (wet + kDry * inR) * (1.0f - bypassMix) + inR * bypassMix;
            L[i] = std::isfinite (oL) ? oL : 0.0f;
            R[i] = std::isfinite (oR) ? oR : 0.0f;
            peak = std::max (peak, std::abs (wet));

            if (i == n - 1)
            {
                uiOscHz.store (fOut);
                uiSubHz.store (subHz);
                uiSubRawHz.store (subRaw);
                uiFloored.store (floored);
            }
        }

        // ---- readouts ----
        uiGateOpen.store (gateOpen);
        uiLevel.store (peak);
        const float tn = tracker.currentNote();
        if (tn > 0.0f) uiNote.store (tn);
        else if (gateOpen && srcPeriod > 0.0f) uiNote.store (ftom (sr / srcPeriod));
        else uiNote.store (-1.0f);
        const float lock = clamp01 (1.0f - (lockErr - 0.04f) * 4.0f);
        const bool nowLocked = lock > 0.5f;
        if (gateOpen && lockedFlag && ! nowLocked) uiLockLosses.fetch_add (1);
        lockedFlag = nowLocked;
        uiLock.store (gateOpen ? lock : -1.0f);
    }

    static float glideSlew (float rate) { return 0.8f * std::pow (250.0f, clamp01 (rate)); }   // octaves per second

    // test hooks
    int dbgRef = 0, dbgFb = 0;
    bool trackerActive() const { return tracker.active(); }
    bool isAssisting() const { return assisting; }
    float oscillatorHz() const { return std::exp2 (std::max (kMinCv, outCv)); }
    float loopHz() const { return std::exp2 (integ); }
    bool gate() const { return gateOpen; }
    float lockQuality() const { return clamp01 (1.0f - (lockErr - 0.04f) * 4.0f); }

private:
    static constexpr float kMinCv = 3.0f;   // 8 Hz
    static constexpr float kDeadband = 0.0125f;   // 15 cents
    static constexpr int kHistHop = 32, kHist = 2048;
    struct Hist { float cv, src; };

    float corruptEff (const EngineParams& p) const
    {
        // bass leans tight, guitar sits where the knob says
        return clamp01 (p.input == 1 ? p.corrupt * 0.6f : p.corrupt);
    }

    void configure (const EngineParams& p)
    {
        const float c = corruptEff (p);
        MonoTracker::Settings s;
        s.range = p.input == 1 ? MonoTracker::Bass : (p.input == 2 ? MonoTracker::Synth : MonoTracker::Guitar);
        s.lowestHz = 28.0f;
        s.sensitivity = p.gate;
        s.character = c * 0.6f;
        s.noteHold = 0.5f;
        s.octaveGuard = true;
        if (! configured || s.range != lastRange || std::abs (s.character - lastChar) > 0.01f || std::abs (s.sensitivity - lastSens) > 0.01f)
        {
            tracker.configure (s);
            lastRange = s.range; lastChar = s.character; lastSens = s.sensitivity;
            configured = true;
        }
        // fundamental emphasis before the comparator; wild lets harmonics through, so the loop grabs them.
        // With lock assist, the filter follows the tracked note so the comparator sees the fundamental.
        const float base = p.input == 1 ? 260.0f : (p.input == 2 ? 2200.0f : 900.0f);
        float lpHz = base * (1.0f + 3.0f * c);
        const bool wantAssist = c < 0.66f && tracker.currentNote() > 0.0f;
        float bpHz = 0.0f;
        if (wantAssist)
        {
            bpHz = mtof (tracker.currentNote());
            lpHz = std::min (lpHz, std::max (30.0f, bpHz * 1.35f));
        }
        if (std::abs (lpHz - lastLpHz) > lastLpHz * 0.01f)
        {
            lastLpHz = lpHz;
            lp[0].lowpass (lpHz, 0.54f, sr);
            lp[1].lowpass (lpHz, 1.31f, sr);
        }
        if (wantAssist && std::abs (bpHz - lastBpHz) > lastBpHz * 0.005f) { lastBpHz = bpHz; bp.bandpass (bpHz, 1.4f, sr); }
        assisting = wantAssist;
    }

    float currentSrcHz (const EngineParams& p, int N, float rootDiv) const
    {
        // the square's own pitch: the tracker when it's sure, else the comparator's period
        if (tracker.active() && corruptEff (p) < 0.66f) return mtof (tracker.currentNote());
        if (srcPeriod > 0.0f) return sr / srcPeriod;
        return std::exp2 (integ) / (float) N * rootDiv;
    }

    void pushHistory (float cv, float src)
    {
        hist[(size_t) histPos] = { cv, src };
        histPos = (histPos + 1) % kHist;
        histCount = std::min (kHist, histCount + 1);
    }

    Hist historyAt (float msAgo) const
    {
        if (histCount == 0) return { glideCv, 110.0f };
        const int back = std::min (histCount - 1, std::max (0, (int) (msAgo * 0.001f * sr / (float) kHistHop)));
        return hist[(size_t) ((histPos - 1 - back + kHist * 2) % kHist)];
    }

    float sr = 48000.0f;
    MonoTracker tracker;
    bool configured = false;
    int lastRange = -1; float lastChar = -1.0f, lastSens = -1.0f, lastLpHz = -1.0f;
    Biquad hp, lp[2], bp, outDc, subHp;
    float lastBpHz = 1.0f; bool assisting = false, resync = false;
    float env = 0, envA = 0, envR = 0, compEnv = 0, compA = 0, compR = 0, playLevel = 0, playUp = 0, playDown = 0;
    float prevX = 0; bool cmpHigh = false, gateOpen = false; int gateHold = 0;
    EdgeSquare sq, oscSq, subSq;
    int rootCount = 0, fbCount = 0; bool up = false, down = false;
    float integ = 6.8f, vcoPhase = 0, spikeCv = 6.8f, glideCv = 6.8f, slewCv = 6.8f, outCv = 6.8f, outPhase = 0;
    int subEdgeCount = 0; bool subState = false, oscState = false; float subPhase = 0;
    int sinceRise = 0; float srcPeriod = 0; std::array<int, 3> lastPeriods {};
    float lfoPhase = 0, amp = 0, voiceAmp = 0, bypassMix = 0;
    float lockErr = 1.0f; int pulseSamples = 0, refPeriodSamples = 0, sinceRef = 0; bool lockedFlag = false;
    bool frozen = false; float frozenCv = 0, frozenSrc = 0;
    std::array<Hist, kHist> hist {}; int histPos = 0, histCount = 0, histTick = 0;
    Smooth sqSmooth, subSmooth, oscSmooth, drySmooth, levelSmooth;
    long long noteOnAt = 0, sampleClock = 0;
    Rng rng;
};
} // namespace bit
