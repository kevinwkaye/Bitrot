// Bitrot engine render test. No JUCE needed: builds and runs on Windows and Linux ARM64.
//
// Plucked bass from low B and guitar up to E5 (with the fundamental weakened, the way a
// neck-pickup bass or a wound string sounds), every interval and root, the subharmonic
// dividers and floor, freeze, octave up, glide, vibrato, the wild loop and a random sweep.
// Fails on slow locks, wrong pitches, mud below the floor, NaNs, runaway levels or noise
// in silence. Pass a folder to write WAV files of each render.

#include "BitrotEngine.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>

using namespace bit;

static int failures = 0;
static void check (bool ok, const std::string& what)
{
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (! ok) ++failures;
}
static std::string fmt (const char* f, double a, double b = 0, double c = 0, double d = 0)
{
    char buf[256]; std::snprintf (buf, sizeof buf, f, a, b, c, d); return buf;
}

static const char* kNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static std::string noteName (int m) { return std::string (kNames[m % 12]) + std::to_string (m / 12 - 1); }
static std::string outDir;
constexpr float kSr = 48000.0f;

// Karplus-Strong pluck; weakFund > 0 high-passes the fundamental away
static std::vector<float> pluck (float hz, float secs, float weakFund, uint32_t seed, float gain = 0.3f)
{
    const int n = (int) (secs * kSr), len = std::max (2, (int) std::lround (kSr / hz));
    std::vector<float> out ((size_t) n), d ((size_t) len);
    Rng r; r.s = seed;
    for (auto& v : d) v = r.bi();
    Biquad hp; hp.highpass (hz * std::max (0.3f, weakFund), 0.7f, kSr);
    int idx = 0;
    for (int i = 0; i < n; ++i)
    {
        const float v = d[(size_t) idx];
        const int j = (idx + 1) % len;
        d[(size_t) idx] = 0.5f * (d[(size_t) idx] + d[(size_t) j]) * 0.9985f;
        idx = j;
        out[(size_t) i] = hp.process (v) * gain;
    }
    return out;
}
// A plucked tone with an exact pitch: a short noise burst, then twelve harmonics that
// decay faster the higher they are. weak scales the fundamental down, the way a neck
// pickup or a P Bass puts more into the second harmonic than the first.
static std::vector<float> stringTone (float hz, float secs, float weak, uint32_t seed, float gain = 0.3f)
{
    const int n = (int) (secs * kSr);
    std::vector<float> out ((size_t) n);
    Rng r; r.s = seed;
    float ph[12];
    for (auto& v : ph) v = r.uni();
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / kSr;
        float v = 0.0f;
        for (int h = 1; h <= 12; ++h)
        {
            if (hz * (float) h > kSr * 0.45f) break;
            float a = 1.0f / (float) h;
            if (h == 1) a *= weak;
            v += a * std::exp (-t * (1.5f + 0.8f * (float) h)) * std::sin (6.2831853f * (hz * (float) h * t + ph[h - 1]));
        }
        if (t < 0.006f) v += r.bi() * 0.6f * (1.0f - t / 0.006f);
        out[(size_t) i] = v * gain;
    }
    return out;
}

// the pitch a plucked string really plays, measured by autocorrelation in its steady middle
static float measuredHz (const std::vector<float>& x, float approx)
{
    const size_t a = (size_t) (0.25f * kSr), n = (size_t) (0.25f * kSr);
    auto acf = [&] (float lag)
    {
        const int l0 = (int) lag; const float fr = lag - (float) l0;
        double s = 0.0;
        for (size_t i = a; i < a + n && i + (size_t) l0 + 1 < x.size(); ++i)
            s += (double) x[i] * ((1.0f - fr) * x[i + (size_t) l0] + fr * x[i + (size_t) l0 + 1]);
        return s;
    };
    float best = kSr / approx, bv = -1.0e30f;
    for (float lag = kSr / approx * 0.97f; lag <= kSr / approx * 1.03f; lag += 0.02f)
    {
        const double v = acf (lag);
        if (v > bv) { bv = (float) v; best = lag; }
    }
    return kSr / best;
}

