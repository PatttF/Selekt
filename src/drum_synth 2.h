// drum_synth.h — Built-in drum synthesizer using miniaudio
// Cross-platform audio (CoreAudio on macOS, ALSA/PulseAudio on Linux)
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <atomic>

// ═══════════════════════════════════════════════════════════════════════
//  Forward-declare miniaudio device (avoid including huge header here)
// ═══════════════════════════════════════════════════════════════════════
struct ma_device;

// ═══════════════════════════════════════════════════════════════════════
//  Constants
// ═══════════════════════════════════════════════════════════════════════
static constexpr int   DS_SAMPLE_RATE = 44100;
static constexpr int   DS_MAX_VOICES  = 32;
static constexpr float DS_TAU         = 6.283185307f;

// FX buffer sizes (per voice)
static constexpr int DS_DLY_MAX  = 8820;   // ~200ms max delay
// Reverb: 4 comb filters (Freeverb-style lengths) + 2 allpass diffusers
static constexpr int DS_COMB1_SZ = 1116;
static constexpr int DS_COMB2_SZ = 1188;
static constexpr int DS_COMB3_SZ = 1277;
static constexpr int DS_COMB4_SZ = 1356;
static constexpr int DS_RAP1_SZ  = 556;    // reverb allpass 1
static constexpr int DS_RAP2_SZ  = 441;    // reverb allpass 2
static constexpr int DS_CHO_SZ   = 1024;   // chorus delay line

// ═══════════════════════════════════════════════════════════════════════
//  Drum types (must match the enum in main.cpp)
// ═══════════════════════════════════════════════════════════════════════
enum DSType { DS_KICK=0, DS_SNARE, DS_HIHAT, DS_TOM, DS_CLAP, DS_PERC, DS_CYMBAL, DS_SHAKER, DS_COWBELL, DS_RIDE, DS_CRASH, DS_TYPE_COUNT };

// ═══════════════════════════════════════════════════════════════════════
//  Synth parameters (normalised 0-127, mapped to useful ranges per type)
// ═══════════════════════════════════════════════════════════════════════
struct DSParams {
    // p[0-5]=SYNTH  p[6-11]=EXT  p[12-17]=FX  p[18-23]=SEND
    int p[24] = {64,64,64,64,64,64, 0,0,0,0,0,0, 127,0,0,0,0,100, 0,40,0,40,0,60};
    // Page 1 (0-5):   synthesis params (per drum type)
    // Page 2 (6-11):  FILTER, RESO, COMP, SATUR, BITE, LEVEL
    // Page 3 (12-17): REVERB, REV SZ, DELAY, D.TIME, CHORUS, CH.SPD
};

// ═══════════════════════════════════════════════════════════════════════
//  Single voice — one active drum hit
// ═══════════════════════════════════════════════════════════════════════
struct DSVoice {
    bool   active  = false;
    int    type    = DS_KICK;
    float  vel     = 1.0f;       // 0-1 velocity
    float  phase   = 0.0f;       // oscillator phase
    float  phase2  = 0.0f;       // secondary oscillator
    float  phase3  = 0.0f;       // tertiary oscillator (FM mod, shell, etc.)
    float  t       = 0.0f;       // time in seconds since trigger
    float  envA    = 1.0f;       // amplitude envelope
    float  envP    = 1.0f;       // pitch envelope
    float  noiseState  = 0.0f;   // noise filter state 1
    float  noiseState2 = 0.0f;   // noise filter state 2
    float  noiseState3 = 0.0f;   // noise filter state 3 (extra band)
    float  noiseState4 = 0.0f;   // noise filter state 4
    float  fltLP  = 0.0f;        // post-processing LP filter state
    float  fltBP  = 0.0f;        // post-processing BP filter state
    float  prevSample = 0.0f;    // for DC blocker / feedback
    // Page 3 FX state — delay, reverb, chorus (per voice)
    float  dlyBuf[DS_DLY_MAX] = {};
    int    dlyPos = 0;
    // Reverb: 4 comb filters + 2 allpass diffusers
    float  comb1[DS_COMB1_SZ] = {}, comb2[DS_COMB2_SZ] = {};
    float  comb3[DS_COMB3_SZ] = {}, comb4[DS_COMB4_SZ] = {};
    int    combPos1 = 0, combPos2 = 0, combPos3 = 0, combPos4 = 0;
    float  combLP1 = 0, combLP2 = 0, combLP3 = 0, combLP4 = 0; // LP damping per comb
    float  rap1[DS_RAP1_SZ] = {}, rap2[DS_RAP2_SZ] = {};
    int    rapPos1 = 0, rapPos2 = 0;
    float  chorusBuf[DS_CHO_SZ] = {};
    int    chorusPos = 0;
    float  chorusPhase = 0.0f;
    DSParams par;
    uint32_t rndseed = 12345;

    // Fast PRNG
    float noise() {
        rndseed = rndseed * 1103515245u + 12345u;
        return (float)(int)(rndseed >> 16 & 0x7FFF) / 16383.5f - 1.0f;
    }
};

// ═══════════════════════════════════════════════════════════════════════
//  Drum synth engine
// ═══════════════════════════════════════════════════════════════════════
class DrumSynth {
public:
    DrumSynth();
    ~DrumSynth();

    bool init();       // start audio device
    void shutdown();   // stop audio device

    // Trigger a drum hit (call from any thread — lock-free via atomic writes)
    void trigger(int type, float velocity, const DSParams& params);

    // Master volume 0-1
    void setVolume(float v) { volume_.store(v); }
    float volume() const { return volume_.load(); }

    bool running() const { return running_; }

private:
    // Audio callback (static, called by miniaudio)
    static void audioCallback(void* device, void* output, const void* input, uint32_t frameCount);
    void render(float* out, uint32_t frames);
    float renderVoice(DSVoice& v);

    DSVoice voices_[DS_MAX_VOICES];
    std::mutex trigMtx_;
    std::atomic<float> volume_{0.8f};
    bool running_ = false;
    void* device_ = nullptr;  // actually ma_device*, cast to avoid header dep
};

// ═══════════════════════════════════════════════════════════════════════
//  Implementation
// ═══════════════════════════════════════════════════════════════════════

// We need the miniaudio implementation in exactly ONE translation unit.
// Since we include this header from main.cpp, we define it there.
// The actual impl functions are below, guarded by DRUM_SYNTH_IMPL.

#ifdef DRUM_SYNTH_IMPL

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

DrumSynth::DrumSynth() {
    memset(voices_, 0, sizeof(voices_));
}

DrumSynth::~DrumSynth() {
    shutdown();
}

bool DrumSynth::init() {
    if (running_) return true;

    auto* dev = new ma_device;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = DS_SAMPLE_RATE;
    cfg.dataCallback      = (ma_device_data_proc)audioCallback;
    cfg.pUserData         = this;

    if (ma_device_init(nullptr, &cfg, dev) != MA_SUCCESS) {
        delete dev;
        return false;
    }
    if (ma_device_start(dev) != MA_SUCCESS) {
        ma_device_uninit(dev);
        delete dev;
        return false;
    }
    device_ = dev;
    running_ = true;
    return true;
}

void DrumSynth::shutdown() {
    if (!running_) return;
    auto* dev = static_cast<ma_device*>(device_);
    if (dev) {
        ma_device_stop(dev);
        ma_device_uninit(dev);
        delete dev;
    }
    device_ = nullptr;
    running_ = false;
}

void DrumSynth::trigger(int type, float velocity, const DSParams& params) {
    std::lock_guard<std::mutex> lk(trigMtx_);
    // Find a free voice (or steal oldest)
    int best = 0;
    float oldest_t = -1.0f;
    for (int i = 0; i < DS_MAX_VOICES; ++i) {
        if (!voices_[i].active) { best = i; break; }
        if (voices_[i].t > oldest_t) { oldest_t = voices_[i].t; best = i; }
    }
    DSVoice& v = voices_[best];
    v.active = true;
    v.type   = type;
    v.vel    = velocity;
    v.phase  = 0.0f;
    v.phase2 = 0.0f;
    v.phase3 = 0.0f;
    v.t      = 0.0f;
    v.envA   = 1.0f;
    v.envP   = 1.0f;
    v.noiseState  = 0.0f;
    v.noiseState2 = 0.0f;
    v.noiseState3 = 0.0f;
    v.noiseState4 = 0.0f;
    v.fltLP  = 0.0f;
    v.fltBP  = 0.0f;
    v.prevSample = 0.0f;
    // Clear FX buffers
    v.dlyPos = 0;
    v.combPos1 = v.combPos2 = v.combPos3 = v.combPos4 = 0;
    v.combLP1 = v.combLP2 = v.combLP3 = v.combLP4 = 0;
    v.rapPos1 = v.rapPos2 = 0;
    v.chorusPos = 0; v.chorusPhase = 0;
    memset(v.dlyBuf, 0, sizeof(v.dlyBuf));
    memset(v.comb1, 0, sizeof(v.comb1)); memset(v.comb2, 0, sizeof(v.comb2));
    memset(v.comb3, 0, sizeof(v.comb3)); memset(v.comb4, 0, sizeof(v.comb4));
    memset(v.rap1, 0, sizeof(v.rap1)); memset(v.rap2, 0, sizeof(v.rap2));
    memset(v.chorusBuf, 0, sizeof(v.chorusBuf));
    v.par    = params;
    v.rndseed = (uint32_t)(velocity * 65536.0f) + 42u;
    // Kick: start at cosine peak (π/2) for immediate impact with no ramp
    if (type == DS_KICK) v.phase = 1.5707963f;
}

void DrumSynth::audioCallback(void* device, void* output, const void* /*input*/, uint32_t frameCount) {
    auto* dev = static_cast<ma_device*>(device);
    auto* self = static_cast<DrumSynth*>(dev->pUserData);
    self->render(static_cast<float*>(output), frameCount);
}

void DrumSynth::render(float* out, uint32_t frames) {
    float vol = volume_.load();
    for (uint32_t f = 0; f < frames; ++f) {
        float mix = 0.0f;
        for (int i = 0; i < DS_MAX_VOICES; ++i) {
            if (!voices_[i].active) continue;
            mix += renderVoice(voices_[i]);
        }
        mix *= vol;
        // Soft clip
        if (mix > 1.0f)  mix = 1.0f - 1.0f / (mix + 1.0f);
        if (mix < -1.0f) mix = -1.0f + 1.0f / (-mix + 1.0f);
        out[f * 2]     = mix;
        out[f * 2 + 1] = mix;
    }
}

// ── Helper: fast approximation sine ──────────────────────────────────
static inline float fastSin(float p) {
    p = fmodf(p, DS_TAU);
    if (p < 0) p += DS_TAU;
    return sinf(p);
}

// ── Helper: exponential decay ────────────────────────────────────────
static inline float expDecay(float t, float rate) {
    return expf(-t * rate);
}

// ── Helper: multi-stage envelope (attack, hold, decay) ───────────────
static inline float adsEnv(float t, float atk, float hold, float decRate) {
    if (t < atk) return t / atk;
    float td = t - atk;
    if (td < hold) return 1.0f;
    return expf(-(td - hold) * decRate);
}

// ── Helper: wavefolder ───────────────────────────────────────────────
static inline float wavefold(float x, float amount) {
    x *= (1.0f + amount * 3.0f);
    // Fold back into [-1, 1] range
    while (x > 1.0f || x < -1.0f) {
        if (x > 1.0f)  x = 2.0f - x;
        if (x < -1.0f) x = -2.0f - x;
    }
    return x;
}