static void writeWav (const std::string& name, const std::vector<float>& l, const std::vector<float>& r)
{
    if (outDir.empty()) return;
    FILE* f = std::fopen ((outDir + "/" + name + ".wav").c_str(), "wb");
    if (f == nullptr) return;
    const uint32_t frames = (uint32_t) l.size(), dataBytes = frames * 4, riff = 36 + dataBytes;
    auto u32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto u16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (riff); std::fwrite ("WAVEfmt ", 1, 8, f);
    u32 (16); u16 (1); u16 (2); u32 ((uint32_t) kSr); u32 ((uint32_t) kSr * 4); u16 (4); u16 (16);
    std::fwrite ("data", 1, 4, f); u32 (dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (float s : { l[i], r[i] })
        {
            const int16_t v = (int16_t) std::lround (std::max (-1.0f, std::min (1.0f, s)) * 32767.0f);
            std::fwrite (&v, 2, 1, f);
        }
    std::fclose (f);
}

// Runs a signal through the engine in 64-sample blocks. onBlock can change the params.
struct Run
{
    std::vector<float> L, R;
    std::vector<float> oscHz;     // per block
    float peak = 0.0f;
    bool finite = true;
};
static Run render (BitrotEngine& e, EngineParams p, const std::vector<float>& in,
                   const std::function<void (int block, EngineParams&)>& onBlock = {})
{
    Run r;
    r.L = in; r.R = in;
    const int bs = 64;
    for (size_t i = 0; i + bs <= in.size(); i += bs)
    {
        if (onBlock) onBlock ((int) (i / bs), p);
        e.process (&r.L[i], &r.R[i], bs, p);
        r.oscHz.push_back (e.oscillatorHz());
        for (int k = 0; k < bs; ++k)
        {
            const float v = r.L[i + (size_t) k];
            if (! std::isfinite (v)) r.finite = false;
            r.peak = std::max (r.peak, std::abs (v));
        }
    }
    return r;
}

// pitch error statistics after lock, in cents
struct Lock { float ms = -1.0f, median = 999.0f, p95 = 999.0f; };
static Lock lockStats (const std::vector<float>& hz, float expect, float startMs, float endMs)
{
    Lock s;
    const float blockMs = 64.0f * 1000.0f / kSr;
    std::vector<float> errs;
    int inside = 0;
    for (size_t b = 0; b < hz.size(); ++b)
    {
        const float t = (float) b * blockMs;
        if (t < startMs) continue;
        const float c = std::abs (1200.0f * std::log2 (std::max (1.0f, hz[b]) / expect));
        if (s.ms < 0.0f)
        {
            // locked once it stays within 25 cents for 10 ms
            inside = c < 25.0f ? inside + 1 : 0;
            if ((float) inside * blockMs >= 10.0f) s.ms = t - startMs - 10.0f + blockMs;
            continue;
        }
        if (t < endMs) errs.push_back (c);
    }
    if (! errs.empty())
    {
        std::sort (errs.begin(), errs.end());
        s.median = errs[errs.size() / 2];
        s.p95 = errs[errs.size() * 95 / 100];
    }
    return s;
}

static std::vector<float> withLeadIn (const std::vector<float>& x, int lead = 2400)
{
    std::vector<float> s ((size_t) lead, 0.0f);
    s.insert (s.end(), x.begin(), x.end());
    return s;
}

static EngineParams oscOnly (int input)
{
    EngineParams p;
    p.input = input; p.square = 0.0f; p.sub = 0.0f; p.osc = 1.0f; p.level = 0.6f;
    p.fmMode = 0; p.rate = 1.0f;   // fastest glide: we're measuring the loop
    return p;
}

// ------------------------------------------------------------------------------------------
static void lockSuite()
{
    std::printf ("Locking on plucked notes\n");
    struct Range { const char* name; int input; std::vector<int> notes; float limitMs; };
    const Range ranges[] = {
        { "bass", 1, { 23, 24, 26, 28, 30, 31, 33, 35, 36, 38, 40, 43, 45, 47, 50, 52, 55 }, 75.0f },
        { "guitar", 0, { 40, 43, 45, 47, 50, 52, 55, 57, 59, 62, 64, 67, 69, 71, 74, 76 }, 60.0f },
    };
    for (const auto& rg : ranges)
    {
        int good = 0, total = 0; float worstMs = 0.0f, worstMed = 0.0f, slowest = 0.0f; std::string bad;
        for (int m : rg.notes)
            for (float weak : { 1.0f, 0.25f })
            {
                BitrotEngine e; e.prepare (kSr);
                auto p = oscOnly (rg.input);
                const auto x = stringTone (mtof ((float) m), 1.2f, weak, 1234u + (uint32_t) m);
                const float f = mtof ((float) m);
                const auto run = render (e, p, withLeadIn (x));
                const auto st = lockStats (run.oscHz, f * 2.0f, 50.0f, 900.0f);   // default interval: +1 oct
                // the tracker needs about a cycle and a half before anyone can know the note,
                // so the lowest notes get three and a half cycles
                const float limit = std::max (rg.limitMs, 3500.0f / f);
                const bool ok = st.ms >= 0.0f && st.ms <= limit && st.median < 8.0f && st.p95 < 40.0f;
                ++total; good += ok ? 1 : 0;
                worstMs = std::max (worstMs, st.ms < 0.0f ? 9999.0f : st.ms - limit + rg.limitMs);
                slowest = std::max (slowest, st.ms);
                worstMed = std::max (worstMed, st.median);
                if (! ok) bad += " " + noteName (m) + (weak < 1.0f ? "(weak)" : "") + fmt ("[%.0fms %.0f/%.0fc]", st.ms, st.median, st.p95);
            }
        check (good == total, std::string (rg.name) + ": " + std::to_string (good) + "/" + std::to_string (total) + " notes lock and hold" + (bad.empty() ? "" : " - " + bad));
        check (worstMs <= rg.limitMs, std::string (rg.name) + fmt (" slowest lock %.0f ms (limit %.0f, or 3.5 cycles below that), worst median error %.1f cents", slowest, rg.limitMs, worstMed));
    }

    // open E on bass, the number in the README
    {
        BitrotEngine e; e.prepare (kSr);
        const auto x = pluck (mtof (28.0f), 0.6f, 1.6f, 99u);
        const auto run = render (e, oscOnly (1), withLeadIn (x));
        const auto st = lockStats (run.oscHz, measuredHz (x, mtof (28.0f)) * 2.0f, 50.0f, 500.0f);
        check (st.ms >= 0.0f && st.ms < 60.0f, fmt ("bass open E locks in %.0f ms", st.ms));
    }
}

// ------------------------------------------------------------------------------------------
static void intervalSuite()
{
    std::printf ("Intervals and roots\n");
    const float f = 110.0f;
    std::vector<float> sine ((size_t) (0.8f * kSr));
    for (size_t i = 0; i < sine.size(); ++i) sine[i] = 0.25f * std::sin (6.2831853f * f * (float) i / kSr);
    int good = 0, total = 0; std::string bad;
    for (int root = 0; root < 3; ++root)
        for (int iv = 0; iv < 8; ++iv)
        {
            BitrotEngine e; e.prepare (kSr);
            auto p = oscOnly (0);
            p.oscInterval = iv; p.oscRoot = root;
            const auto run = render (e, p, withLeadIn (sine));
            const float expect = f * (float) (iv + 1) / (float) (1 << root);
            const auto st = lockStats (run.oscHz, expect, 50.0f, 800.0f);
            const bool ok = st.ms >= 0.0f && st.median < 10.0f;
            ++total; good += ok ? 1 : 0;
            if (! ok) bad += fmt (" x%.0f/%.0f[%.0fc]", iv + 1, 1 << root, st.median);
        }
    check (good == total, std::to_string (good) + "/" + std::to_string (total) + " interval and root settings land on input x N / root" + bad);

    // the oscillator voice really is at that pitch: count its cycles
    {
        BitrotEngine e; e.prepare (kSr);
        auto p = oscOnly (0); p.oscInterval = 2;   // +1 oct 5th: 330 Hz
        const auto run = render (e, p, withLeadIn (sine));
        int rises = 0;
        const size_t a = (size_t) (0.3f * kSr), b = (size_t) (0.7f * kSr);
        for (size_t i = a + 1; i < b; ++i) if (run.L[i - 1] <= 0.0f && run.L[i] > 0.0f) ++rises;
        const float measured = (float) rises / ((float) (b - a) / kSr);
        check (std::abs (measured - 330.0f) < 4.0f, fmt ("the oscillator voice plays %.1f Hz (expect 330)", measured));
        writeWav ("interval_x3", run.L, run.R);
    }

    // octave up doubles, bend sweeps further
    {
        BitrotEngine e; e.prepare (kSr);
        auto p = oscOnly (0); p.oscInterval = 2; p.octUp = true;
        const auto run = render (e, p, withLeadIn (sine));
        const auto st = lockStats (run.oscHz, 660.0f, 50.0f, 800.0f);
        check (st.median < 6.0f, fmt ("octave up: x3 becomes x6 (median error %.1f cents)", st.median));
    }
}

// ------------------------------------------------------------------------------------------
static void subSuite()
{
    std::printf ("Subharmonic\n");
    const float f = 110.0f;
    std::vector<float> sine ((size_t) (0.8f * kSr));
    for (size_t i = 0; i < sine.size(); ++i) sine[i] = 0.25f * std::sin (6.2831853f * f * (float) i / kSr);
    int good = 0; std::string bad;
    for (int d = 0; d < 8; ++d)
    {
        BitrotEngine e; e.prepare (kSr);
        EngineParams p; p.square = 0; p.osc = 0; p.sub = 1; p.subInterval = d; p.floor = false;
        render (e, p, withLeadIn (sine));
        const float got = e.uiSubHz.load(), expect = f / (float) (d + 2);
        const bool ok = std::abs (1200.0f * std::log2 (got / expect)) < 10.0f;
        good += ok ? 1 : 0;
        if (! ok) bad += fmt (" /%.0f=%.1fHz", d + 2, got);
    }
    check (good == 8, "unison root divides the square by 2 to 9" + bad);

    // the sub voice really divides: A2 / 2 = 55 Hz in the audio
    {
        BitrotEngine e; e.prepare (kSr);
        EngineParams p; p.square = 0; p.osc = 0; p.sub = 1; p.subInterval = 0;
        const auto run = render (e, p, withLeadIn (sine));
        int rises = 0;
        const size_t a = (size_t) (0.3f * kSr), b = (size_t) (0.75f * kSr);
        for (size_t i = a + 1; i < b; ++i) if (run.L[i - 1] <= 0.0f && run.L[i] > 0.0f) ++rises;
        const float measured = (float) rises / ((float) (b - a) / kSr);
        check (std::abs (measured - 55.0f) < 3.0f, fmt ("the sub voice plays %.1f Hz (expect 55)", measured));
    }

    // bass: the floor lifts subs out of the mud, keeping the note name
    {
        const float lowE = measuredHz (pluck (mtof (28.0f), 1.0f, 1.6f, 7u), mtof (28.0f));
        for (int d : { 0, 4, 7 })
        {
            BitrotEngine e; e.prepare (kSr);
            EngineParams p; p.input = 1; p.square = 0; p.osc = 0; p.sub = 1; p.subInterval = d; p.floor = true; p.floorHz = 1;
            const auto run = render (e, p, withLeadIn (pluck (mtof (28.0f), 1.0f, 1.6f, 7u + (uint32_t) d)));
            const float got = e.uiSubHz.load(), raw = lowE / (float) (d + 2);
            const float octs = std::log2 (got / raw);
            const bool ok = got >= 29.5f && got < 62.0f && std::abs (octs - std::round (octs)) < 0.03f;
            check (ok, fmt ("low E / %.0f: %.1f Hz lifted to %.1f Hz (%.0f octaves up, same note)", d + 2, raw, got, std::round (octs)));
            if (d == 7) writeWav ("bass_sub_floor", run.L, run.R);
        }
        BitrotEngine e; e.prepare (kSr);
        EngineParams p; p.input = 1; p.square = 0; p.osc = 0; p.sub = 1; p.subInterval = 7; p.floor = false;
        const auto run = render (e, p, withLeadIn (pluck (mtof (28.0f), 1.0f, 1.6f, 7u)));
        check (e.uiSubHz.load() < 6.0f, fmt ("floor off: low E / 9 flutters at %.1f Hz, as the pedal", e.uiSubHz.load()));
        writeWav ("bass_sub_flutter", run.L, run.R);
    }

    // oscillator root: the sub follows the oscillator, sum and difference makes new tones
    {
        BitrotEngine e; e.prepare (kSr);
        EngineParams p; p.square = 0; p.osc = 0; p.sub = 1; p.subRoot = 1; p.subInterval = 0; p.oscInterval = 3; p.fmMode = 0; p.rate = 1.0f;
        const auto run = render (e, p, withLeadIn (sine));
        check (std::abs (e.uiSubHz.load() - 220.0f) < 3.0f, fmt ("oscillator root: x4 then / 2 = %.1f Hz (expect 220)", e.uiSubHz.load()));
        check (run.finite && run.peak > 0.05f, "sum and difference sounds");
        writeWav ("sub_sum_difference", run.L, run.R);
    }
}

// ------------------------------------------------------------------------------------------
static void performanceSuite()
{
    std::printf ("Freeze, glide, vibrato\n");
    auto tone = [] (float hz, float secs)
    {
        std::vector<float> s ((size_t) (secs * kSr));
        for (size_t i = 0; i < s.size(); ++i) s[i] = 0.25f * std::sin (6.2831853f * hz * (float) i / kSr);
        return s;
    };
    // freeze A2, then play D3: the oscillator stays on A2's harmonic, the square follows
    {
        auto x = tone (110.0f, 0.5f);
        auto y = tone (146.83f, 0.6f);
        x.insert (x.end(), y.begin(), y.end());
        BitrotEngine e; e.prepare (kSr);
        auto p = oscOnly (0);
        const auto run = render (e, p, withLeadIn (x), [] (int b, EngineParams& q) { q.freeze = b * 64 > (int) (0.45f * kSr); });
        const auto st = lockStats (std::vector<float> (run.oscHz.end() - 200, run.oscHz.end()), 220.0f, 0.0f, 1.0e9f);
        check (st.median < 10.0f && e.uiFrozen.load(), fmt ("freeze holds the note through a new one (%.1f cents off A2 +1 oct)", st.median));
        // and keeps sounding after the input stops
        auto silence = std::vector<float> ((size_t) (0.4f * kSr), 0.0f);
        auto p2 = p; p2.freeze = true;
        const auto tail = render (e, p2, silence);
        float late = 0.0f;
        for (size_t i = tail.L.size() / 2; i < tail.L.size(); ++i) late = std::max (late, std::abs (tail.L[i]));
        check (late > 0.05f, "frozen, the drone keeps going after you stop");
        writeWav ("freeze", run.L, run.R);
    }
    // freeze from the moment the switch went down
    {
        auto x = tone (110.0f, 0.5f);
        auto y = tone (164.81f, 0.6f);
        x.insert (x.end(), y.begin(), y.end());
        BitrotEngine e; e.prepare (kSr);
        auto p = oscOnly (0);
        // the switch went down on A2 but freeze arrives 120 ms later, during E3
        render (e, p, withLeadIn (x), [] (int b, EngineParams& q)
        {
            const float t = (float) (b * 64 - 2400) / kSr;
            q.freeze = t > 0.62f; q.freezeBackMs = 170.0f;
        });
        const float got = e.uiOscHz.load();
        check (std::abs (1200.0f * std::log2 (got / 220.0f)) < 20.0f, fmt ("freeze looks back to the press: %.1f Hz (A2's 220)", got));
    }
    // glide: slow Rate slides, fast Rate jumps
    for (float rate : { 0.2f, 1.0f })
    {
        auto x = tone (110.0f, 0.4f);
        auto y = tone (164.81f, 0.4f);
        x.insert (x.end(), y.begin(), y.end());
        BitrotEngine e; e.prepare (kSr);
        auto p = oscOnly (0); p.rate = rate;
        const auto run = render (e, p, withLeadIn (x));
        const size_t at = (size_t) ((2400 + 0.4f * kSr + 0.08f * kSr) / 64);   // 80 ms after the change
        const float c = 1200.0f * std::log2 (run.oscHz[at] / 329.6f);
        if (rate < 0.5f) check (c < -150.0f, fmt ("slow glide is still sliding 80 ms in (%.0f cents to go)", -c));
        else check (std::abs (c) < 30.0f, fmt ("fast glide has arrived 80 ms in (%.0f cents)", c));
        if (rate < 0.5f) writeWav ("glide", run.L, run.R);
    }
    // vibrato
    {
        BitrotEngine e; e.prepare (kSr);
        auto p = oscOnly (0); p.fmMode = 1; p.rate = 0.5f; p.depth = 0.4f;
        const auto run = render (e, p, withLeadIn (tone (110.0f, 1.0f)));
        float lo = 1e9f, hi = 0.0f;
        for (size_t b = run.oscHz.size() / 2; b < run.oscHz.size(); ++b) { lo = std::min (lo, run.oscHz[b]); hi = std::max (hi, run.oscHz[b]); }
        const float swing = 1200.0f * std::log2 (hi / lo);
        check (swing > 150.0f && swing < 300.0f, fmt ("vibrato swings %.0f cents peak to peak (depth 1 st)", swing));
        writeWav ("vibrato", run.L, run.R);
    }
}

// ------------------------------------------------------------------------------------------
static void characterSuite()
{
    std::printf ("Corruption and safety\n");
    // two strings at once: the wild loop loses hold and stutters, and stays safe
    {
        auto a = pluck (mtof (45.0f), 1.5f, 0.0f, 3u), b = pluck (mtof (52.0f), 1.5f, 0.0f, 4u);
        for (size_t i = 0; i < a.size(); ++i) a[i] += b[i];
        BitrotEngine wild; wild.prepare (kSr);
        EngineParams p; p.corrupt = 1.0f; p.subRoot = 1; p.oscInterval = 5;
        const auto run = render (wild, p, withLeadIn (a));
        BitrotEngine tight; tight.prepare (kSr);
        EngineParams q = p; q.corrupt = 0.0f;
        render (tight, q, withLeadIn (a));
        check (wild.uiLockLosses.load() >= 1, "wild: a chord makes the loop lose hold (" + std::to_string (wild.uiLockLosses.load()) + " times)");
        check (run.finite && run.peak <= 1.0f, fmt ("wild chaos stays finite and under 0 dBFS (peak %.2f)", run.peak));
        writeWav ("wild_chord", run.L, run.R);
    }
    // silence in, silence out
    {
        BitrotEngine e; e.prepare (kSr);
        EngineParams p; p.oscInterval = 7; p.subInterval = 7; p.corrupt = 1.0f;
        std::vector<float> hiss ((size_t) kSr);
        Rng r; for (auto& v : hiss) v = r.bi() * 0.0002f;   // a quiet noise floor
        const auto run = render (e, p, hiss);
        check (run.peak < 0.002f, fmt ("noise floor in, silence out (peak %.4f)", run.peak));
    }
    // a note stops dead when it fades below the gate
    {
        BitrotEngine e; e.prepare (kSr);
        EngineParams p; p.corrupt = 0.2f;
        auto x = pluck (mtof (40.0f), 0.4f, 0.0f, 5u);
        x.resize ((size_t) (0.9f * kSr), 0.0f);
        const auto run = render (e, p, withLeadIn (x));
        float tail = 0.0f;
        for (size_t i = (size_t) (2400 + 0.6f * kSr); i < run.L.size(); ++i) tail = std::max (tail, std::abs (run.L[i]));
        check (tail < 0.002f, fmt ("the note stops after the string does (tail %.4f)", tail));
        writeWav ("decay", run.L, run.R);
    }
    // a random sweep through every setting
    {
        BitrotEngine e; e.prepare (kSr);
        Rng r; r.s = 4242u;
        std::vector<float> x;
        for (int k = 0; k < 12; ++k) { auto pl = pluck (mtof (24.0f + r.uni() * 52.0f), 0.5f, r.uni() * 2.0f, 100u + (uint32_t) k); x.insert (x.end(), pl.begin(), pl.end()); }
        EngineParams p;
        const auto run = render (e, p, x, [&] (int b, EngineParams& q)
        {
            if (b % 120 != 0) return;
            q.square = r.uni(); q.sub = r.uni(); q.osc = r.uni(); q.dry = r.uni() * 0.5f; q.level = r.uni();
            q.subInterval = (int) (r.uni() * 8); q.subRoot = r.uni() < 0.5f; q.floor = r.uni() < 0.5f; q.subMix = r.uni() < 0.5f;
            q.fmMode = r.uni() < 0.5f; q.rate = r.uni(); q.depth = r.uni(); q.oscInterval = (int) (r.uni() * 8); q.oscRoot = (int) (r.uni() * 3);
            q.bend = r.uni() < 0.2f ? r.uni() : 0.0f; q.input = (int) (r.uni() * 3); q.corrupt = r.uni(); q.trackComp = r.uni() < 0.8f;
            q.freeze = r.uni() < 0.1f; q.octUp = r.uni() < 0.2f; q.bypass = r.uni() < 0.05f;
        });
        double dc = 0.0;
        for (float v : run.L) dc += v;
        dc /= (double) run.L.size();
        check (run.finite && run.peak <= 1.2f && std::abs (dc) < 0.01, fmt ("random sweep: finite, peak %.2f, DC %.4f", run.peak, dc));
        writeWav ("sweep", run.L, run.R);
    }
    // cost
    {
        BitrotEngine e; e.prepare (kSr);
        EngineParams p; p.oscInterval = 7; p.corrupt = 0.5f;
        auto x = pluck (110.0f, 10.0f, 0.0f, 9u);
        const auto t0 = std::chrono::steady_clock::now();
        render (e, p, x);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        const double load = secs / 10.0 * 100.0;
        check (load < 10.0, fmt ("10 s of audio in %.0f ms: %.2f%% of one core", secs * 1000.0, load));
    }
}

int main (int argc, char** argv)
{
    if (argc > 1) outDir = argv[1];
    lockSuite();
    intervalSuite();
    subSuite();
    performanceSuite();
    characterSuite();
    std::printf (failures == 0 ? "All passed\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