// ── Render one sample from a voice ───────────────────────────────────
float DrumSynth::renderVoice(DSVoice& v) {
    float dt = 1.0f / DS_SAMPLE_RATE;
    float sample = 0.0f;

    // Map params 0-127 → normalised 0-1
    float p0 = v.par.p[0] / 127.0f;
    float p1 = v.par.p[1] / 127.0f;
    float p2 = v.par.p[2] / 127.0f;
    float p3 = v.par.p[3] / 127.0f;
    float p4 = v.par.p[4] / 127.0f;
    float p5 = v.par.p[5] / 127.0f;
    // EXT per-type params (page 2, p[6-11])
    float pe0 = v.par.p[6]  / 127.0f;  // ring amt / harm / FM ratio
    float pe1 = v.par.p[7]  / 127.0f;  // ring freq / sweep / parts / FM depth
    float pe2 = v.par.p[8]  / 127.0f;  // texture / brightness / shape / fold
    float pe3 = v.par.p[9]  / 127.0f;  // transient / crack / pmod / shell
    float pe4 = v.par.p[10] / 127.0f;  // sym / pitch2 / harm2 / tail
    float pe5 = v.par.p[11] / 127.0f;  // body2 / wire / hiss / crsp / src / sym

    switch (v.type) {
    case DS_KICK: {
        // p0=PITCH  p1=DECAY  p2=DRIVE  p3=CLICK  p4=SUB  p5=PUNCH
        // 808/909 analogue model: cosine-start sine + exponential pitch sweep + click
        float baseFreq  = 28.0f + p0 * 60.0f;            // 28-88 Hz
        float decayRate = 1.8f  + (1.0f - p1) * 12.0f;   // amplitude decay rate

        // ─── Pitch envelope: 808-style — deep fast exponential sweep ───
        // Pitch starts at (1 + sweepDepth) × baseFreq and falls to baseFreq
        float sweepDepth = 3.0f + p5 * 9.0f + pe1 * 18.0f;  // pe1=SWEEP: how high it starts
        float sweepRate  = 40.0f + (1.0f - p5) * 40.0f;     // p5=PUNCH: faster punch = faster fall
        float pitchEnv   = sweepDepth * expDecay(v.t, sweepRate);
        float freq = baseFreq * (1.0f + pitchEnv);

        float subFreq = baseFreq * 0.5f;
        v.phase  += freq    * dt * DS_TAU;
        v.phase2 += subFreq * dt * DS_TAU;
        // pe3=CLICK2: beater pitch 300→10kHz
        v.phase3 += (300.0f + pe3 * 9700.0f) * dt * DS_TAU;

        // Main body: sine wave (phase was initialised at π/2 in trigger() for immediate peak)
        float osc = sinf(v.phase);
        if (pe2 > 0.01f) { osc = wavefold(osc, pe2 * 0.9f); }  // pe2=SHAPE
        osc += pe4 * 0.55f * osc * osc;                          // pe4=SYM: asymmetric warmth

        if (p2 > 0.01f) {
            float drv = 1.0f + p2 * 14.0f;
            if (p2 < 0.5f) {
                osc = tanhf(osc * drv) / tanhf(drv);
            } else {
                osc = tanhf(osc * drv * 0.5f) / tanhf(drv * 0.5f);
                osc = wavefold(osc, (p2 - 0.5f) * 2.0f);
            }
        }

        // Second harmonic adds thump character (pe0=HARM)
        float harm2 = sinf(v.phase * 2.0f) * (0.03f + pe0 * 0.5f) * expDecay(v.t, decayRate * 1.5f);
        // Sub oscillator for low-end weight (p4=SUB, pe5=BODY2)
        float sub   = sinf(v.phase2) * (p4 * 0.85f + pe5 * 0.95f) * expDecay(v.t, decayRate * 0.45f);

        // Beater click: HP noise burst + pitched click sine (p3=CLICK)
        float clickRate  = 110.0f + (1.0f - p3) * 280.0f;
        float clickEnv   = expDecay(v.t, clickRate);
        float clickNoise = v.noise();
        v.noiseState3 += 0.22f * (clickNoise - v.noiseState3);
        float clickHP   = (clickNoise - v.noiseState3) * clickEnv * p3 * 0.65f;
        float clickSine = sinf(v.phase3) * clickEnv * p3 * 0.22f;

        // Pure exponential amplitude — no soft-attack ramp, phase init handles the transient
        float env = expDecay(v.t, decayRate);
        float rawSample = (osc * 0.58f + harm2 + sub + clickHP + clickSine) * env;

        // DC blocker (removes any DC offset from asymmetric wavefolding)
        float dc = rawSample - v.prevSample + 0.995f * v.fltLP;
        v.fltLP      = dc;
        v.prevSample = rawSample;
        sample = dc * (0.38f + v.vel * 0.62f);
        v.envA = env;
        break;
    }
    case DS_SNARE: {
        // p0=PITCH  p1=DECAY  p2=SNAP  p3=NOISE  p4=TONE  p5=RING
        // Body = two detuned sines; Wire = resonant SVF bandpass on noise (key quality upgrade)
        float freq      = 140.0f + p0 * 180.0f;              // 140-320 Hz
        float decayRate = 5.0f + (1.0f - p1) * 26.0f;
        float noiseDecay= 3.5f + (1.0f - p1) * 15.0f;

        // ─── Body: two detuned sines with initial pitch crack ───
        float pSweep = 1.0f + 0.8f * expDecay(v.t, 55.0f);
        v.phase  += freq        * pSweep * dt * DS_TAU;
        v.phase2 += freq * 1.56f * pSweep * dt * DS_TAU;  // inharmonic second mode
        float body1 = sinf(v.phase)  * 0.50f;
        float body2 = sinf(v.phase2) * (0.12f + pe4 * 0.38f);  // pe4=BODY2
        float bodyMix = (body1 + body2) * (0.28f + p4 * 0.72f);

        // ─── Crack: high-frequency pitched transient for snap character ───
        float crackFreq = 350.0f + p2 * 600.0f + pe3 * 400.0f;  // pe3=CRACK freq
        v.phase3 += crackFreq * 3.1f * dt * DS_TAU;
        float crackEnv = expDecay(v.t, 55.0f + (1.0f - p2) * 220.0f + pe3 * 300.0f);
        float crack     = sinf(v.phase3) * crackEnv * p2 * 0.50f;
        float snapNoise = v.noise() * crackEnv * p2 * 0.55f;

        // ─── Snare wire: RESONANT SVF bandpass on noise ───
        // This creates the characteristic "metallic rattle" of the snare wire
        // SVF: hp = n - LP*Qdamp - BP;  BP += F*hp;  LP += F*BP;  output=BP
        float wireCenter = 1500.0f + p3 * 2500.0f + pe2 * 2000.0f;  // pe2=RATTL: 1.5-6kHz
        float wireF      = 2.0f * sinf(3.14159f * fminf(wireCenter / DS_SAMPLE_RATE, 0.45f));
        float wireQdamp  = 0.12f + (1.0f - p3) * 0.28f;  // low Qdamp = high resonance
        // Allow pe5 to boost resonance further (pe5=WIRE)
        wireQdamp       *= fmaxf(0.05f, 1.0f - pe5 * 0.75f);

        float n = v.noise();
        float wireHP = n - v.noiseState2 - wireQdamp * v.noiseState;
        v.noiseState  += wireF * wireHP;
        v.noiseState2 += wireF * v.noiseState;
        float wireRattle = v.noiseState;  // resonant bandpass of noise

        // Air layer: simple HP noise above ~5kHz for "sssss" hiss tail
        float n2 = v.noise();
        float airCut = 0.20f + pe2 * 0.15f;
        v.noiseState3 += airCut * (n2 - v.noiseState3);
        float airNoise = (n2 - v.noiseState3) * (0.25f + pe5 * 0.35f);

        // Body noise (low-frequency "thwump" noise component)
        float n3 = v.noise();
        v.noiseState4 += 0.04f * (n3 - v.noiseState4);
        float bodyNoise = (n3 - v.noiseState4) * 0.3f;

        float wireSig = (wireRattle * 0.65f + airNoise * 0.20f + bodyNoise * 0.15f)
                        * (0.35f + p3 * 0.95f);

        // ─── Metallic ring tail (pe0=RNGAMT, pe1=RNGFRQ) ───
        float ringFreq = freq * (2.6f + p5 * 4.5f + (pe1 - 0.5f) * 3.0f);
        float ring = sinf(v.phase3 * (ringFreq / (crackFreq * 3.1f)))
                     * (p5 * 0.18f + pe0 * 0.35f);

        float toneEnv = expDecay(v.t, decayRate);
        float nenv    = expDecay(v.t, noiseDecay);
        sample = (bodyMix * toneEnv + wireSig * nenv + crack + snapNoise + ring * toneEnv)
                 * (0.42f + v.vel * 0.58f);
        v.envA = fmaxf(toneEnv, nenv);
        break;
    }
    case DS_HIHAT: {
        // p0=PITCH  p1=DECAY  p2=COLOR  p3=METAL  p4=TONE  p5=OPEN
        // Yamaha/TR approach: 6 inharmonic square-wave oscillators → HP filter
        // The sum of 6 non-integer-ratio square waves creates a complex
        // pseudo-random metallic waveform across the spectrum.
        float closedDecayRate = 32.0f + (1.0f - p1) * 80.0f;
        float openDecayRate   = 2.0f  + (1.0f - p1) * 7.0f;
        float decayRate = closedDecayRate * (1.0f - p5) + openDecayRate * p5;

        // Base: 300-900 Hz. Square-wave harmonics reach 12kHz+ easily.
        float baseFreq = 300.0f + p0 * 600.0f;

        // 6 inharmonic ratios — the specific non-integer values create the
        // beating and inter-modulation products that give hihats their metallic character
        static const float HH_RATIOS[6] = {1.0f, 1.483f, 1.932f, 2.346f, 2.952f, 3.864f};

        // Accumulate phases (noiseState×3 repurposed as extra phase accumulators)
        v.phase  += baseFreq * HH_RATIOS[0] * dt * DS_TAU;
        v.phase2 += baseFreq * HH_RATIOS[1] * dt * DS_TAU;
        v.phase3 += baseFreq * HH_RATIOS[2] * dt * DS_TAU;
        v.noiseState  += baseFreq * HH_RATIOS[3] * dt;   // used as raw phase acc
        v.noiseState2 += baseFreq * HH_RATIOS[4] * dt;
        v.noiseState3 += baseFreq * HH_RATIOS[5] * dt;

        // Hard-clip each sine → square-wave approximation
        // p3=METAL: high metal = smaller clip level = harder square = more metallic
        float clipLvl = 0.06f + (1.0f - p3) * 0.74f;
        auto sq = [&](float ph) -> float {
            float s = sinf(ph);
            return fmaxf(-clipLvl, fminf(clipLvl, s)) / clipLvl;
        };
        // Sum with descending amplitudes (higher partials less loud)
        float metalSum = sq(v.phase)  * 0.21f
                       + sq(v.phase2) * 0.19f
                       + sq(v.phase3) * 0.17f
                       + sq(v.noiseState  * DS_TAU) * 0.15f
                       + sq(v.noiseState2 * DS_TAU) * 0.13f
                       + sq(v.noiseState3 * DS_TAU) * 0.11f;

        // HP filter: remove low-frequency body content, keep the metallic complex
        // p2=COLOR + pe2=BRGHT control where the HP sits (higher = more airy/crisp)
        float hpCutFreq = fminf(0.48f,
            (3500.0f + p2 * 4000.0f + pe2 * 3500.0f) / DS_SAMPLE_RATE);
        // Two-stage one-pole HP for steeper roll-off
        v.noiseState4 += hpCutFreq * (metalSum - v.noiseState4);   // LP stage 1
        float hp1 = metalSum - v.noiseState4;

        // Air sizzle: HP white noise adds the "tsssss" sustain tail
        float noiseRaw = v.noise();
        float airCut   = 0.12f + p2 * 0.20f;
        v.prevSample  += airCut * (noiseRaw - v.prevSample);         // LP for air HP
        float airNoise = (noiseRaw - v.prevSample)
                         * (0.55f - p4 * 0.40f);  // p4=TONE: more tone = less air

        // p4=TONE crossfades: 0=air-dominant, 1=metallic-oscillator-dominant
        float sig = hp1 * (0.45f + p4 * 0.55f) + airNoise * 0.55f;

        // Attack transient (short burst for the initial "tick")
        float attEnv = expDecay(v.t, 380.0f + (1.0f - p5) * 120.0f);
        float att    = v.noise() * attEnv * (0.18f + pe3 * 0.28f);  // pe3=HIT

        // pe0=SHADE, pe1=HISS
        float hiss = v.noise() * pe5 * 0.18f;  // pe5=HISS: constant noise floor

        float env = expDecay(v.t, decayRate);
        sample = (sig + att + hiss) * env * (0.30f + v.vel * 0.70f) * 0.55f;
        v.envA = env;
        break;
    }
    case DS_TOM: {
        // p0=PITCH  p1=DECAY  p2=TONE  p3=ATTACK  p4=BODY  p5=RING
        // ─── Two detuned oscillators — gives warm tom "beating" distinct from kick ───
        float baseFreq = 40.0f + p0 * 340.0f;  // 40-380 Hz (wide range: floor to high tom)
        float decayRate = 3.0f + (1.0f - p1) * 15.0f;

        // Gentler pitch sweep than kick — tom dips, not dives
        float sweepAmt = 0.30f + p4 * 1.0f;
        float pitchSweep = sweepAmt * expDecay(v.t, 17.0f + (1.0f - p4) * 16.0f);
        float f1 = baseFreq * (1.0f + pitchSweep);
        float f2 = f1 * (1.0f + 0.005f + p2 * 0.016f);  // close detune — TONE controls beating width

        v.phase  += f1 * dt * DS_TAU;
        v.phase2 += f2 * dt * DS_TAU;

        // Beating between the two oscillators gives characteristic tom resonance
        float body = fastSin(v.phase) * 0.56f + fastSin(v.phase2) * 0.44f;
        body += pe5 * 0.25f * body * body;  // pe5=SYM: asymmetric warmth

        // Drum head modes at realistic membrane ratios
        float modeScale = 0.35f + pe2 * 0.90f;  // pe2=MODES
        float mode2 = fastSin(v.phase * (1.594f + (pe4 - 0.5f) * 0.10f)) * p2 * 0.22f * modeScale;  // pe4=PITCH2
        float mode3 = fastSin(v.phase * 2.136f) * p2 * 0.09f * modeScale;

        // ─── Beater: bandpass noise burst (softer than kick — membrane thwack, not wooden click) ───
        float atkSpeed = 50.0f + (1.0f - p3) * 180.0f;
        float atkEnv = expDecay(v.t, atkSpeed);
        float n = v.noise();
        // BP centered at ~3-5x fundamental for "thwok" quality
        float bpCut = fminf(0.38f, 0.04f + (baseFreq / (float)DS_SAMPLE_RATE) * 10.0f);
        v.noiseState  += bpCut * (n - v.noiseState);
        v.noiseState2 += bpCut * 0.75f * (v.noiseState - v.noiseState2);
        float attack = (v.noiseState - v.noiseState2) * atkEnv * p3 * 0.65f;

        // ─── Shell ring (pe0=RNGAMT, pe1=RNGFRQ) ───
        float ringFreq = baseFreq * (1.51f + p5 * 2.5f + (pe1 - 0.5f) * 1.5f);
        v.phase3 += ringFreq * dt * DS_TAU;
        float ringEnv = expDecay(v.t, decayRate * 0.65f);
        float ring  = fastSin(v.phase3)         * (p5 * 0.14f + pe0 * 0.22f) * ringEnv;
        float ring2 = fastSin(v.phase3 * 1.34f) * (p5 * 0.06f + pe0 * 0.09f) * ringEnv;

        // pe3=SHELL: sharp shell pop for articulation
        float shell = v.noise() * pe3 * 0.24f * expDecay(v.t, 380.0f);

        float env = adsEnv(v.t, 0.0005f, 0.001f, decayRate);
        sample = (body * 0.56f + mode2 + mode3 + attack + ring + ring2 + shell)
                 * env * (0.44f + v.vel * 0.56f);
        v.envA = env;
        break;
    }
    case DS_CLAP: {
        // p0=SPREAD  p1=DECAY  p2=TONE  p3=ROOM  p4=DRIVE  p5=WIDTH
        float decayRate = 5.0f + (1.0f - p1) * 22.0f;
        float spread = 0.004f + p0 * (0.038f + pe0 * 0.025f);  // pe0=BURST: spread timing

        // ─── Multi-burst: 5 staggered slaps build into a full clap ───
        float burstEnv = 0.0f;
        float burstTimes[5] = {0.0f, spread * 0.8f, spread * 1.8f, spread * 3.1f, spread * 4.5f};
        float burstAmps[5]  = {0.6f + pe3 * 0.7f, 1.0f, 0.85f, 0.65f, 0.45f};  // pe3=SLAP: first burst
        for (int b = 0; b < 5; ++b) {
            float bt = v.t - burstTimes[b];
            if (bt >= 0.0f)
                burstEnv += expDecay(bt, 95.0f + b * 12.0f + pe4 * 130.0f) * burstAmps[b];  // pe4=CRACK
        }
        float tailEnv = expDecay(v.t, decayRate);

        // ─── Palm smack: short resonant bandpass in low-mid region ───
        float n = v.noise();
        float cut  = 0.08f + p2 * 0.52f + pe2 * 0.22f;  // pe2=BRGHT
        float reso = 0.28f + p2 * 0.42f + pe1 * 0.28f;  // pe1=RESON
        // 2-pole SVF bandpass
        v.noiseState  += cut * (n - v.noiseState - v.noiseState2 * reso);
        v.noiseState2 += cut * v.noiseState;
        float bp = v.noiseState;

        // ─── Slap layer: HP noise for crisp initial transient ───
        float n2 = v.noise();
        float cutHi = fminf(0.45f, cut * 2.5f);
        v.noiseState3 += cutHi * (n2 - v.noiseState3);
        float slapHi = (n2 - v.noiseState3) * expDecay(v.t, 180.0f) * 0.5f;

        // Width: detuned second noise source
        float n3 = v.noise();
        v.noiseState4 += (cut * 0.75f) * (n3 - v.noiseState4);
        float widthSig = (n3 - v.noiseState4) * (p5 * 0.14f + pe5 * 0.18f) * tailEnv;  // pe5=SRC

        if (p4 > 0.01f) {
            float drv = 1.0f + p4 * 8.0f;
            bp = tanhf(bp * drv) / tanhf(drv);
        }

        // Room: comb approximation via brief feedback delay
        float roomAmt = p3 * 0.28f;
        v.prevSample = v.prevSample * 0.83f + bp * 0.17f;
        float roomSig = v.prevSample * roomAmt * expDecay(v.t, decayRate * 0.30f);

        float combinedEnv = fmaxf(burstEnv * 0.48f, tailEnv * 0.38f);
        sample = (bp * combinedEnv + slapHi * burstEnv + roomSig + widthSig)
                 * (0.42f + v.vel * 0.58f) * 0.72f;
        v.envA = tailEnv;
        break;
    }
    case DS_PERC: {
        // p0=PITCH  p1=DECAY  p2=TONE  p3=ATTACK  p4=DRIVE  p5=BODY
        // ─── Metallic two-oscillator percussion: cowbell / agogo / block character ───
        float f1 = 180.0f + p0 * 1400.0f;  // 180-1580 Hz
        float decayRate = 9.0f + (1.0f - p1) * 42.0f;

        // Two oscillators at a fixed interval — cowbell uses tritone-ish gap
        float f2Ratio = 1.50f + pe0 * 1.10f;   // pe0=FMRAT: ratio 1.5-2.6
        float f2 = f1 * f2Ratio;

        // FM modulation from osc2 into osc1 — metallic inharmonic bite
        float modDepth = 0.5f + p5 * 3.5f + pe1 * 2.5f;  // pe1=FMDEP
        float modDecay = expDecay(v.t, 10.0f + (1.0f - p5) * 28.0f);

        // Pitch sweep (pe3=PMOD)
        float sweep = 1.0f + pe3 * 1.6f * expDecay(v.t, 40.0f);

        v.phase2 += f2 * sweep * dt * DS_TAU;
        float modSig = fastSin(v.phase2) * modDepth * modDecay;
        v.phase  += f1 * sweep * dt * DS_TAU;
        float osc1 = fastSin(v.phase + modSig);

        // Second non-modulated tone for the "two-note" cowbell character
        float osc2 = fastSin(v.phase2 * 0.667f) * 0.45f;  // ~5th below f2

        // TONE: hard-clip blend for metallic edginess (p2=0→soft, p2=1→harsh)
        float blend = osc1 * 0.60f + osc2 * 0.40f;
        float clipLvl = 0.08f + p2 * 0.88f;
        float metallic = fmaxf(-clipLvl, fminf(clipLvl, blend)) / clipLvl;
        // pe2=FOLD: wavefolding for extra harmonic complexity
        if (pe2 > 0.01f) metallic = wavefold(metallic, pe2 * 0.75f);
        // pe5=SYM: asymmetric character
        metallic += pe5 * 0.28f * metallic * metallic;

        // Drive
        if (p4 > 0.01f) {
            float drv = 1.0f + p4 * 9.0f;
            metallic = tanhf(metallic * drv) / tanhf(drv);
        }

        // ─── Mallet attack ───
        float atkSpeed = 85.0f + (1.0f - p3) * 230.0f;
        float atkEnv = expDecay(v.t, atkSpeed);
        float atk = v.noise() * atkEnv * p3 * 0.38f;
        v.phase3 += (f1 * 2.7f) * dt * DS_TAU;
        float pClick = fastSin(v.phase3) * atkEnv * p3 * 0.18f;

        // pe4=HARM2: upper harmonic presence
        float harm2p = fastSin(v.phase * 3.0f) * pe4 * 0.16f * expDecay(v.t, decayRate * 0.6f);

        float env = expDecay(v.t, decayRate);
        sample = (metallic * 0.58f + atk + pClick + harm2p) * env * (0.42f + v.vel * 0.58f);
        v.envA = env;
        break;
    }
    case DS_CYMBAL: {
        // p0=PITCH  p1=DECAY  p2=BELL  p3=SHIMMER  p4=TONE  p5=BODY
        // Same 6-osc Yamaha approach as hihat, but: Lower freq, longer decay,
        // prominent bell, and the HP sits lower so the body has more presence.
        // pe4=TAIL: extend decay further
        float decayBase = 0.5f + (1.0f - p1) * 4.5f;
        float decayRate = decayBase * fmaxf(0.12f, 1.0f - pe4 * 0.72f);

        // Lower fundamental range, clearly below the hihat
        float baseFreq = 180.0f + p0 * 360.0f;  // 180-540 Hz

        // Cymbal ratios: more spread out than hihat → "washy" quality
        // These give a broader spectral spread versus hihat's tighter ratios
        static const float CYM_RATIOS[6] = {1.0f, 1.484f, 1.763f, 2.110f, 2.468f, 2.946f};

        // Single master phase; derive all partials from it (saves state variables)
        v.phase += baseFreq * dt * DS_TAU;

        // Hard-clip level: cymbal is softer than hihat (less harsh)
        float clipLvl = 0.15f + (1.0f - p3) * 0.60f;  // p3=SHIMMER influences brightness
        auto sq = [&](float ph) -> float {
            float s = sinf(ph);
            return fmaxf(-clipLvl, fminf(clipLvl, s)) / clipLvl;
        };
        // Amplitude weighting — each partial gets its own decay rate
        float bodySum = 0.0f;
        for (int h = 0; h < 6; ++h) {
            float partDecay = expDecay(v.t, decayRate * (1.0f + h * 0.15f));
            float amp = (0.20f - h * 0.02f) * partDecay;
            bodySum += sq(v.phase * CYM_RATIOS[h]) * amp;
        }

        // HP filter at LOWER frequency than hihat — gives cymbal its "body"
        // p4=TONE controls HP position: low tone = dark/low HP, high = bright/high HP
        float cymHPFreq = fminf(0.45f,
            (1200.0f + p4 * 2000.0f + pe2 * 2000.0f) / DS_SAMPLE_RATE);
        v.noiseState  += cymHPFreq * (bodySum - v.noiseState);  // LP state
        float cymHP = bodySum - v.noiseState;                    // HP output

        // ─── Bell: clean sine overtone with its own longer decay ───
        // Bell frequency at ~3.8× base (inharmonic, creates the "ting" sticking out)
        float bellFreq = baseFreq * (3.5f + p2 * 2.0f);  // pe1=PARTS doubles as bell ratio
        v.phase2 += bellFreq * dt * DS_TAU;
        float bellDecay = expDecay(v.t, decayRate * 0.35f);  // bell outlasts the wash
        float bell = sinf(v.phase2) * p2 * 0.40f * bellDecay;
        // Bell 2nd partial adds overtone richness
        float bell2 = sinf(v.phase2 * 2.18f) * p2 * 0.10f * bellDecay;

        // ─── Shimmer: AM tremolo at low rate + slight vibrato ───
        v.phase3 += (14.0f + p3 * 22.0f) * dt * DS_TAU;
        float am = 1.0f - p3 * 0.28f * sinf(v.phase3);
        float vibrato = 1.0f + p3 * 0.003f * sinf(v.phase3 * 0.37f);

        // ─── Sizzle: HP noise for the "shhh" tail ───
        float noiseRaw = v.noise();
        float sizzleCut = 0.08f + p4 * 0.20f;
        v.noiseState2 += sizzleCut * (noiseRaw - v.noiseState2);
        float sizzle = (noiseRaw - v.noiseState2)
                       * (0.12f + p5 * 0.20f)                  // p5=BODY: more sizzle
                       * expDecay(v.t, decayRate * 1.1f);

        // Extra inharmonic partial for complex wash (pe0=RNGMIX)
        float rm = sinf(v.phase * CYM_RATIOS[0])
                 * sinf(v.phase * CYM_RATIOS[3]) * (0.08f + pe0 * 0.20f);

        // ─── Attack: softer than hihat — stick strike not sharp pick ───
        float attEnv = expDecay(v.t, 130.0f);
        float att    = v.noise() * attEnv * (0.18f + pe3 * 0.28f);  // pe3=TRANS

        // Crisp note (pe5=CRSP): optional short HP burst
        float crsp = v.noise();
        v.noiseState3 += (0.22f + pe5 * 0.35f) * (crsp - v.noiseState3);
        float crspSig = (crsp - v.noiseState3) * pe5 * 0.10f * expDecay(v.t, decayRate * 1.4f);

        float env = expDecay(v.t, decayRate);
        sample = ((cymHP * am * vibrato) + bell + bell2 + rm * env + sizzle + att + crspSig)
                 * (0.28f + v.vel * 0.72f) * 0.50f;
        v.envA = env;
        break;
    }
    case DS_RIDE: {
        // p0=PITCH  p1=DECAY  p2=BELL  p3=STICK  p4=TONE  p5=WASH
        // pe0=BELLRNG pe1=BELLHM pe2=DAMP pe3=PING pe4=SUSTAIN pe5=EDGE
        // ═══ Ride cymbal ═══
        // Based on Mutable Instruments Plaits/Braids 6-oscillator metallic noise technique.
        // RIDE = prominent pitched bell + controlled 6-osc metallic wash + stick ping.

        // Base frequency for metallic noise (nominal ~414 Hz like TR-808)
        float metalF0 = (320.0f + p0 * 300.0f) / DS_SAMPLE_RATE;  // 320-620 Hz
        // 808/Plaits inharmonic ratios for metallic noise
        static const float MR[6] = {1.0f, 1.3420f, 1.5148f, 1.8510f, 2.0028f, 2.6834f};

        // ─── 6-oscillator metallic noise (square wave summation) ───
        // Accumulate 6 independent phases via v.t (linear time → free-running phases)
        // Each oscillator outputs +1 or -1 (square wave), sum creates metallic texture
        int metalNoise = 0;
        for (int i = 0; i < 6; ++i) {
            float ph = metalF0 * MR[i] * v.t * DS_SAMPLE_RATE;
            metalNoise += (ph - floorf(ph)) > 0.5f ? 1 : -1;
        }
        float metalRaw = metalNoise * (1.0f / 6.0f);  // normalize to ±1

        // Decay controls (rate for expDecay: ~3=1.5s, ~15=300ms at -40dB)
        float washDecay = 3.0f + (1.0f - p1) * 12.0f;
        float bellDecayRate = 1.5f + (1.0f - pe4) * 5.0f + washDecay * 0.3f;

        // ─── Bandpass filter the metallic noise (Plaits-style SVF) ───
        // p4=TONE: sweeps the coloration filter. pe2=DAMP: extra damping on wash.
        float bpCut = fminf(0.49f, (3500.0f + p4 * 6000.0f) / DS_SAMPLE_RATE);
        float bpQ   = 0.8f + pe5 * 1.8f;  // pe5=EDGE: resonance of wash filter
        float bpHP  = metalRaw - v.noiseState - bpQ * v.noiseState2;
        v.noiseState2 += bpCut * bpHP;
        v.noiseState  += bpCut * v.noiseState2;
        float metalBP = v.noiseState2;

        // Clocked noise mixed in for variety (Plaits technique)
        // noisiness controlled by pe2=DAMP; clock rate tracks metallic freq
        float nClk = metalF0 * (16.0f + 16.0f * (1.0f - pe2));
        if (nClk < 0.001f) nClk = 0.001f;
        v.phase3 += nClk;
        if (v.phase3 >= 1.0f) { v.phase3 -= 1.0f; v.prevSample = v.noise(); }
        float noisiness = pe2 * pe2;
        float metalFinal = metalBP + noisiness * (v.prevSample - metalBP);

        // Wash envelope + HP filter for ride tightness
        float washEnv = expDecay(v.t, washDecay * (1.0f + pe2 * 1.5f));
        float hpCut = fminf(0.45f, (1200.0f + p4 * 1800.0f) / DS_SAMPLE_RATE);
        v.noiseState3 += hpCut * (metalFinal - v.noiseState3);
        float washSig = (metalFinal - v.noiseState3) * washEnv * p5 * 0.55f;

        // ─── Bell: pitched sine cluster (ride bell is the star) ───
        float bellHz = 2800.0f + p0 * 2600.0f;  // 2800-5400 Hz
        v.phase  += bellHz * dt * DS_TAU;
        v.phase2 += bellHz * 1.0037f * dt * DS_TAU;  // slow beating
        float bellEnv = expDecay(v.t, bellDecayRate);
        float bell = (sinf(v.phase) * 0.45f + sinf(v.phase2) * 0.40f) * bellEnv;
        // Upper inharmonic overtone
        float bellOT = sinf(v.phase * 2.14f) * 0.20f * bellEnv;
        // pe1=BELLHM: harmonic complexity; pe0=BELLRNG: ring mod character
        float bellExt = sinf(v.phase * 3.07f) * pe1 * 0.12f * bellEnv;
        float bellRM  = sinf(v.phase) * sinf(v.phase * 2.14f) * pe0 * 0.15f * bellEnv;
        float bellSig = (bell + bellOT + bellExt + bellRM) * (0.25f + p2 * 0.75f);

        // ─── Stick transient: sharp metallic click ───
        float stickRate = 250.0f + (1.0f - p3) * 350.0f;
        float stickEnv  = expDecay(v.t, stickRate);
        float stickN    = v.noise();
        // Simple HP noise for click character
        v.noiseState4 += 0.22f * (stickN - v.noiseState4);
        float stickSig = (stickN - v.noiseState4) * stickEnv * p3 * 0.45f;
        // Ping element at bell frequency
        stickSig += sinf(v.phase * 1.5f) * stickEnv * p3 * pe3 * 0.18f;

        float env = expDecay(v.t, washDecay);
        sample = (bellSig + washSig + stickSig) * (0.28f + v.vel * 0.72f) * 0.50f;
        v.envA = fmaxf(env, bellEnv);
        break;
    }
    case DS_CRASH: {
        // p0=PITCH  p1=DECAY  p2=SPREAD  p3=SIZZLE  p4=TONE  p5=BODY
        // pe0=WASH pe1=BURST pe2=BRIGHT pe3=ATTACK pe4=TAIL pe5=TRASH
        // ═══ Crash cymbal ═══
        // Based on Mutable Instruments Plaits/Braids 6-oscillator metallic noise technique
        // plus Plaits RingModNoise for extra density. CRASH = explosive, wide, noise-dominant.

        // Base frequency for metallic noise — lower and wider than ride
        float metalF0 = (200.0f + p0 * 250.0f) / DS_SAMPLE_RATE;  // 200-450 Hz
        // Wider, more chaotic ratios than ride (spread from hi-hat 808 ratios)
        // p2=SPREAD: interpolates from tight (hi-hat-like) to wide (crash-like)
        static const float CR_TIGHT[6] = {1.0f, 1.3420f, 1.5148f, 1.8510f, 2.0028f, 2.6834f};
        static const float CR_WIDE[6]  = {1.0f, 1.4631f, 1.9405f, 2.5714f, 3.2318f, 4.1277f};

        // Decay controls (rate for expDecay: ~2=2.3s, ~12=380ms at -40dB)
        float decayRate = 2.0f + (1.0f - p1) * 10.0f;
        decayRate *= fmaxf(0.40f, 1.0f - pe4 * 0.50f);  // pe4=TAIL

        // ─── 6-oscillator metallic noise ───
        int metalNoise = 0;
        for (int i = 0; i < 6; ++i) {
            float r = CR_TIGHT[i] + (CR_WIDE[i] - CR_TIGHT[i]) * p2;
            float ph = metalF0 * r * v.t * DS_SAMPLE_RATE;
            metalNoise += (ph - floorf(ph)) > 0.5f ? 1 : -1;
        }
        float metalRaw = metalNoise * (1.0f / 6.0f);

        // ─── Ring mod noise layer (Plaits RingModNoise style) ───
        // 3 pairs of sine oscillators ring-modulated for extra density
        float rmRat = metalF0 / (0.01f + metalF0);
        float f1a = 200.0f / DS_SAMPLE_RATE * rmRat;
        float f1b = 7530.0f / DS_SAMPLE_RATE * rmRat;
        float f2a = 510.0f / DS_SAMPLE_RATE * rmRat;
        float f2b = 8075.0f / DS_SAMPLE_RATE * rmRat;
        float rmPair1 = sinf(f1a * v.t * DS_SAMPLE_RATE * DS_TAU) * sinf(f1b * v.t * DS_SAMPLE_RATE * DS_TAU);
        float rmPair2 = sinf(f2a * v.t * DS_SAMPLE_RATE * DS_TAU) * sinf(f2b * v.t * DS_SAMPLE_RATE * DS_TAU);
        float rmNoise = (rmPair1 + rmPair2) * pe0 * 0.30f;  // pe0=WASH

        // ─── Bandpass filter the metallic noise (SVF) ───
        float bpCut = fminf(0.49f, (1500.0f + p4 * 5000.0f + pe2 * 3000.0f) / DS_SAMPLE_RATE);
        float bpQ   = 0.5f + p3 * 1.5f;  // p3=SIZZLE: higher = more resonant
        float metalIn = metalRaw + rmNoise;
        float bpHP  = metalIn - v.noiseState - bpQ * v.noiseState2;
        v.noiseState2 += bpCut * bpHP;
        v.noiseState  += bpCut * v.noiseState2;
        float metalBP = v.noiseState2;

        // Clocked noise for variety (Plaits technique)
        float nClk = metalF0 * (16.0f + 16.0f * (1.0f - p3));
        if (nClk < 0.001f) nClk = 0.001f;
        v.phase3 += nClk;
        if (v.phase3 >= 1.0f) { v.phase3 -= 1.0f; v.prevSample = v.noise(); }
        float noisiness = p3 * p3;
        float metalFinal = metalBP + noisiness * (v.prevSample - metalBP);

        // Per-partial decay (higher partials die first for natural evolution)
        float metalEnv = expDecay(v.t, decayRate);
        float metalSig = metalFinal * metalEnv * 0.65f;

        // ─── Sizzle: dense HP filtered noise (the "shhhhh") ───
        float n1 = v.noise();
        float n2 = v.noise();
        float sizzleCut = 0.05f + p3 * 0.15f + pe2 * 0.10f;
        v.noiseState3 += sizzleCut * (n1 - v.noiseState3);
        float sizzleHP = n1 - v.noiseState3;
        // Second noise band for body
        float bodyCut = 0.02f + p4 * 0.06f;
        v.noiseState4 += bodyCut * (n2 - v.noiseState4);
        float bodyHP = n2 - v.noiseState4;

        float sizzleEnv = expDecay(v.t, decayRate * 0.80f);  // sizzle outlasts metal
        float noiseSig = (sizzleHP * (0.22f + p3 * 0.38f) + bodyHP * p5 * 0.20f) * sizzleEnv;

        // ─── Explosive attack burst ───
        float burstRate = 180.0f + (1.0f - pe3) * 340.0f;  // pe3=ATTACK
        float burstEnv  = expDecay(v.t, burstRate);
        float burstSig  = v.noise() * burstEnv * (0.30f + pe1 * 0.50f);  // pe1=BURST
        // Swept sine impact
        float burstFreq = metalF0 * DS_SAMPLE_RATE * 5.0f * (1.0f + 4.0f * expDecay(v.t, 220.0f));
        v.phase += burstFreq * dt * DS_TAU;
        float burstPing = sinf(v.phase) * burstEnv * 0.18f * pe1;

        // Shimmer AM
        v.phase2 += (8.0f + p3 * 14.0f) * dt * DS_TAU;
        float shimmer = 1.0f - p3 * 0.18f * sinf(v.phase2);

        // pe5=TRASH: saturation
        float sig = metalSig * shimmer + noiseSig + burstSig + burstPing;
        if (pe5 > 0.01f) {
            float drv = 1.0f + pe5 * 5.0f;
            sig = tanhf(sig * drv) / tanhf(drv);
        }

        // Body: low-end presence (p5=BODY)
        float bodyLP = v.fltLP * 0.93f + sig * 0.07f;
        v.fltLP = bodyLP;
        sig += bodyLP * p5 * 0.25f;

        float env = expDecay(v.t, decayRate);
        sample = sig * (0.24f + v.vel * 0.76f) * 0.54f;
        v.envA = fmaxf(env, sizzleEnv);
        break;
    }
    case DS_SHAKER: {
        // p0=PITCH  p1=DECAY  p2=TONE  p3=DENSITY  p4=DRIVE  p5=SHAKE
        // pe0=GRNSZ pe1=GRNVAR pe2=BRGHT pe3=RESON pe4=DUST pe5=BODY
        // ─── Granular noise engine: filtered noise bursts with grain modulation ───
        // Professional shaker/maracas/cabasa — NOT metallic oscillators.
        // Each "grain" is a short burst of bandpass-filtered noise.
        // Multiple grains overlap at controllable density for the rattle quality.

        float filterCenter = 3500.0f + p0 * 9500.0f;  // 3.5-13 kHz (shakers are high)
        float decayRate = 4.0f + (1.0f - p1) * 35.0f;

        // ─── Generate filtered noise ───
        // SVF bandpass — p2=TONE controls bandwidth (low=narrow, high=wide)
        float bpF = 2.0f * sinf(3.14159f * fminf(filterCenter / DS_SAMPLE_RATE, 0.45f));
        float bpQ = 0.15f + (1.0f - p2) * 0.70f;  // resonance inversely prop to tone
        // pe3=RESON: extra resonance boost
        bpQ = fmaxf(0.05f, bpQ * (1.0f - pe3 * 0.65f));

        float n1 = v.noise();
        float svfHP = n1 - v.noiseState2 - bpQ * v.noiseState;
        v.noiseState  += bpF * svfHP;
        v.noiseState2 += bpF * v.noiseState;
        float bpSig = v.noiseState;  // bandpass output

        // Secondary HP for brightness/air (pe2=BRGHT)
        float n2 = v.noise();
        float airCut = 0.06f + pe2 * 0.25f;
        v.noiseState3 += airCut * (n2 - v.noiseState3);
        float airSig = (n2 - v.noiseState3) * pe2 * 0.35f;

        // Dust: random sparse crackle (pe4=DUST)
        float dustSig = 0.0f;
        if (pe4 > 0.01f) {
            float dustN = v.noise();
            dustSig = (fabsf(dustN) > (1.0f - pe4 * 0.35f)) ? dustN * 0.20f : 0.0f;
        }

        // ─── Grain amplitude modulation: creates the rattle/shaking quality ───
        // Two non-integer-ratio modulators create irregular density pattern
        float grainRate = 25.0f + p3 * 220.0f;  // 25-245 Hz modulation
        float grainSize = 0.3f + pe0 * 0.65f;   // pe0=GRNSZ: 0=tiny grains, 1=long grains
        v.phase  += grainRate * dt * DS_TAU;
        v.phase2 += grainRate * 1.618f * dt * DS_TAU;  // golden ratio for irregularity
        // pe1=GRNVAR: randomness/variance in grain timing
        v.phase3 += grainRate * (0.73f + pe1 * 0.8f) * dt * DS_TAU;

        // Half-wave rectify + threshold for grain-like bursts
        float mod1 = fmaxf(0.0f, sinf(v.phase));
        float mod2 = fmaxf(0.0f, sinf(v.phase2));
        float mod3 = fmaxf(0.0f, sinf(v.phase3));
        // Grain envelope: combine modulators, shape with size param
        float grainEnv = fmaxf(mod1, fmaxf(mod2 * 0.75f, mod3 * 0.55f));
        grainEnv = powf(grainEnv, 0.3f + (1.0f - grainSize) * 2.5f);  // shape steepness

        // p5=SHAKE: blends grain modulation depth (0=constant, 1=deep modulation)
        float grainMod = (1.0f - p5) + p5 * grainEnv;

        // p5=BODY: adds a low sub-layer filtered noise for body/weight
        float n3 = v.noise();
        v.noiseState4 += 0.015f * (n3 - v.noiseState4);
        float bodySig = v.noiseState4 * pe5 * 0.30f;

        float sig = (bpSig * 0.65f + airSig + dustSig + bodySig) * grainMod;

        // Drive
        if (p4 > 0.01f) {
            float drv = 1.0f + p4 * 7.0f;
            sig = tanhf(sig * drv) / tanhf(drv);
        }

        float env = expDecay(v.t, decayRate);
        sample = sig * env * (0.32f + v.vel * 0.68f) * 0.50f;
        v.envA = env;
        break;
    }
    case DS_COWBELL: {
        // p0=PITCH  p1=DECAY  p2=TONE  p3=ATTACK  p4=DRIVE  p5=RING
        // pe0=RATIO pe1=BRGHT pe2=HARM pe3=HIT pe4=SYM pe5=TAIL
        // ─── Classic 808/909 analogue cowbell ───
        // Two square-wave oscillators at a fixed non-octave interval.
        // The original TR-808 used 587 Hz and 845 Hz (~1.44 ratio).
        // Hard limiting creates the characteristic metallic, hollow tone.

        float baseFreq = 420.0f + p0 * 520.0f;  // 420-940 Hz (cowbell range)
        // Two-stage decay: fast initial drop + sustained ring
        float fastDecay = 28.0f + (1.0f - p1) * 55.0f;
        float slowDecay = 6.0f + (1.0f - p1) * 16.0f;
        float tailMix = 0.25f + pe5 * 0.45f;  // pe5=TAIL: blend fast/slow
        float decayRate = fastDecay * (1.0f - tailMix) + slowDecay * tailMix;

        // ─── Two-oscillator metallic tone ───
        // pe0=RATIO fine-tunes the interval (classic 808 = ~1.44)
        float ratio = 1.34f + pe0 * 0.28f;  // 1.34-1.62 ratio range
        float f2 = baseFreq * ratio;
        v.phase  += baseFreq * dt * DS_TAU;
        v.phase2 += f2 * dt * DS_TAU;

        // Hard limiting — the key to cowbell character
        // p2=TONE controls clip level (lower = harsher & more metallic)
        float clipLvl = 0.04f + p2 * 0.50f;
        auto clip = [&](float x) -> float {
            return fmaxf(-clipLvl, fminf(clipLvl, x)) / clipLvl;
        };
        float osc1 = clip(sinf(v.phase));
        float osc2 = clip(sinf(v.phase2));
        // Mix: slight emphasis on lower osc (808-style)
        float sig = osc1 * 0.55f + osc2 * 0.45f;

        // pe2=HARM: additional harmonic from sum/difference frequencies
        float harmSig = sinf(v.phase + v.phase2) * pe2 * 0.15f
                      + sinf(v.phase * 2.0f) * pe2 * 0.08f;

        // pe4=SYM: asymmetric waveshaping for warmth
        sig += pe4 * 0.22f * sig * sig;

        // pe1=BRGHT: HP filter to make it brighter/thinner
        if (pe1 > 0.01f) {
            float hpCut = pe1 * 0.18f;
            v.noiseState += hpCut * (sig - v.noiseState);
            sig = sig - v.noiseState;
        }

        // Drive
        if (p4 > 0.01f) {
            float drv = 1.0f + p4 * 9.0f;
            sig = tanhf(sig * drv) / tanhf(drv);
        }

        // ─── Attack: sharp beater click ───
        float atkSpeed = 120.0f + (1.0f - p3) * 280.0f;
        float atkEnv = expDecay(v.t, atkSpeed);
        float atkNoise = v.noise() * atkEnv * p3 * 0.30f;
        // Pitched click at high harmonic of base
        v.phase3 += (baseFreq * 3.5f) * dt * DS_TAU;
        float atkClick = sinf(v.phase3) * atkEnv * p3 * 0.15f;
        // pe3=HIT: extra transient noise burst
        float hitBurst = v.noise() * pe3 * 0.22f * expDecay(v.t, 350.0f);

        // ─── Ring: pitched resonance tail (p5=RING) ───
        float ringFreq = baseFreq * (2.1f + p5 * 2.8f);
        v.noiseState2 += ringFreq * dt * DS_TAU;  // repurpose as phase acc
        float ringEnv = expDecay(v.t, decayRate * 0.45f);
        float ring = sinf(v.noiseState2 * DS_TAU) * p5 * 0.14f * ringEnv;

        // Envelope: 808-style — fast attack, shaped decay
        float env = expDecay(v.t, decayRate);
        // Initial transient envelope (slightly boosts the first few ms)
        float transEnv = 1.0f + 0.35f * expDecay(v.t, 180.0f);

        sample = (sig * env * transEnv + harmSig * env + atkNoise + atkClick + hitBurst + ring)
                 * (0.38f + v.vel * 0.62f) * 0.52f;
        v.envA = env;
        break;
    }
    default:
        v.active = false;
        return 0.0f;
    }

    // ── Post-processing (page 2 params) ──────────────────────────
    {
        float pFilt  = v.par.p[12] / 127.0f;  // FILTER (LP cutoff)
        float pReso  = v.par.p[13] / 127.0f;  // RESO
        float pComp  = v.par.p[14] / 127.0f;  // COMP
        float pSat   = v.par.p[15] / 127.0f;  // SATUR
        float pBite  = v.par.p[16] / 127.0f;  // BITE
        float pLevel = v.par.p[17] / 127.0f;  // LEVEL

        // Saturation FIRST (before filter, so filter smooths distortion edges)
        if (pSat > 0.01f) {
            float drv = 1.0f + pSat * 14.0f;
            // Asymmetric warmth: add subtle even harmonics
            float asym = sample + 0.1f * pSat * sample * sample;
            sample = tanhf(asym * drv) / tanhf(drv);
        }

        // Low-pass filter (2-pole SVF with resonant peak)
        if (pFilt < 0.98f) {
            // Quadratic mapping for more useful low-end control
            float cutoff = 0.003f + pFilt * pFilt * 0.45f;
            float f = 2.0f * sinf(3.14159f * fminf(cutoff, 0.45f));
            float q = 1.0f - pReso * 0.92f;
            float hp = sample - v.fltLP - q * v.fltBP;
            v.fltBP += f * hp;
            v.fltLP += f * v.fltBP;
            // Mix LP with a touch of BP for resonant character
            sample = v.fltLP + v.fltBP * pReso * 0.25f;
        }

        // Compression (program-dependent, with attack/release behavior)
        if (pComp > 0.01f) {
            float thresh = 1.0f - pComp * 0.65f;
            float ratio = 1.0f + pComp * 6.0f;    // 1:1 → 1:7
            float absS = fabsf(sample);
            if (absS > thresh && absS > 0.001f) {
                float over = absS - thresh;
                float gain = thresh + over / ratio;
                sample *= gain / absS;
            }
            // Makeup gain scales with ratio
            sample *= 1.0f + pComp * 0.5f;
        }

        // Bit reduction / lo-fi
        if (pBite > 0.01f) {
            // Combine bit crushing with sample-rate reduction feel
            float bits = 16.0f - pBite * 13.0f;    // 16 → 3 bits
            float levels = powf(2.0f, bits);
            sample = roundf(sample * levels) / levels;
            // Subtle fold-back at extreme settings for extra grit
            if (pBite > 0.7f) {
                float fold = (pBite - 0.7f) * 3.33f; // 0→1 over last 30%
                sample = wavefold(sample, fold * 0.3f);
            }
        }

        // Level (with gentle saturation ceiling)
        float gain = pLevel * 1.3f;
        sample *= gain;
        // Soft clip to prevent harsh digital clipping
        if (fabsf(sample) > 1.0f) {
            sample = tanhf(sample);
        }
    }

    // ── Page 3 FX: Reverb, Delay, Chorus ─────────────────────────────
    {
        float pReverb = v.par.p[18] / 127.0f;
        float pRevSz  = v.par.p[19] / 127.0f;
        float pDelay  = v.par.p[20] / 127.0f;
        float pDTime  = v.par.p[21] / 127.0f;
        float pChorus = v.par.p[22] / 127.0f;
        float pChSpd  = v.par.p[23] / 127.0f;

        float wet = 0.0f;

        // ── Delay (echo) ───
        if (pDelay > 0.01f) {
            int delaySamples = 441 + (int)(pDTime * (DS_DLY_MAX - 441)); // 10ms-200ms
            v.dlyBuf[v.dlyPos] = sample;
            int readPos = (v.dlyPos - delaySamples + DS_DLY_MAX) % DS_DLY_MAX;
            float delayed = v.dlyBuf[readPos];
            float feedback = 0.2f + pDTime * 0.5f;
            v.dlyBuf[v.dlyPos] += delayed * feedback;
            v.dlyPos = (v.dlyPos + 1) % DS_DLY_MAX;
            wet += delayed * pDelay * 0.7f;
        }

        // ── Reverb (Freeverb-style: 4 parallel comb filters → 2 series allpass) ───
        if (pReverb > 0.01f) {
            float input = sample;
            float feedback = 0.75f + pRevSz * 0.20f; // 0.75-0.95 (long tail)
            float damp = 0.3f + (1.0f - pRevSz) * 0.4f; // LP damping in combs

            // Helper macro: comb filter with LP-damped feedback
            #define DS_COMB(buf, sz, pos, lpSt) do { \
                float rd = buf[pos]; \
                lpSt += damp * (rd - lpSt); \
                buf[pos] = input + lpSt * feedback; \
                pos = (pos + 1) % sz; \
                combSum += rd; \
            } while(0)

            float combSum = 0.0f;
            DS_COMB(v.comb1, DS_COMB1_SZ, v.combPos1, v.combLP1);
            DS_COMB(v.comb2, DS_COMB2_SZ, v.combPos2, v.combLP2);
            DS_COMB(v.comb3, DS_COMB3_SZ, v.combPos3, v.combLP3);
            DS_COMB(v.comb4, DS_COMB4_SZ, v.combPos4, v.combLP4);
            #undef DS_COMB

            combSum *= 0.25f; // average the 4 combs

            // Allpass diffuser 1
            {
                float rd = v.rap1[v.rapPos1];
                float wr = combSum + rd * 0.5f;
                v.rap1[v.rapPos1] = wr;
                combSum = rd - wr * 0.5f;
                v.rapPos1 = (v.rapPos1 + 1) % DS_RAP1_SZ;
            }
            // Allpass diffuser 2
            {
                float rd = v.rap2[v.rapPos2];
                float wr = combSum + rd * 0.5f;
                v.rap2[v.rapPos2] = wr;
                combSum = rd - wr * 0.5f;
                v.rapPos2 = (v.rapPos2 + 1) % DS_RAP2_SZ;
            }

            wet += combSum * pReverb * 0.7f;
        }

        // ── Chorus (modulated delay) ───
        if (pChorus > 0.01f) {
            v.chorusBuf[v.chorusPos] = sample;
            float rate = 0.5f + pChSpd * 5.0f; // 0.5-5.5 Hz
            v.chorusPhase += rate * dt;
            float modDepth = 10.0f + pChorus * 40.0f; // 10-50 samples
            float modDelay = 200.0f + modDepth * (1.0f + sinf(v.chorusPhase * DS_TAU));

            int readI = (int)modDelay;
            if (readI >= DS_CHO_SZ - 1) readI = DS_CHO_SZ - 2;
            float frac = modDelay - (float)readI;
            int pos1 = (v.chorusPos - readI + DS_CHO_SZ) % DS_CHO_SZ;
            int pos2 = (pos1 - 1 + DS_CHO_SZ) % DS_CHO_SZ;
            float chorSample = v.chorusBuf[pos1] * (1.0f - frac) + v.chorusBuf[pos2] * frac;

            v.chorusPos = (v.chorusPos + 1) % DS_CHO_SZ;
            wet += chorSample * pChorus * 0.45f;
        }

        sample += wet;

        // Final clip after FX
        if (fabsf(sample) > 1.2f) sample = tanhf(sample);
    }

    v.t += dt;

    // Kill voice when envelope is effectively silent and FX tails have faded
    if (v.envA < 0.001f && v.t > 0.01f) {
        bool hasFX = (v.par.p[18] > 1 || v.par.p[20] > 1 || v.par.p[22] > 1);
        if (!hasFX || v.t > 3.0f || fabsf(sample) < 0.0001f) {
            v.active = false;
            return 0.0f;
        }
    }

    return sample;
}

#endif // DRUM_SYNTH_IMPL
