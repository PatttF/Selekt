// SELEKT — Drum Machine MIDI Sequencer  –  OpenGL + Dear ImGui + RtMidi + Built-in Drum Synth
// v5: built-in audio synthesis via miniaudio — works without external MIDI synth
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

#include "imgui.h"
#ifdef __ANDROID__
#include "imgui_impl_android.h"
struct android_app;
struct android_app* g_App = nullptr;
#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#else
#include "imgui_impl_glfw.h"
#endif
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"


#ifndef __ANDROID__
#include <GLFW/glfw3.h>
#endif


#ifdef __ANDROID__
#include <string>
#include <vector>
// Stub RtMidi for Android
class RtMidiOut {
public:
    void openPort(unsigned int) {}
    void closePort() {}
    void sendMessage(std::vector<unsigned char>*) {}
    unsigned int getPortCount() { return 0; }
    std::string getPortName(unsigned int) { return ""; }
    void openVirtualPort(std::string) {}
};
class RtMidiIn {
public:
    void openPort(unsigned int) {}
    void closePort() {}
    void setCallback(void(*)(double, std::vector<unsigned char>*, void*), void*) {}
    void ignoreTypes(bool, bool, bool) {}
    unsigned int getPortCount() { return 0; }
    std::string getPortName(unsigned int) { return ""; }
    void openVirtualPort(std::string) {}
};
#else
#include "RtMidi.h"
#endif


#define DRUM_SYNTH_IMPL
#include "drum_synth.h"

#include "RobotoMono_Regular.h"
#include "RobotoMono_Bold.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>

static double get_time_sec() {
    static auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}


// ═══════════════════════════════════════════════════════════════════════
static constexpr int   WIN_W      = 1280;
static constexpr int   WIN_H      = 800;
static constexpr int   NUM_TRACKS = 16;
static constexpr int   NUM_STEPS  = 16;         // steps per page
static constexpr int   MAX_STEPS  = 128;        // max total steps (8 pages)

// ═══════════════════════════════════════════════════════════════════════
//  Colour helpers
// ═══════════════════════════════════════════════════════════════════════
static ImU32 im(int r,int g,int b,int a=255){ return IM_COL32(r,g,b,a); }

static const ImU32 TCLR[NUM_TRACKS] = {
    im(220, 75, 75), im(230,150, 80), im(220,200, 60), im(110,200, 75),
    im( 60,195,140), im( 55,185,210), im( 70,140,230), im(140, 80,220),
    im(200, 75,200), im(220, 80,140), im(240,165, 55), im( 55,190,240),
    im(230,110, 90), im( 90,225,110), im( 95,130,240), im(230,220, 80),
};

static ImU32 dimC(ImU32 c, float t) {
    int r = std::min(255,(int)((c & 0xFF) * t));
    int g = std::min(255,(int)(((c >> 8) & 0xFF) * t));
    int b = std::min(255,(int)(((c >> 16) & 0xFF) * t));
    int a = (c >> 24) & 0xFF;
    return IM_COL32(r, g, b, a);
}
static ImU32 lerpC(ImU32 a, ImU32 b, float t) {
    int ar=a&0xFF, ag=(a>>8)&0xFF, ab=(a>>16)&0xFF;
    int br=b&0xFF, bg=(b>>8)&0xFF, bb=(b>>16)&0xFF;
    return IM_COL32(std::min(255,(int)(ar+(br-ar)*t)),
                    std::min(255,(int)(ag+(bg-ag)*t)),
                    std::min(255,(int)(ab+(bb-ab)*t)),255);
}
static ImU32 withA(ImU32 c, int a) {
    return (c & 0x00FFFFFF) | ((ImU32)a << 24);
}

// ═══════════════════════════════════════════════════════════════════════
//  Note helpers
// ═══════════════════════════════════════════════════════════════════════
static const char* NNAMES[12]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
static std::string noteName(int n) {
    n = std::clamp(n, 0, 127);
    char b[16]; snprintf(b, sizeof(b), "%s%d", NNAMES[n%12], n/12-1);
    return b;
}

// ═══════════════════════════════════════════════════════════════════════
//  MIDI
// ═══════════════════════════════════════════════════════════════════════
struct MidiEv { uint8_t st=0, d1=0, d2=0; };

class MidiPorts {
public:
    MidiPorts() {
        try { out_ = std::make_unique<RtMidiOut>(); } catch (...) {}
        try {
            in_ = std::make_unique<RtMidiIn>();
            in_->setCallback(inCB, this);
            in_->ignoreTypes(false, true, true);
        } catch (...) {}
        refresh();
        // Default to no output (user must explicitly select a MIDI device)
    }
    ~MidiPorts() { closeIn(); closeOut(); }
    void refresh() {
        inNames_.clear(); outNames_.clear(); inPortMap_.clear();
        if (out_) for (unsigned i = 0; i < out_->getPortCount(); ++i) outNames_.push_back(out_->getPortName(i));
        if (in_) {
            for (unsigned i = 0; i < in_->getPortCount(); ++i) {
                std::string name = in_->getPortName(i);
                if (name.find("SELEKT") != std::string::npos) continue;  // skip own virtual port
                inPortMap_.push_back(i);
                inNames_.push_back(name);
            }
        }
    }
    const std::vector<std::string>& outPorts() const { return outNames_; }
    const std::vector<std::string>& inPorts()  const { return inNames_; }
    int  curOut() const { return curOut_; }
    int  curIn()  const { return curIn_; }
    bool outOpen() const { return outOk_; }
    bool inOpen()  const { return inOk_; }
    // Synth engine only active when no real hardware port is open
    bool synthEnabled() const { return curOut_ == -1; } // only when no output selected; real port (>=0) and virtual (-2) disable synth
    std::string outName() const {
        if (curOut_ == -2) return "Virtual";
        if (curOut_ >= 0 && curOut_ < (int)outNames_.size()) return outNames_[curOut_];
        return "---";
    }
    std::string inName() const {
        if (curIn_ >= 0 && curIn_ < (int)inNames_.size()) return inNames_[curIn_];
        return "---";
    }
    bool openOut(int i) {
        closeOut();
        if (!out_ || i < 0 || i >= (int)outNames_.size()) return false;
        try { out_->openPort((unsigned)i); curOut_ = i; outOk_ = true; return true; } catch (...) { return false; }
    }
    void openVirtual() {
        closeOut(); if (!out_) return;
        try { out_->openVirtualPort("SELEKT"); curOut_ = -2; outOk_ = true; } catch (...) {}
    }
    void closeOut() { if (outOk_ && out_) { try { out_->closePort(); } catch (...) {} outOk_ = false; curOut_ = -1; } }
    bool openIn(int i) {
        closeIn();
        if (!in_ || i < 0 || i >= (int)inNames_.size()) return false;
        unsigned realIdx = inPortMap_[i];
        try { in_->openPort(realIdx); curIn_ = i; inOk_ = true; return true; } catch (...) { return false; }
    }
    void closeIn() { if (inOk_ && in_) { try { in_->closePort(); } catch (...) {} inOk_ = false; curIn_ = -1; } }
    void noteOn(int ch, int n, int v) { sendMsg({(uint8_t)(0x90|(ch&0xF)),(uint8_t)(n&0x7F),(uint8_t)(v&0x7F)}); }
    void noteOff(int ch, int n) { sendMsg({(uint8_t)(0x80|(ch&0xF)),(uint8_t)(n&0x7F),0}); }
    void sendCC(int ch, int cc, int v) { sendMsg({(uint8_t)(0xB0|(ch&0xF)),(uint8_t)(cc&0x7F),(uint8_t)(v&0x7F)}); }
    void sendPC(int ch, int p) { sendMsg({(uint8_t)(0xC0|(ch&0xF)),(uint8_t)(p&0x7F)}); }
    void sendNRPN(int ch, int nmsb, int nlsb, int val) {
        sendCC(ch, 99, nmsb & 0x7F);
        sendCC(ch, 98, nlsb & 0x7F);
        sendCC(ch, 6,  val  & 0x7F);
    }
    void sendRaw(uint8_t b) { sendMsg({b}); }
    std::vector<MidiEv> drain() {
        std::lock_guard<std::mutex> lk(mtx_);
        auto r = std::move(q_); q_.clear(); return r;
    }
private:
    void sendMsg(std::vector<uint8_t> m) {
        if (!outOk_ || !out_) return;
        try { out_->sendMessage(&m); } catch (...) {}
    }
    static void inCB(double, std::vector<uint8_t>* m, void* ud) {
        if (!m || m->empty()) return;
        auto* s = static_cast<MidiPorts*>(ud);
        MidiEv e{(*m)[0], m->size()>1?(*m)[1]:(uint8_t)0, m->size()>2?(*m)[2]:(uint8_t)0};
        std::lock_guard<std::mutex> lk(s->mtx_);
        s->q_.push_back(e);
    }
    std::unique_ptr<RtMidiOut> out_;
    std::unique_ptr<RtMidiIn>  in_;
    std::vector<std::string> outNames_, inNames_;
    std::vector<unsigned> inPortMap_;  // maps filtered index → real RtMidi port index
    int curOut_ = -1, curIn_ = -1;
    bool outOk_ = false, inOk_ = false;
    std::mutex mtx_;
    std::vector<MidiEv> q_;
};

// ═══════════════════════════════════════════════════════════════════════
//  Data model  — now with probability & ratchet per step
// ═══════════════════════════════════════════════════════════════════════
struct Step {
    bool on       = false;
    int  vel      = 100;
    int  prob     = 100;   // 0-100% probability
    int  ratchet  = 1;     // 1-4 ratchet subdivisions
};
// ── Drum synth types & parameter names ───────────────────────────────
static constexpr int NUM_SYNTH_PARAMS = 114; // 19 pages × 6
static constexpr int SYNTH_PARAMS_PER_PAGE = 6;
enum DrumType { DT_KICK=0, DT_SNARE, DT_HIHAT, DT_TOM, DT_CLAP, DT_PERC, DT_CYMBAL, DT_SHAKER, DT_COWBELL, DT_RIDE, DT_CRASH, DT_COUNT };

// Page 1 param names (per drum type — core synthesis)
static const char* SYNTH_PNAMES[DT_COUNT][SYNTH_PARAMS_PER_PAGE] = {
    {"PITCH","DECAY","DRIVE","CLICK","SUB","PUNCH"},      // KICK
    {"PITCH","DECAY","SNAP","NOISE","TONE","RING"},       // SNARE
    {"PITCH","DECAY","COLOR","METAL","TONE","OPEN"},      // HIHAT
    {"PITCH","DECAY","TONE","ATTACK","BODY","RING"},      // TOM
    {"SPREAD","DECAY","TONE","ROOM","DRIVE","WIDTH"},     // CLAP
    {"PITCH","DECAY","TONE","ATTACK","DRIVE","BODY"},     // PERC
    {"PITCH","DECAY","BELL","SHIMMER","TONE","BODY"},     // CYMBAL
    {"PITCH","DECAY","TONE","DENSTY","DRIVE","SHAKE"},    // SHAKER
    {"PITCH","DECAY","TONE","ATTACK","DRIVE","RING"},     // COWBELL
    {"PITCH","DECAY","BELL","STICK","TONE","WASH"},       // RIDE
    {"PITCH","DECAY","SPREAD","SIZZLE","TONE","BODY"},    // CRASH
};
// Page 2 param names (per drum type — extended synthesis controls)
static const char* SYNTH_PNAMES_EXT[DT_COUNT][SYNTH_PARAMS_PER_PAGE] = {
    {"HARM", "SWEEP","SHAPE","CLICK2","SYM",  "BODY2"},  // KICK
    {"RNGAMT","RNGFRQ","RATTL","CRACK", "BODY2","WIRE"},  // SNARE
    {"RNGMIX","PARTS","BRGHT","HIT",   "SHADE","HISS"},  // HIHAT
    {"RNGAMT","RNGFRQ","MODES","SHELL", "PITCH2","SYM"},  // TOM
    {"BURST","RESON","BRGHT","SLAP",   "CRACK","SRC"},   // CLAP
    {"FMRAT","FMDEP","FOLD","PMOD",    "HARM2","SYM"},   // PERC
    {"RNGMIX","PARTS","SHIMSP","TRANSI","TAIL","CRSP"},  // CYMBAL
    {"GRNSZ","GRNVAR","BRGHT","RESON", "DUST","BODY"},   // SHAKER
    {"RATIO","BRGHT","HARM", "HIT",   "SYM", "TAIL"},    // COWBELL
    {"BELLRN","BELLHM","DAMP","PING",  "SUSTN","EDGE"},   // RIDE
    {"WASH", "BURST","BRGHT","ATTACK", "TAIL","TRASH"},  // CRASH
};
// Page 3 param names (same for all — shaping/FX)
static const char* SYNTH_PNAMES_P2[SYNTH_PARAMS_PER_PAGE] = {
    "FILTER","RESO","COMP","SATUR","BITE","LEVEL"
};
// Page 4 param names (same for all — FX2/spatial effects)
static const char* SYNTH_PNAMES_P3[SYNTH_PARAMS_PER_PAGE] = {
    "REVERB","REV SZ","DELAY","D.TIME","CHORUS","CH.SPD"
};
// synth param accent colours per type (subtle tints for slider fills)
static const ImU32 SYNTH_ACCENT[DT_COUNT] = {
    IM_COL32(220,80,60,255),    // KICK     – warm red
    IM_COL32(230,180,70,255),   // SNARE    – amber
    IM_COL32(180,210,60,255),   // HIHAT    – lime
    IM_COL32(70,190,180,255),   // TOM      – teal
    IM_COL32(210,80,180,255),   // CLAP     – magenta
    IM_COL32(100,160,230,255),  // PERC     – sky
    IM_COL32(200,190,80,255),   // CYMBAL   – gold
    IM_COL32(90,200,140,255),   // SHAKER   – mint
    IM_COL32(230,140,60,255),   // COWBELL  – orange
    IM_COL32(160,200,220,255),  // RIDE     – silver
    IM_COL32(240,220,90,255),   // CRASH    – bright gold
};
// CC numbers for each synth param slot (Sound Controller 1-12)
static constexpr int SYNTH_CC[NUM_SYNTH_PARAMS] = {
    70,71,72,73,74,75,  // page 1
    76,77,78,79,80,81,  // page 2
    82,83,84,85,86,87,  // page 3
    88,89,90,91,92,93,  // page 4
    94,95,96,97,98,99,  // page 5 (generic extra)
   100,101,102,103,104,105  // page 6 (generic extra)
};

// Circuit Tracks Synth 1/2 CC tables — 9 pages × 6 params each (pages 9-18 use NRPN)
// All send on MIDI ch 1 (SY1) or ch 2 (SY2), except FX page slots 2-5 send on ch16
static constexpr int CT_SYNTH_CC[9][6] = {
    { 19, 20, 21, 22, 24, 26},  // pg0: OSC1 wave,interp,pw,vsync,density,semi
    { 29, 30, 31, 33, 35, 37},  // pg1: OSC2 wave,interp,pw,vsync,density,semi
    { 68, 74, 71, 63, 69, 79},  // pg2: FILTER type,freq,res,drive,track,e2freq
    { 73, 75, 70, 72,108, 5 },  // pg3: ENV atk,dec,sus,rel,vel,porta
    { 51, 52, 54, 56, 58, 59},  // pg4: MIXER o1lvl,o2lvl,ring,noise,preFX,postFX
    {  3,  9, 13, 60, 78, 65},  // pg5: VOICE poly,preglide,kbdoct,fltroute,Qnorm,drvtype
    { 91, 93, 88,111, 74, 71},  // pg6: FX dist,chorus,rvbSnd(SY1=88/SY2=89),dlySnd(SY1=111/SY2=112),mfxFreq,mfxRes
    { 80, 81, 82, 83, 84, 85},  // pg7: MACRO macroKnobs 1-6 positions
    { 25, 27, 28, 36, 39, 40},  // pg8: TUNE o1detune,o1cents,o1pb,o2detune,o2cents,o2pb
};
static const char* CT_SYNTH_PNAMES[9][6] = {
    {"O1WAVE","O1INTP","O1 PW","O1VSN","O1DNS","O1SEMI"},  // OSC1
    {"O2WAVE","O2INTP","O2 PW","O2VSN","O2DNS","O2SEMI"},  // OSC2
    {"FTYPE","FREQ","RES","DRIVE","TRACK","E2FRQ"},         // FILTER
    {"ATTK","DECAY","SUST","REL","E1VEL","PORTA"},          // ENV
    {"O1LVL","O2LVL","RING","NOISE","PREFX","PSTFX"},       // MIXER
    {"POLY","PRGLD","KBOCT","FRTNG","QNORM","DRVTP"},       // VOICE
    {"DIST","CHORUS","RVB SND","DLY SND","MFX FRQ","MFX RES"}, // FX
    {"MACRO1","MACRO2","MACRO3","MACRO4","MACRO5","MACRO6"}, // MACRO
    {"O1DTUN","O1CENT","O1 PB","O2DTUN","O2CENT","O2 PB"},  // TUNE
};
static const char* CT_SYNTH_PAGE_NAMES[19] = {
    "OSC 1","OSC 2","FILTER","ENV","MIXER","VOICE","FX","MACRO","TUNE",
    "MOD 1","MOD 2","MOD 3","MOD 4","MOD 5","MOD 6","MOD 7","MOD 8","MOD 9","MOD 10"
};
// Mod matrix NRPN table — 10 slots × 4 params {msb,lsb}: src1,src2,depth,dest
static constexpr int CT_MOD_NRPN[10][4][2] = {
    {{1, 83},{1, 84},{1, 86},{1, 87}},  // mod 1
    {{1, 88},{1, 89},{1, 91},{1, 92}},  // mod 2
    {{1, 93},{1, 94},{1, 96},{1, 97}},  // mod 3
    {{1, 98},{1, 99},{1,101},{1,102}},  // mod 4
    {{1,103},{1,104},{1,106},{1,107}},  // mod 5
    {{1,108},{1,109},{1,111},{1,112}},  // mod 6
    {{1,113},{1,114},{1,116},{1,117}},  // mod 7
    {{1,118},{1,119},{1,121},{1,122}},  // mod 8
    {{1,123},{1,124},{1,126},{1,127}},  // mod 9
    {{2,  0},{2,  1},{2,  3},{2,  4}},  // mod 10
};
static const char* CT_MOD_PNAMES[4] = {"SRC 1","SRC 2","DEPTH","DEST"};
static const char* CT_MOD_SRC[13] = {
    "DIRECT","MOD WHL","AFTOUCH","EXPR","VEL","KBD",
    "LFO1 +","LFO1+/-","LFO2 +","LFO2+/-",
    "ENV AMP","ENV FLT","ENV 3"
};
static const char* CT_MOD_DEST[18] = {
    "O1+O2 P","O1 PTCH","O2 PTCH","O1 VSYN","O2 VSYN",
    "O1 PW  ","O2 PW  ","O1 LVL ","O2 LVL ","NOISE  ",
    "RING   ","FLT DRV","FLT FRQ","FLT RES","LFO1 RT",
    "LFO2 RT","AMP DEC","FLT DEC"
};

// Circuit Tracks drum CC tables — 4 drums × 9 params
// Params 0-6 send on MIDI ch10; params 7-8 (RVB/DLY sends) send on MIDI ch16
static constexpr int CT_DRUM_CC[4][9] = {
    { 8, 12, 14, 15, 16, 17, 77,  90, 113}, // DR1
    {18, 23, 34, 40, 42, 43, 78, 106, 114}, // DR2
    {44, 45, 46, 47, 48, 49, 79, 109, 115}, // DR3
    {50, 53, 55, 57, 61, 76, 80, 110, 116}, // DR4
};
// MIDI channel (0-indexed) for each drum param slot
static constexpr int CT_DRUM_PARAM_CH[9] = {9,9,9,9,9,9,9,15,15};
static const char* CT_DRUM_PNAMES[9] = {"PATCH","LEVEL","PITCH","DECAY","DIST","EQ","PAN","RVB","DLY"};

// Arturia MicroFreak CC tables — 4 pages × 6 params (-1 = unused slot)
static constexpr int MF_CC[4][6] = {
    {  9, 10, 12, 13,  5, -1},  // OSC: Type, Wave, Timbre, Shape, Glide
    { 23, 83, 26,105,106, 29},  // FILTER+ENV: Cutoff, Res, EnvAmt, Attack, Decay, Sustain
    {102,103, 28, 24, -1, -1},  // CYC ENV: Rise, Fall, Hold, Amount
    { 93, 94, 91, 92,  2, 64},  // LFO/ARP: LFO free, LFO sync, ARP free, ARP sync, Spice, Hold
};
static const char* MF_OSC_TYPE_NAMES[] = {
    "Basic", "Superwave", "Harmonic", "Wavetable", "Triangle", "Noise",
    "PWM", "Mod", "FM", "Karplus", "Paraphonic", "Vocoder", "Waveshaper", "Sample"
};
static const char* MF_PNAMES[4][6] = {
    {"TYPE","WAVE","TIMBRE","SHAPE","GLIDE",""},
    {"CUTOFF","RES","ENV AMT","ATTACK","DECAY","SUST"},
    {"RISE","FALL","HOLD","AMOUNT","",""},
    {"LFO","LFO SYN","ARP","ARP SYN","SPICE","HOLD"},
};
static const char* MF_PAGE_NAMES[4] = {"OSC","FILTER","CYC ENV","LFO/ARP"};

struct Track {
    std::string name;
    int ch = 9, note = 36, vel = 100;
    int patLen = 16;
    int noteLen = 1;
    int dtype = DT_PERC;  // drum synth type
    Step steps[MAX_STEPS];
    // p[0-5]=SYNTH  p[6-11]=EXT(type-specific)  p[12-17]=FX  p[18-23]=FX2
    // Note: synthP[0] is overridden per-track at init for CT drum (patch 0-15)
    int synthP[NUM_SYNTH_PARAMS] = {
         0,64,64,64,64,64,  // page 1 (synthP[0]=drum patch default 0)
         0, 0, 0, 0, 0, 0,  // page 2
        127, 0, 0, 0, 0,100, // page 3
         0,40, 0,40, 0,60,  // page 4
        64,64,64,64,64,64,  // page 5
        64,64,64,64,64,64,  // page 6
         0, 0, 0, 0,64,64,  // page 7: FX (dist,chorus,rvbSnd,dlySnd,mfxFreq,mfxRes)
         0, 0, 0, 0, 0, 0   // page 8: MACRO knobs 1-6
    };
};
static const struct { const char* n; int note; int dtype; } TDEFS[NUM_TRACKS] = {
    {"KICK",36,DT_KICK},{"SNARE",38,DT_SNARE},{"HI-HAT",42,DT_HIHAT},{"OH",46,DT_HIHAT},
    {"LO PRC",41,DT_PERC},{"HI TOM",45,DT_TOM},{"MID TOM",47,DT_TOM},{"LO TOM",43,DT_TOM},
    {"CLAP",39,DT_CLAP},{"SHAKER",70,DT_SHAKER},{"COWBELL",56,DT_COWBELL},{"RIDE",51,DT_RIDE},
    {"CRASH",49,DT_CRASH},{"TOM 1",48,DT_TOM},{"TOM 2",50,DT_TOM},{"PERC",60,DT_PERC},
};

// Pending note-off for deferred release
struct PendingOff { int ch; int note; double time; };

struct App {
    Track  tracks[NUM_TRACKS];
    int    selTrk   = 0;
    bool   playing  = false;
    int    curStep  = 0;
    float  bpm      = 120.f;
    int    swing    = 0;
    bool   midiThru = false;
    bool   clockOut = false;
    bool   syncIn   = false;
    int    midiCh   = 9;
    int    clkCount = 0;
    double padFlash[NUM_TRACKS]                  {};
    double stepFlash[NUM_TRACKS][MAX_STEPS]       {};
    double lastStep = 0;
    double lastClk  = 0;
    int    clkPulse = 0;
    bool   showMidi = false;
    int    showMidiOpenFrame = -10;  // frame when MIDI panel was opened (for click guard)
    int    frameCount = 0;
    bool   velDrag = false;
    int    vdT = -1, vdS = -1, vdY0 = 0, vdV0 = 100;
    // Click-and-hold to open step editor
    int    holdT = -1, holdS = -1;
    double holdStart = 0.0;
    bool   holdTriggered = false;
    // Popup screen rect (set each frame when popup is drawn, used to block step clicks)
    float  editPopupX = 0, editPopupY = 0, editPopupW = 0, editPopupH = 0;
    // Beat repeat (press-and-hold, loops playhead within 4-step region)
    bool   beatRepeat = false;
    int    beatRepeatStart = 0;  // first step of the 4-step loop region
    int    beatRepeatLen   = 4;  // always 4 steps
    // Pages: each page is 16 steps; numPages controls total length
    int    numPages = 1;         // 1-8 pages (16-128 steps total)
    int    viewPage = 0;         // which page is shown in the grid (0-based)
    bool   wasPaused = false;    // true after pause, so playhead stays visible
    // Synth param page
    int    synthPage = 0;  // 0=SYNTH 1=EXT 2=FX 3=FX2
    // Circuit Tracks track selector
    int    ctTrack    = -1; // -1=none 0=SY1 1=SY2 2=DR1 3=DR2 4=DR3 5=DR4
    int    ctDrumPage =  0; // drum param page when ctTrack>=2
    // Mutate (randomise velocities & toggle random steps)
    // Ratchet scheduling
    int    ratchetTrack = -1;
    int    ratchetStep  = -1;
    int    ratchetNote = 0;
    int    ratchetCh = 9;
    int    ratchetTotal = 1;
    int    ratchetDone = 0;
    double ratchetInterval = 0;
    double ratchetNext = 0;
    // Per-step editor popup
    int    editT = -1;   // track being edited (-1 = none)
    int    editS = -1;   // step being edited
    // Click-and-hold on pad to open track properties popup
    int    padHoldT = -1;
    double padHoldStart = 0.0;
    bool   padHoldTriggered = false;
    bool   showPadEdit = false;
    float  padEditPopupX = 0, padEditPopupY = 0, padEditPopupW = 0, padEditPopupH = 0;
    // Pending note-offs
    std::vector<PendingOff> pendingOffs;
    MidiPorts midi;
};

static constexpr double FLASH_DUR = 0.12;

// ── Global drum synth instance (audio output) ────────────────────────
static DrumSynth g_synth;

// ── Helper: trigger both MIDI + synth ────────────────────────────────
static void synthTrigger(App& app, int trackIdx, float velocity) {
    Track& tr = app.tracks[trackIdx];
    DSParams dp;
    memcpy(dp.p, tr.synthP, sizeof(dp.p));
    g_synth.trigger(tr.dtype, velocity / 127.0f, dp);
}

// ═══════════════════════════════════════════════════════════════════════
//  Draw helpers
// ═══════════════════════════════════════════════════════════════════════
static void GradV(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 t, ImU32 bt) {
    dl->AddRectFilledMultiColor(a, b, t, t, bt, bt);
}
static void GradH(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 l, ImU32 r) {
    dl->AddRectFilledMultiColor(a, b, l, r, r, l);
}

static void DrawPill(ImDrawList* dl, float x, float y, float w, float h, ImU32 bg, float rnd=6) {
    dl->AddRectFilled(ImVec2(x+1,y+2), ImVec2(x+w+1,y+h+2), im(0,0,0,50), rnd);
    dl->AddRectFilled(ImVec2(x,y), ImVec2(x+w,y+h), bg, rnd);
    dl->AddRectFilled(ImVec2(x+2,y+1), ImVec2(x+w-2,y+2), withA(im(255,255,255),25), 1);
    dl->AddRect(ImVec2(x,y), ImVec2(x+w,y+h), withA(im(255,255,255),12), rnd);
}

static void DrawGlow(ImDrawList* dl, float x, float y, float w, float h, ImU32 c, float spread=4, float rnd=8) {
    for (int i = (int)spread; i >= 1; --i) {
        float a = (1.0f - (float)i/spread) * 0.35f;
        dl->AddRect(ImVec2(x-i,y-i), ImVec2(x+w+i,y+h+i),
                    withA(c, (int)(a * 255)), rnd + i, 0, 1.5f);
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Step trigger  — respects probability & schedules ratchets
// ═══════════════════════════════════════════════════════════════════════
static void triggerStep(App& app, double now) {
    int s = app.curStep;
    int totalSteps = app.numPages * NUM_STEPS;
    double stepSec = 15.0 / app.bpm;
    // CT drum mode flag
    bool isCTDrum = (app.ctTrack >= 2);
    for (int t = 0; t < NUM_TRACKS; ++t) {
        if (s >= app.tracks[t].patLen || !app.tracks[t].steps[s].on) continue;
        const Step& step = app.tracks[t].steps[s];
        // Probability check
        if (step.prob < 100) {
            if ((rand() % 100) >= step.prob) continue;
        }
        int note = app.tracks[t].note;
        int ratch = step.ratchet;
        // Retrigger: remove stale pending note-offs for this ch+note (CT drum only)
        if (isCTDrum) {
            app.pendingOffs.erase(
                std::remove_if(app.pendingOffs.begin(), app.pendingOffs.end(),
                    [&](const PendingOff& p){ return p.ch == app.midiCh && p.note == note; }),
                app.pendingOffs.end());
        }
        // CT drum: NoteOff to clear voice, PATCH CC to select sample, NoteOn to fire
        if (isCTDrum) {
            int drumIdx = app.ctTrack - 2;
            int drumNote = app.tracks[t].note;
            int patchVal = app.tracks[t].synthP[0];
            app.midi.noteOff(9, drumNote);
            app.midi.sendCC(9, CT_DRUM_CC[drumIdx][0], patchVal);
            app.midi.noteOn(9, drumNote, step.vel);
            fprintf(stderr, "[CT DRUM seq] drumIdx=%d CC=%d patch=%d note=%d\n",
                drumIdx, CT_DRUM_CC[drumIdx][0], patchVal, drumNote);
            app.pendingOffs.push_back({9, drumNote, now + 0.15});
            app.padFlash[t] = now;
            app.stepFlash[t][s] = now;
            continue;
        }
        // Fire first hit
        double noteDur = (15.0 / app.bpm) * app.tracks[t].noteLen * 0.9;
        app.midi.noteOn(app.midiCh, note, step.vel);
        app.pendingOffs.push_back({app.midiCh, note, now + noteDur});
        if (app.midi.synthEnabled()) synthTrigger(app, t, (float)step.vel);
        app.padFlash[t] = now;
        app.stepFlash[t][s] = now;
        // Schedule remaining ratchets via app state
        if (ratch > 1) {
            app.ratchetTrack = t;
            app.ratchetStep  = s;
            app.ratchetNote = note;
            app.ratchetCh = app.midiCh;
            app.ratchetTotal = ratch;
            app.ratchetDone = 1;
            app.ratchetInterval = stepSec / ratch;
            app.ratchetNext = now + app.ratchetInterval;
        }
    }
    // Advance step — if beat repeat is active, loop within the 4-step region
    if (app.beatRepeat) {
        int next = s + 1;
        int regionEnd = app.beatRepeatStart + app.beatRepeatLen;
        if (next >= regionEnd || next >= totalSteps)
            next = app.beatRepeatStart;
        app.curStep = next;
    } else {
        app.curStep = (s + 1) % totalSteps;
    }
    // Update view page to follow playhead
    app.viewPage = app.curStep / NUM_STEPS;
    app.clkPulse = 0;
}

// ═══════════════════════════════════════════════════════════════════════
//  Mutate function — rhythm-aware, musical mutations
// ═══════════════════════════════════════════════════════════════════════

// Rhythmic weight of each 16th-note position in a bar (0-15)
// Higher = stronger metric position (downbeat > backbeat > 8th > 16th)
static const int METRIC_WEIGHT[16] = {
    8, 1, 3, 1,  // beat 1: 1 . . .
    6, 1, 3, 1,  // beat 2: 2 . . .
    7, 1, 3, 1,  // beat 3: 3 . . .
    6, 1, 3, 1,  // beat 4: 4 . . .
};

// Instrument role classification
enum InstRole { ROLE_KICK, ROLE_SNARE, ROLE_HAT, ROLE_PERC, ROLE_ACCENT };

static InstRole classifyTrack(int dtype) {
    switch (dtype) {
        case DT_KICK:    return ROLE_KICK;
        case DT_SNARE:   return ROLE_SNARE;
        case DT_CLAP:    return ROLE_SNARE;
        case DT_HIHAT:   return ROLE_HAT;
        case DT_SHAKER:  return ROLE_HAT;
        case DT_RIDE:    return ROLE_HAT;
        case DT_CRASH:   return ROLE_ACCENT;
        case DT_CYMBAL:  return ROLE_ACCENT;
        default:         return ROLE_PERC;
    }
}

static void mutatePattern(App& app) {
    int totalSteps = app.numPages * NUM_STEPS;

    for (int t = 0; t < NUM_TRACKS; ++t) {
        Track& tr = app.tracks[t];
        int patEnd = std::min(tr.patLen, totalSteps);
        InstRole role = classifyTrack(tr.dtype);

        // ── Analyze current pattern ──
        int density = 0;
        for (int s = 0; s < patEnd; ++s) if (tr.steps[s].on) density++;
        if (density == 0 && (rand() % 100) > 12) continue;  // mostly skip empty tracks

        float fillRatio = (float)density / patEnd;

        // ── Per-role mutation strategies ──
        // Each strategy picks ONE small change per invocation

        int mutation = rand() % 100;

        switch (role) {
        case ROLE_KICK: {
            // Kick: stay on strong beats. Displace, add syncopation, or ghost.
            if (mutation < 30) {
                // DISPLACE: shift one hit by ±1 step (creates push/pull feel)
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = rand() % patEnd;
                    if (!tr.steps[s].on) continue;
                    int dir = (rand() % 2) ? 1 : -1;
                    int ns = (s + dir + patEnd) % patEnd;
                    if (!tr.steps[ns].on && METRIC_WEIGHT[ns % 16] >= 3) {
                        tr.steps[ns] = tr.steps[s];
                        tr.steps[s].on = false;
                        break;
                    }
                }
            } else if (mutation < 55 && fillRatio < 0.35f) {
                // ADD: place a hit on a strong beat that's empty
                int candidates[] = {0, 8, 4, 12, 6, 10};
                for (int c : candidates) {
                    int s = c % patEnd;
                    if (!tr.steps[s].on && (rand() % 2)) {
                        tr.steps[s].on = true;
                        tr.steps[s].vel = 85 + rand() % 30;
                        break;
                    }
                }
            } else if (mutation < 75 && density > 1) {
                // REMOVE: drop one hit (prefer weaker positions)
                for (int attempt = 0; attempt < 6; ++attempt) {
                    int s = rand() % patEnd;
                    if (tr.steps[s].on && METRIC_WEIGHT[s % 16] < 6) {
                        tr.steps[s].on = false;
                        break;
                    }
                }
            } else {
                // GHOST: add a quiet ghost kick on an off-beat
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = (rand() % (patEnd / 2)) * 2 + 1;  // odd positions
                    if (s < patEnd && !tr.steps[s].on) {
                        tr.steps[s].on = true;
                        tr.steps[s].vel = 35 + rand() % 25;
                        break;
                    }
                }
            }
            break;
        }
        case ROLE_SNARE: {
            // Snare: protect backbeats (4, 12), add flams/ghosts/drags
            if (mutation < 35) {
                // GHOST NOTE: quiet hit before or after an existing hit
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = rand() % patEnd;
                    if (!tr.steps[s].on) continue;
                    int gs = (s - 1 + patEnd) % patEnd;  // just before
                    if (!tr.steps[gs].on) {
                        tr.steps[gs].on = true;
                        tr.steps[gs].vel = 25 + rand() % 30;  // very quiet
                        break;
                    }
                }
            } else if (mutation < 55 && fillRatio < 0.25f) {
                // ADD on a backbeat or off-beat
                int opts[] = {4, 12, 10, 14, 2, 6};
                for (int c : opts) {
                    int s = c % patEnd;
                    if (!tr.steps[s].on && (rand() % 2)) {
                        tr.steps[s].on = true;
                        tr.steps[s].vel = 80 + rand() % 35;
                        break;
                    }
                }
            } else if (mutation < 70) {
                // DRAG: add a ratchet (2 or 3) to an existing hit
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = rand() % patEnd;
                    if (tr.steps[s].on && tr.steps[s].ratchet == 1) {
                        tr.steps[s].ratchet = (rand() % 2) ? 2 : 3;
                        break;
                    }
                }
            } else if (mutation < 85 && density > 2) {
                // REMOVE a non-backbeat hit
                for (int attempt = 0; attempt < 6; ++attempt) {
                    int s = rand() % patEnd;
                    if (tr.steps[s].on && (s % 16) != 4 && (s % 16) != 12) {
                        tr.steps[s].on = false;
                        break;
                    }
                }
            } else {
                // ACCENT: vary velocity of existing hits for dynamics
                for (int s = 0; s < patEnd; ++s) {
                    if (tr.steps[s].on) {
                        bool isBackbeat = ((s % 16) == 4 || (s % 16) == 12);
                        if (isBackbeat)
                            tr.steps[s].vel = std::clamp(tr.steps[s].vel + (rand() % 11 - 3), 90, 127);
                        else
                            tr.steps[s].vel = std::clamp(tr.steps[s].vel + (rand() % 21 - 10), 25, 110);
                    }
                }
            }
            break;
        }
        case ROLE_HAT: {
            // Hats/shaker/ride: density changes, open/close patterns, accent shifts
            if (mutation < 25 && fillRatio < 0.6f) {
                // FILL IN: add 1-2 hits in gaps to increase density
                int added = 0;
                for (int attempt = 0; attempt < 8 && added < 2; ++attempt) {
                    int s = rand() % patEnd;
                    if (!tr.steps[s].on) {
                        tr.steps[s].on = true;
                        // Off-beats quieter than on-beats
                        tr.steps[s].vel = (METRIC_WEIGHT[s % 16] >= 3) ? 80 + rand() % 30 : 50 + rand() % 30;
                        added++;
                    }
                }
            } else if (mutation < 45 && density > 3) {
                // THIN OUT: remove 1-2 hits from weak positions
                int removed = 0;
                for (int attempt = 0; attempt < 8 && removed < 2; ++attempt) {
                    int s = rand() % patEnd;
                    if (tr.steps[s].on && METRIC_WEIGHT[s % 16] <= 3) {
                        tr.steps[s].on = false;
                        removed++;
                    }
                }
            } else if (mutation < 65) {
                // ACCENT PATTERN: apply a velocity curve for groove
                // Emphasize on-beats, soften off-beats
                for (int s = 0; s < patEnd; ++s) {
                    if (!tr.steps[s].on) continue;
                    int w = METRIC_WEIGHT[s % 16];
                    int target = (w >= 6) ? 100 + rand() % 20 : (w >= 3) ? 70 + rand() % 25 : 45 + rand() % 25;
                    // Nudge toward target (don't snap — gradual over multiple mutates)
                    tr.steps[s].vel = std::clamp((tr.steps[s].vel * 2 + target) / 3, 30, 127);
                }
            } else if (mutation < 80) {
                // SHIFT PATTERN: rotate the entire hat pattern by 1 step
                int dir = (rand() % 2) ? 1 : -1;
                Step tmp[MAX_STEPS];
                for (int s = 0; s < patEnd; ++s) tmp[(s + dir + patEnd) % patEnd] = tr.steps[s];
                for (int s = 0; s < patEnd; ++s) tr.steps[s] = tmp[s];
            } else {
                // PROBABILITY: add probability to 1-2 off-beat hits
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = rand() % patEnd;
                    if (tr.steps[s].on && METRIC_WEIGHT[s % 16] <= 3 && tr.steps[s].prob == 100) {
                        tr.steps[s].prob = 50 + (rand() % 3) * 15;  // 50, 65, or 80%
                        break;
                    }
                }
            }
            break;
        }
        case ROLE_ACCENT: {
            // Crash/cymbal: very sparse, accent transitions
            if (mutation < 40 && density == 0) {
                // ADD: put a crash on beat 1 of a random bar
                int bar = rand() % app.numPages;
                int s = bar * 16;
                if (s < patEnd) {
                    tr.steps[s].on = true;
                    tr.steps[s].vel = 90 + rand() % 30;
                }
            } else if (mutation < 65 && density > 0) {
                // MOVE: shift crash to a different downbeat
                for (int s = 0; s < patEnd; ++s) {
                    if (tr.steps[s].on) {
                        tr.steps[s].on = false;
                        int newBar = rand() % app.numPages;
                        int ns = newBar * 16;
                        if (ns < patEnd) {
                            tr.steps[ns].on = true;
                            tr.steps[ns].vel = 95 + rand() % 25;
                        }
                        break;
                    }
                }
            } else if (density > 2) {
                // THIN: remove extra crashes (keep max 1-2)
                int kept = 0;
                for (int s = 0; s < patEnd; ++s) {
                    if (tr.steps[s].on) {
                        if (kept >= 1 && (rand() % 2)) tr.steps[s].on = false;
                        else kept++;
                    }
                }
            }
            // else: do nothing (crashes should be rare)
            break;
        }
        case ROLE_PERC: {
            // Toms, cowbell, perc: syncopation-oriented mutations
            if (mutation < 30 && fillRatio < 0.3f) {
                // ADD syncopated hit: prefer off-beats and weak positions
                for (int attempt = 0; attempt < 6; ++attempt) {
                    int s = rand() % patEnd;
                    if (!tr.steps[s].on && METRIC_WEIGHT[s % 16] <= 3) {
                        tr.steps[s].on = true;
                        tr.steps[s].vel = 60 + rand() % 40;
                        break;
                    }
                }
            } else if (mutation < 50) {
                // DISPLACE: shift a hit by ±1 or ±2
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = rand() % patEnd;
                    if (!tr.steps[s].on) continue;
                    int shift = (rand() % 2) ? 1 : 2;
                    if (rand() % 2) shift = -shift;
                    int ns = (s + shift + patEnd) % patEnd;
                    if (!tr.steps[ns].on) {
                        tr.steps[ns] = tr.steps[s];
                        tr.steps[s].on = false;
                        break;
                    }
                }
            } else if (mutation < 70 && density > 3) {
                // REMOVE: thin out busier perc parts
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = rand() % patEnd;
                    if (tr.steps[s].on) { tr.steps[s].on = false; break; }
                }
            } else if (mutation < 85) {
                // VELOCITY SHAPE: create a crescendo or decrescendo across hits
                bool cresc = rand() % 2;
                int hitIdx = 0, hitCount = density;
                for (int s = 0; s < patEnd; ++s) {
                    if (!tr.steps[s].on) continue;
                    float pos = (hitCount > 1) ? (float)hitIdx / (hitCount - 1) : 0.5f;
                    float curve = cresc ? pos : (1.0f - pos);
                    int target = 45 + (int)(curve * 75);
                    tr.steps[s].vel = std::clamp((tr.steps[s].vel + target) / 2, 30, 120);
                    hitIdx++;
                }
            } else {
                // RATCHET: add a flam/roll to one hit
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = rand() % patEnd;
                    if (tr.steps[s].on && tr.steps[s].ratchet == 1) {
                        tr.steps[s].ratchet = 2;
                        break;
                    }
                }
            }
            break;
        }
        } // switch role

        // ── Chaos pass: ~20% chance per track for a wild move ──
        if ((rand() % 100) < 20) {
            int chaos = rand() % 100;
            if (chaos < 25) {
                // RANDOM TOGGLE: flip 1-3 random steps regardless of role
                int flips = 1 + rand() % 3;
                for (int f = 0; f < flips; ++f) {
                    int s = rand() % patEnd;
                    tr.steps[s].on = !tr.steps[s].on;
                    if (tr.steps[s].on) tr.steps[s].vel = 50 + rand() % 70;
                }
            } else if (chaos < 45) {
                // VELOCITY SPIKE: one hit gets very loud or very quiet
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = rand() % patEnd;
                    if (tr.steps[s].on) {
                        tr.steps[s].vel = (rand() % 2) ? 115 + rand() % 13 : 20 + rand() % 25;
                        break;
                    }
                }
            } else if (chaos < 60) {
                // SURPRISE HIT: drop a note on a totally unexpected position
                int s = rand() % patEnd;
                if (!tr.steps[s].on) {
                    tr.steps[s].on = true;
                    tr.steps[s].vel = 60 + rand() % 50;
                    if (rand() % 3 == 0) tr.steps[s].ratchet = 2 + rand() % 2;
                }
            } else if (chaos < 75) {
                // STUTTER: copy one hit to a neighboring slot
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = rand() % patEnd;
                    if (!tr.steps[s].on) continue;
                    int ns = (s + 1) % patEnd;
                    if (!tr.steps[ns].on) {
                        tr.steps[ns] = tr.steps[s];
                        tr.steps[ns].vel = std::max(30, tr.steps[s].vel - 15);
                        break;
                    }
                }
            } else if (chaos < 88) {
                // PROBABILITY CHAOS: randomize prob on 1-2 hits
                for (int i = 0; i < 1 + rand() % 2; ++i) {
                    int s = rand() % patEnd;
                    if (tr.steps[s].on)
                        tr.steps[s].prob = 25 + (rand() % 4) * 25;  // 25,50,75,100
                }
            } else {
                // RATCHET BURST: wild ratchet on a random hit
                for (int attempt = 0; attempt < 4; ++attempt) {
                    int s = rand() % patEnd;
                    if (tr.steps[s].on) {
                        tr.steps[s].ratchet = 2 + rand() % 3;  // 2, 3, or 4
                        break;
                    }
                }
            }
        }

        // ── Global: subtle velocity humanization on all active hits (±5) ──
        for (int s = 0; s < patEnd; ++s) {
            if (tr.steps[s].on) {
                int dv = (rand() % 11) - 5;
                tr.steps[s].vel = std::clamp(tr.steps[s].vel + dv, 1, 127);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Clear pattern — reset all steps
// ═══════════════════════════════════════════════════════════════════════
static void clearPattern(App& app) {
    int totalSteps = app.numPages * NUM_STEPS;
    for (int t = 0; t < NUM_TRACKS; ++t)
        for (int s = 0; s < totalSteps; ++s)
            app.tracks[t].steps[s] = Step{};
    app.editT = -1; app.editS = -1;
}

// ═══════════════════════════════════════════════════════════════════════
//  MIDI file save / load (Standard MIDI File format 0)
// ═══════════════════════════════════════════════════════════════════════
static void writeVLQ(std::vector<uint8_t>& buf, uint32_t val) {
    uint8_t bytes[4]; int n = 0;
    bytes[n++] = val & 0x7F;
    val >>= 7;
    while (val > 0) { bytes[n++] = (val & 0x7F) | 0x80; val >>= 7; }
    for (int i = n - 1; i >= 0; --i) buf.push_back(bytes[i]);
}

static uint32_t readVLQ(FILE* f) {
    uint32_t val = 0; uint8_t b;
    do { if (fread(&b, 1, 1, f) < 1) return val;
         val = (val << 7) | (b & 0x7F);
    } while (b & 0x80);
    return val;
}

static bool saveMIDI(const App& app, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    struct NoteEv { uint32_t tick; uint8_t status, note, vel; };
    std::vector<NoteEv> events;
    uint32_t ticksPerStep = 24; // 96 ppqn → 16th note = 24 ticks

    for (int t = 0; t < NUM_TRACKS; ++t) {
        const Track& tr = app.tracks[t];
        for (int s = 0; s < tr.patLen; ++s) {
            if (!tr.steps[s].on) continue;
            uint32_t tick = s * ticksPerStep;
            uint8_t ch = (uint8_t)(app.midiCh & 0xF);
            events.push_back({tick, (uint8_t)(0x90|ch), (uint8_t)tr.note, (uint8_t)tr.steps[s].vel});
            uint32_t offTick = tick + ticksPerStep - 1;
            events.push_back({offTick, (uint8_t)(0x80|ch), (uint8_t)tr.note, 0});
        }
    }

    std::sort(events.begin(), events.end(), [](const NoteEv& a, const NoteEv& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return (a.status & 0xF0) < (b.status & 0xF0); // note-off before note-on
    });

    std::vector<uint8_t> td;
    // Tempo meta: set BPM
    uint32_t uspqn = (uint32_t)(60000000.0 / app.bpm);
    td.push_back(0x00); td.push_back(0xFF); td.push_back(0x51); td.push_back(0x03);
    td.push_back((uspqn >> 16) & 0xFF); td.push_back((uspqn >> 8) & 0xFF); td.push_back(uspqn & 0xFF);

    uint32_t lastTick = 0;
    for (auto& ev : events) {
        writeVLQ(td, ev.tick - lastTick);
        lastTick = ev.tick;
        td.push_back(ev.status); td.push_back(ev.note); td.push_back(ev.vel);
    }
    // End of track
    td.push_back(0x00); td.push_back(0xFF); td.push_back(0x2F); td.push_back(0x00);

    // MThd
    uint8_t hdr[14] = {'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96};
    fwrite(hdr, 1, 14, f);
    // MTrk
    uint32_t tl = (uint32_t)td.size();
    uint8_t th[8] = {'M','T','r','k', (uint8_t)(tl>>24),(uint8_t)(tl>>16),(uint8_t)(tl>>8),(uint8_t)(tl&0xFF)};
    fwrite(th, 1, 8, f);
    fwrite(td.data(), 1, td.size(), f);
    fclose(f);
    return true;
}

static bool loadMIDI(App& app, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[14];
    if (fread(hdr, 1, 14, f) < 14 || memcmp(hdr, "MThd", 4) != 0) { fclose(f); return false; }
    uint16_t ppqn = (hdr[12] << 8) | hdr[13];
    if (ppqn == 0) ppqn = 96;
    uint16_t numTracks = (hdr[10] << 8) | hdr[11];

    for (int t = 0; t < NUM_TRACKS; ++t)
        for (int s = 0; s < MAX_STEPS; ++s)
            app.tracks[t].steps[s] = Step{};

    float ticksPerStep = ppqn / 4.0f;

    for (int trk = 0; trk < numTracks; ++trk) {
        uint8_t trkHdr[8];
        if (fread(trkHdr, 1, 8, f) < 8 || memcmp(trkHdr, "MTrk", 4) != 0) break;
        uint32_t trkLen = (trkHdr[4]<<24)|(trkHdr[5]<<16)|(trkHdr[6]<<8)|trkHdr[7];
        long startPos = ftell(f);
        uint32_t absTick = 0;
        uint8_t runStat = 0;

        while (ftell(f) - startPos < (long)trkLen) {
            absTick += readVLQ(f);
            uint8_t b;
            if (fread(&b, 1, 1, f) < 1) goto done;
            if (b == 0xFF) { // meta
                uint8_t mtype; if (fread(&mtype, 1, 1, f) < 1) goto done;
                uint32_t mlen = readVLQ(f);
                if (mtype == 0x51 && mlen == 3) {
                    uint8_t tb[3]; if (fread(tb, 1, 3, f) < 3) goto done;
                    uint32_t us = (tb[0]<<16)|(tb[1]<<8)|tb[2];
                    if (us > 0) app.bpm = 60000000.0f / us;
                } else { fseek(f, mlen, SEEK_CUR); }
                continue;
            }
            if (b == 0xF0 || b == 0xF7) { // sysex
                uint32_t slen = readVLQ(f); fseek(f, slen, SEEK_CUR); continue;
            }
            if (b & 0x80) { runStat = b; if (fread(&b, 1, 1, f) < 1) goto done; }
            uint8_t d1 = b, st = runStat & 0xF0;
            if (st == 0x90 || st == 0x80) {
                uint8_t d2; if (fread(&d2, 1, 1, f) < 1) goto done;
                if (st == 0x90 && d2 > 0) {
                    int step = (int)(absTick / ticksPerStep + 0.5f) % MAX_STEPS;
                    for (int ti = 0; ti < NUM_TRACKS; ++ti) {
                        if (app.tracks[ti].note == d1) {
                            app.tracks[ti].steps[step].on = true;
                            app.tracks[ti].steps[step].vel = d2;
                            break;
                        }
                    }
                }
            } else if (st == 0xC0 || st == 0xD0) { /* 1-byte msg, d1 already read */ }
            else { uint8_t d2; if (fread(&d2, 1, 1, f) < 1) goto done; }
        }
    }
done:
    fclose(f);
    return true;
}

static std::string fileDialog(bool save) {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "MIDI Files\0*.mid;*.midi\0All Files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    if (save) {
        strncpy(buf, "pattern.mid", sizeof(buf)-1);
        ofn.lpstrTitle = "Save MIDI";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameA(&ofn)) return "";
    } else {
        ofn.lpstrTitle = "Load MIDI";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameA(&ofn)) return "";
    }
    std::string path(buf);
#else
    char buf[4096] = {};
#ifdef __APPLE__
    const char* script = save
        ? "osascript -e 'POSIX path of (choose file name with prompt \"Save MIDI\" default name \"pattern.mid\")' 2>/dev/null"
        : "osascript -e 'POSIX path of (choose file with prompt \"Load MIDI\" of type {\"mid\",\"midi\",\"public.midi-audio\"})' 2>/dev/null";
#else
    const char* script = save
        ? "zenity --file-selection --save --confirm-overwrite --filename='pattern.mid' --file-filter='MIDI|*.mid *.midi' 2>/dev/null"
        : "zenity --file-selection --file-filter='MIDI|*.mid *.midi' 2>/dev/null";
#endif
    FILE* pipe = popen(script, "r");
    if (!pipe) return "";
    if (fgets(buf, sizeof(buf), pipe)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
    }
    int rc = pclose(pipe);
    if (rc != 0 || buf[0] == 0) return "";
    std::string path(buf);
#endif
    // Ensure .mid extension for save
    if (save && path.size() > 0) {
        if (path.size() < 4 || (path.substr(path.size()-4) != ".mid" && path.substr(path.size()-5) != ".midi"))
            path += ".mid";
    }
    return path;
}

// ═══════════════════════════════════════════════════════════════════════
//  Fonts
// ═══════════════════════════════════════════════════════════════════════
static ImFont* fSm   = nullptr;
static ImFont* fMd   = nullptr;
static ImFont* fBd   = nullptr;
static ImFont* fLg   = nullptr;
static ImFont* fXl   = nullptr;
static ImFont* fTiny = nullptr;

// ═══════════════════════════════════════════════════════════════════════
//  RENDER FRAME
// ═══════════════════════════════════════════════════════════════════════
static void renderFrame(App& app) {
    double now = get_time_sec();
    ImGuiIO& io = ImGui::GetIO();
    ++app.frameCount;

    // ── Click inside popup this frame? Block step interactions. ───────
    bool popupConsumedClick = false;
    if (app.showMidi) {
        // MIDI panel is a full-screen modal — block all step clicks
        popupConsumedClick = true;
    } else if (app.showPadEdit && ImGui::IsMouseClicked(0)) {
        ImVec2 mp = io.MousePos;
        popupConsumedClick = (mp.x >= app.padEditPopupX && mp.x <= app.padEditPopupX + app.padEditPopupW &&
                              mp.y >= app.padEditPopupY && mp.y <= app.padEditPopupY + app.padEditPopupH);
    } else if (app.editT >= 0 && ImGui::IsMouseClicked(0)) {
        ImVec2 mp = io.MousePos;
        popupConsumedClick = (mp.x >= app.editPopupX && mp.x <= app.editPopupX + app.editPopupW &&
                              mp.y >= app.editPopupY && mp.y <= app.editPopupY + app.editPopupH);
    }

    // ── Process pending note-offs ──────────────────────────────────
    for (int i = (int)app.pendingOffs.size()-1; i >= 0; --i) {
        if (now >= app.pendingOffs[i].time) {
            app.midi.noteOff(app.pendingOffs[i].ch, app.pendingOffs[i].note);
            app.pendingOffs.erase(app.pendingOffs.begin()+i);
        }
    }

    // ── Ratchet continuation ─────────────────────────────────────────
    while (app.ratchetDone > 0 && app.ratchetDone < app.ratchetTotal && now >= app.ratchetNext) {
        app.midi.noteOn(app.ratchetCh, app.ratchetNote, 90);
        app.pendingOffs.push_back({app.ratchetCh, app.ratchetNote, now + app.ratchetInterval * 0.8});
        if (app.ratchetTrack >= 0) {
            if (app.midi.synthEnabled()) synthTrigger(app, app.ratchetTrack, 90.0f);
            app.padFlash[app.ratchetTrack] = now;
            if (app.ratchetStep >= 0)
                app.stepFlash[app.ratchetTrack][app.ratchetStep] = now;
        }
        ++app.ratchetDone;
        app.ratchetNext += app.ratchetInterval;
        if (app.ratchetDone >= app.ratchetTotal) app.ratchetDone = 0;
    }

    // ── Sequencer tick ───────────────────────────────────────────────
    if (app.playing && !app.syncIn) {
        double stepSec = 15.0 / app.bpm;
        double swingSec = 0;
        if (app.swing > 0 && (app.curStep % 2 == 1))
            swingSec = (app.swing / 100.0) * stepSec;
        if (app.lastStep == 0) app.lastStep = now;
        if (now - app.lastStep >= stepSec + swingSec) {
            triggerStep(app, now);
            app.lastStep += stepSec + swingSec;
            if (app.clockOut) { app.midi.sendRaw(0xF8); app.clkPulse = 1; app.lastClk = now; }
        }
        if (app.clockOut && app.clkPulse > 0 && app.clkPulse < 6) {
            double clkInt = stepSec / 6.0;
            if (now - app.lastClk >= app.clkPulse * clkInt) {
                app.midi.sendRaw(0xF8); ++app.clkPulse;
                if (app.clkPulse >= 6) app.clkPulse = 0;
            }
        }
    }

    // ── Incoming MIDI ────────────────────────────────────────────────
    for (auto& e : app.midi.drain()) {
        uint8_t type = e.st & 0xF0;
        uint8_t ch   = e.st & 0x0F;
        // Log all incoming messages for debugging
        if (type == 0x80) fprintf(stderr, "[MIDI IN] NoteOff  ch=%d note=%d vel=%d\n", ch+1, e.d1, e.d2);
        else if (type == 0x90) fprintf(stderr, "[MIDI IN] NoteOn   ch=%d note=%d vel=%d\n", ch+1, e.d1, e.d2);
        else if (type == 0xA0) fprintf(stderr, "[MIDI IN] Aftertouch ch=%d note=%d val=%d\n", ch+1, e.d1, e.d2);
        else if (type == 0xB0) fprintf(stderr, "[MIDI IN] CC       ch=%d cc=%d val=%d\n", ch+1, e.d1, e.d2);
        else if (type == 0xC0) fprintf(stderr, "[MIDI IN] ProgramChange ch=%d prog=%d\n", ch+1, e.d1);
        else if (e.st >= 0xF0) fprintf(stderr, "[MIDI IN] Sys/RT   0x%02X 0x%02X 0x%02X\n", e.st, e.d1, e.d2);
        if (type == 0x90 && e.d2 > 0) {
            for (int t = 0; t < NUM_TRACKS; ++t)
                if (app.tracks[t].note == e.d1) app.padFlash[t] = now;
            if (app.midiThru) { app.midi.noteOn(e.st & 0xF, e.d1, e.d2); app.pendingOffs.push_back({e.st & 0xF, e.d1, now + 0.1}); }
        }
        if (app.syncIn) {
            if (e.st == 0xF8) { ++app.clkCount; if (app.clkCount >= 6) { app.clkCount = 0; if (app.playing) triggerStep(app, now); } }
            if (e.st == 0xFA || e.st == 0xFB) { app.playing = true; app.curStep = 0; app.clkCount = 0; }
            if (e.st == 0xFC) { app.playing = false; if (app.clockOut) app.midi.sendRaw(0xFC); }
        }
    }

    // ── Window setup ─────────────────────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float W = io.DisplaySize.x, H = io.DisplaySize.y;

    // ═══════════════════ BACKGROUND ═════════════════════════════════
    GradV(dl, ImVec2(0,0), ImVec2(W,H), im(18,18,26), im(10,10,18));
    for (int i = 0; i < 3; ++i) {
        float r = 400.f - i * 80.f;
        dl->AddCircleFilled(ImVec2(W/2, -50), r, im(30,25,45, 8 + i*3), 64);
    }

    // ═════════════════════ HEADER BAR ═══════════════════════════════
    float hdrH = 56;
    GradV(dl, ImVec2(0,0), ImVec2(W,hdrH), im(28,26,38), im(20,19,30));
    dl->AddRectFilled(ImVec2(0,hdrH-1), ImVec2(W,hdrH), im(80,70,120,100));
    dl->AddRectFilled(ImVec2(0,hdrH), ImVec2(W,hdrH+2), im(0,0,0,60));

    // ── Logo (compact top row) ──
    // ── MIDI button + Page controls (full-height touch row) ──
    {
        const float btnH = hdrH - 6.f, rowY = 3.f;
        float bx = 8.f;

        // MIDI button
        {
            float bw = 52.f;
            ImU32 mbg = app.showMidi ? im(80,70,150) : im(35,33,50);
            DrawPill(dl, bx, rowY, bw, btnH, mbg, 8);
            if (app.showMidi) DrawGlow(dl, bx, rowY, bw, btnH, im(120,110,200), 2, 8);
            dl->AddRect(ImVec2(bx,rowY), ImVec2(bx+bw,rowY+btnH),
                        app.showMidi ? im(120,110,200) : im(55,52,75), 8);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ms2 = ImGui::CalcTextSize("MIDI");
            dl->AddText(ImVec2(bx+bw/2-ms2.x/2, rowY+btnH/2-ms2.y/2),
                        app.showMidi ? im(200,190,255) : im(130,125,165), "MIDI");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(bx, rowY));
            ImGui::InvisibleButton("##midi_btn", ImVec2(bw, btnH));
            if (ImGui::IsItemClicked()) { app.showMidi = true; app.showMidiOpenFrame = app.frameCount; }
        }
        bx += 58.f;

        // − page button
        {
            float bw = 40.f;
            bool enabled = app.numPages > 1;
            DrawPill(dl, bx, rowY, bw, btnH, enabled ? im(75,65,140) : im(18,17,26), 8);
            if (enabled) { DrawGlow(dl, bx, rowY, bw, btnH, im(100,90,180), 2, 8); dl->AddRect(ImVec2(bx,rowY), ImVec2(bx+bw,rowY+btnH), im(130,120,200), 8); }
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ts2 = ImGui::CalcTextSize("-PG");
            dl->AddText(ImVec2(bx+bw/2-ts2.x/2, rowY+btnH/2-ts2.y/2),
                        enabled ? im(240,235,255) : im(40,38,55), "-PG");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(bx, rowY));
            ImGui::InvisibleButton("##prem2", ImVec2(bw, btnH));
            if (!app.showMidi && ImGui::IsItemClicked() && enabled) {
                app.numPages--;
                if (app.viewPage >= app.numPages) app.viewPage = app.numPages - 1;
                int maxSt = app.numPages * NUM_STEPS;
                for (int t2 = 0; t2 < NUM_TRACKS; ++t2)
                    app.tracks[t2].patLen = std::min(app.tracks[t2].patLen, maxSt);
                if (app.curStep >= maxSt) app.curStep = 0;
            }
            if (ImGui::IsItemHovered() && enabled)
                dl->AddRect(ImVec2(bx,rowY), ImVec2(bx+bw,rowY+btnH), im(200,190,255), 8);
        }
        bx += 44.f;

        // ◀ prev page
        {
            float bw = 32.f;
            bool enabled = app.numPages > 1 && app.viewPage > 0;
            DrawPill(dl, bx, rowY, bw, btnH, enabled ? im(75,65,140) : im(18,17,26), 8);
            if (enabled) { DrawGlow(dl, bx, rowY, bw, btnH, im(100,90,180), 2, 8); dl->AddRect(ImVec2(bx,rowY), ImVec2(bx+bw,rowY+btnH), im(130,120,200), 8); }
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ts2 = ImGui::CalcTextSize("<");
            dl->AddText(ImVec2(bx+bw/2-ts2.x/2, rowY+btnH/2-ts2.y/2),
                        enabled ? im(240,235,255) : im(40,38,55), "<");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(bx, rowY));
            ImGui::InvisibleButton("##pgprev2", ImVec2(bw, btnH));
            if (!app.showMidi && ImGui::IsItemClicked() && enabled) app.viewPage--;
            if (ImGui::IsItemHovered() && enabled)
                dl->AddRect(ImVec2(bx,rowY), ImVec2(bx+bw,rowY+btnH), im(200,190,255), 8);
        }
        bx += 34.f;

        // Page label
        {
            float bw = 52.f;
            DrawPill(dl, bx, rowY, bw, btnH, im(22,20,35), 8);
            dl->AddRect(ImVec2(bx,rowY), ImVec2(bx+bw,rowY+btnH), im(50,47,80), 8);
            char pgStr[16]; snprintf(pgStr, sizeof(pgStr), "P%d/%d", app.viewPage+1, app.numPages);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 pls2 = ImGui::CalcTextSize(pgStr);
            dl->AddText(ImVec2(bx+bw/2-pls2.x/2, rowY+btnH/2-pls2.y/2), im(160,155,200), pgStr);
            if (fSm) ImGui::PopFont();
        }
        bx += 54.f;

        // ▶ next page
        {
            float bw = 32.f;
            bool enabled = app.numPages > 1 && app.viewPage < app.numPages - 1;
            DrawPill(dl, bx, rowY, bw, btnH, enabled ? im(75,65,140) : im(18,17,26), 8);
            if (enabled) { DrawGlow(dl, bx, rowY, bw, btnH, im(100,90,180), 2, 8); dl->AddRect(ImVec2(bx,rowY), ImVec2(bx+bw,rowY+btnH), im(130,120,200), 8); }
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ts2 = ImGui::CalcTextSize(">");
            dl->AddText(ImVec2(bx+bw/2-ts2.x/2, rowY+btnH/2-ts2.y/2),
                        enabled ? im(240,235,255) : im(40,38,55), ">");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(bx, rowY));
            ImGui::InvisibleButton("##pgnext2", ImVec2(bw, btnH));
            if (!app.showMidi && ImGui::IsItemClicked() && enabled) app.viewPage++;
            if (ImGui::IsItemHovered() && enabled)
                dl->AddRect(ImVec2(bx,rowY), ImVec2(bx+bw,rowY+btnH), im(200,190,255), 8);
        }
        bx += 34.f;

        // + page button
        {
            float bw = 40.f;
            bool enabled = app.numPages < 8;
            DrawPill(dl, bx, rowY, bw, btnH, enabled ? im(75,65,140) : im(18,17,26), 8);
            if (enabled) { DrawGlow(dl, bx, rowY, bw, btnH, im(100,90,180), 2, 8); dl->AddRect(ImVec2(bx,rowY), ImVec2(bx+bw,rowY+btnH), im(130,120,200), 8); }
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ts2 = ImGui::CalcTextSize("+PG");
            dl->AddText(ImVec2(bx+bw/2-ts2.x/2, rowY+btnH/2-ts2.y/2),
                        enabled ? im(240,235,255) : im(40,38,55), "+PG");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(bx, rowY));
            ImGui::InvisibleButton("##padd2", ImVec2(bw, btnH));
            if (!app.showMidi && ImGui::IsItemClicked() && enabled) {
                app.numPages++;
                app.viewPage = app.numPages - 1;
                // Expand each track's patLen to cover the new page
                int newTotal = app.numPages * NUM_STEPS;
                for (int t2 = 0; t2 < NUM_TRACKS; ++t2)
                    app.tracks[t2].patLen = newTotal;
            }
            if (ImGui::IsItemHovered() && enabled)
                dl->AddRect(ImVec2(bx,rowY), ImVec2(bx+bw,rowY+btnH), im(200,190,255), 8);
        }
    }

    // ── BPM (center-left area) ──
    // (Removed from header — now rendered in left panel above pads)

    // ── BEAT RPT & MUTATE buttons (center-right) ──
    {
        float bh = hdrH - 6.f, by = 3.f;
        float gx = W * 0.28f;
        float gap = 8.f;
        // Beat Repeat
        {
            float bw = 88;
            ImU32 bg = app.beatRepeat ? im(160,60,200) : im(35,30,50);
            DrawPill(dl, gx, by, bw, bh, bg, 8);
            if (app.beatRepeat) DrawGlow(dl, gx, by, bw, bh, im(180,80,240), 3, 8);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ts = ImGui::CalcTextSize("REPEAT");
            dl->AddText(ImVec2(gx+bw/2-ts.x/2, by+bh/2-ts.y/2),
                        app.beatRepeat ? im(255,220,255) : im(120,100,150), "REPEAT");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(gx,by));
            ImGui::InvisibleButton("##beatrpt", ImVec2(bw,bh));
            if (!app.showMidi && ImGui::IsItemActivated()) {
                app.beatRepeat = true;
                int totalSteps2 = app.numPages * NUM_STEPS;
                int step = std::clamp(app.curStep, 0, totalSteps2 - 1);
                app.beatRepeatStart = (step / 4) * 4;
                app.curStep = app.beatRepeatStart;
            }
            if (!app.showMidi && ImGui::IsItemDeactivated()) app.beatRepeat = false;
            gx += bw + gap;
        }
        // Mutate
        {
            float bw = 82;
            DrawPill(dl, gx, by, bw, bh, im(35,45,55), 8);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ts = ImGui::CalcTextSize("MUTATE");
            dl->AddText(ImVec2(gx+bw/2-ts.x/2, by+bh/2-ts.y/2), im(100,200,170), "MUTATE");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(gx,by));
            ImGui::InvisibleButton("##mutate", ImVec2(bw,bh));
            if (!app.showMidi && ImGui::IsItemClicked()) mutatePattern(app);
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(gx,by), ImVec2(gx+bw,by+bh), im(100,200,170,80), 8);
            gx += bw + gap;
        }
        // Clear
        {
            float bw = 74;
            DrawPill(dl, gx, by, bw, bh, im(45,30,35), 8);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ts = ImGui::CalcTextSize("CLEAR");
            dl->AddText(ImVec2(gx+bw/2-ts.x/2, by+bh/2-ts.y/2), im(220,120,120), "CLEAR");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(gx,by));
            ImGui::InvisibleButton("##clear", ImVec2(bw,bh));
            if (!app.showMidi && ImGui::IsItemClicked()) clearPattern(app);
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(gx,by), ImVec2(gx+bw,by+bh), im(220,120,120,80), 8);
            gx += bw + gap;
        }
        // Save
        {
            float bw = 66;
            DrawPill(dl, gx, by, bw, bh, im(30,35,50), 8);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ts = ImGui::CalcTextSize("SAVE");
            dl->AddText(ImVec2(gx+bw/2-ts.x/2, by+bh/2-ts.y/2), im(120,160,220), "SAVE");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(gx,by));
            ImGui::InvisibleButton("##save", ImVec2(bw,bh));
            if (!app.showMidi && ImGui::IsItemClicked()) { std::string p = fileDialog(true);  if (!p.empty()) saveMIDI(app, p.c_str()); }
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(gx,by), ImVec2(gx+bw,by+bh), im(120,160,220,80), 8);
            gx += bw + gap;
        }
        // Load
        {
            float bw = 66;
            DrawPill(dl, gx, by, bw, bh, im(30,40,35), 8);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ts = ImGui::CalcTextSize("LOAD");
            dl->AddText(ImVec2(gx+bw/2-ts.x/2, by+bh/2-ts.y/2), im(120,210,150), "LOAD");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(gx,by));
            ImGui::InvisibleButton("##load", ImVec2(bw,bh));
            if (!app.showMidi && ImGui::IsItemClicked()) { std::string p = fileDialog(false); if (!p.empty()) loadMIDI(app, p.c_str()); }
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(gx,by), ImVec2(gx+bw,by+bh), im(120,210,150,80), 8);
        }
    }

    // ── Transport (right) ──
    {
        float tx = W - 254, ty = 3.f, th = hdrH - 6.f;
        float pw = 76;
        // Play — always restarts from step 0
        {
            ImU32 playBg = app.playing ? im(40,70,45) : im(30,50,35);
            DrawPill(dl, tx, ty, pw, th, playBg, 8);
            if (app.playing) DrawGlow(dl, tx, ty, pw, th, im(60,200,80), 2, 8);
            float cx2 = tx + 20, cy = ty + th/2;
            ImVec2 tri[3] = {{cx2-3,cy-6},{cx2-3,cy+6},{cx2+6,cy}};
            ImU32 triCol = app.playing ? im(140,255,150) : im(100,200,120);
            dl->AddTriangleFilled(tri[0],tri[1],tri[2], triCol);
            if (fSm) ImGui::PushFont(fSm);
            dl->AddText(ImVec2(tx+30, ty+th/2-6), app.playing ? im(140,220,140) : im(100,160,110), "PLAY");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(tx,ty));
            ImGui::InvisibleButton("##play", ImVec2(pw,th));
            if (!app.showMidi && ImGui::IsItemClicked()) {
                app.playing = true; app.wasPaused = false; app.curStep = 0; app.lastStep = now;
                if (app.clockOut) app.midi.sendRaw(0xFA);
            }
            if (ImGui::IsItemHovered())
                dl->AddRect(ImVec2(tx,ty), ImVec2(tx+pw,ty+th), im(60,200,80), 8);
        }

        // Pause / Resume — toggles without resetting step
        float contx = tx + pw + 6;
        ImU32 prBg = app.playing ? im(60,50,20) : im(25,35,65);
        DrawPill(dl, contx, ty, pw, th, prBg, 8);
        if (app.playing) DrawGlow(dl, contx, ty, pw, th, im(220,180,50), 2, 8);
        {
            float cx2 = contx + 16, cy = ty + th/2;
            if (app.playing) {
                // Pause bars
                dl->AddRectFilled(ImVec2(cx2-3,cy-6), ImVec2(cx2,cy+6), im(255,220,80), 1);
                dl->AddRectFilled(ImVec2(cx2+2,cy-6), ImVec2(cx2+5,cy+6), im(255,220,80), 1);
            } else {
                // Resume triangle
                ImVec2 tri[3] = {{cx2-3,cy-6},{cx2-3,cy+6},{cx2+6,cy}};
                dl->AddTriangleFilled(tri[0],tri[1],tri[2], im(100,150,230));
            }
        }
        if (fSm) ImGui::PushFont(fSm);
        const char* prLabel = app.playing ? "PAUSE" : "RESUME";
        ImU32 prCol = app.playing ? im(255,220,80) : im(100,150,230);
        dl->AddText(ImVec2(contx+26, ty+th/2-6), prCol, prLabel);
        if (fSm) ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(contx,ty));
        ImGui::InvisibleButton("##pauseresume", ImVec2(pw,th));
        if (!app.showMidi && ImGui::IsItemClicked()) {
            app.playing = !app.playing;
            if (app.playing) { app.wasPaused = false; app.lastStep = now; if (app.clockOut) app.midi.sendRaw(0xFB); }
            else              { app.wasPaused = true;  if (app.clockOut) app.midi.sendRaw(0xFC); }
        }
        if (ImGui::IsItemHovered())
            dl->AddRect(ImVec2(contx,ty), ImVec2(contx+pw,ty+th), prCol, 8);

        // Stop
        float sx = contx + pw + 6;
        ImU32 stopBg = app.playing ? im(170,45,45) : im(50,30,30);
        DrawPill(dl, sx, ty, pw, th, stopBg, 8);
        if (app.playing) DrawGlow(dl, sx, ty, pw, th, im(220,60,60), 2, 8);
        {
            float cx2 = sx + 20, cy = ty + th/2;
            dl->AddRectFilled(ImVec2(cx2-4,cy-4), ImVec2(cx2+4,cy+4),
                              app.playing ? im(255,200,200) : im(120,80,80), 2);
        }
        if (fSm) ImGui::PushFont(fSm);
        dl->AddText(ImVec2(sx+30, ty+th/2-6), app.playing ? im(255,200,200) : im(120,80,80), "STOP");
        if (fSm) ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(sx,ty));
        ImGui::InvisibleButton("##stop", ImVec2(pw,th));
        if (!app.showMidi && ImGui::IsItemClicked()) {
            app.playing = false; app.wasPaused = false; app.curStep = 0;
            if (app.clockOut) app.midi.sendRaw(0xFC);
            // Send note-off for all pending notes
            for (const auto& off : app.pendingOffs) app.midi.noteOff(off.ch, off.note);
            app.pendingOffs.clear();
        }
        if (ImGui::IsItemHovered())
            dl->AddRect(ImVec2(sx,ty), ImVec2(sx+pw,ty+th), im(255,80,80), 8);

        // MIDI Panic/All Notes Off button
        float px = sx + pw + 10;
        ImGui::SetCursorScreenPos(ImVec2(px,ty));
        ImGui::InvisibleButton("##panic", ImVec2(pw,th));
        if (!app.showMidi && ImGui::IsItemClicked()) {
            for (int ch = 0; ch < 16; ++ch) app.midi.sendCC(ch, 123, 0);
            app.pendingOffs.clear();
        }
        if (ImGui::IsItemHovered())
            dl->AddRect(ImVec2(px,ty), ImVec2(px+pw,ty+th), im(255,180,80), 8);
        ImGui::SetCursorScreenPos(ImVec2(px+pw/2-18,ty+th/2-7));
        ImGui::Text("PANIC");
    // MicroFreak oscillator type labels
    static const char* MF_OSC_TYPE_NAMES[] = {
        "Basic", "Superwave", "Harmonic", "Wavetable", "Triangle", "Noise",
        "PWM", "Mod", "FM", "Karplus", "Paraphonic", "Vocoder", "Waveshaper", "Sample"
    };
    }

    // ═══════════════════ LAYOUT ═════════════════════════════════════
    float contentY = hdrH + 2;
    float statusH = 0;
    float contentH = H - contentY - statusH;
    float leftW = 320;
    float divX = leftW;
    float tedH = 0;   // track editor moved to pad-hold popup
    float padAreaH = contentH - tedH;

    int padGap = 6;
    int padS = (int)((leftW - 20 - 3*padGap) / 4);
    if (padS > 72) padS = 72;
    int padGridW = 4*padS + 3*padGap;
    int padGridH = 4*padS + 3*padGap;
    float padGX = 10 + (leftW - 20 - padGridW) / 2.f;
    float padGY = contentY + 6 + (padAreaH - padGridH - 6) / 2.f;

    float lblW = 54;
    float seqHdrH = 20;
    float synthPanH = 106;  // title bar removed; freed height goes to step rows
    float seqX = divX + lblW;
    float stepW = (W - seqX - 4) / NUM_STEPS;
    float rowH = (contentH - seqHdrH - synthPanH) / NUM_TRACKS;
    if (rowH < 20) rowH = 20;
    if (rowH > 44) rowH = 44;
    float gridBotY = contentY + seqHdrH + NUM_TRACKS * rowH;
    float synthTopY = gridBotY + 2;

    // ── Dropdown open states (declared here so pad input guard can reference them) ──
    static bool ctDropOpen = false;
    static bool pgDropOpen = false;

    // ═══════════════════ LEFT PANEL ═════════════════════════════════
    GradV(dl, ImVec2(0, contentY), ImVec2(leftW, H-statusH), im(16,15,24), im(12,11,19));
    GradH(dl, ImVec2(leftW-2, contentY), ImVec2(leftW+2, H-statusH), im(0,0,0,0), im(0,0,0,50));
    dl->AddLine(ImVec2(leftW, contentY), ImVec2(leftW, H-statusH), im(45,42,60));

    // ── BPM (left panel, above pads) ──
    {
        float bpmY = contentY + 6;
        float bpmH = 52.f;
        float cx = leftW / 2.f;
        char bstr[16]; snprintf(bstr, sizeof(bstr), "%.1f", (double)app.bpm);
        if (fXl) ImGui::PushFont(fXl);
        ImVec2 bs = ImGui::CalcTextSize(bstr);
        dl->AddText(ImVec2(cx-bs.x/2+1, bpmY+bpmH/2-bs.y/2+1), im(180,140,40,30), bstr);
        dl->AddText(ImVec2(cx-bs.x/2,   bpmY+bpmH/2-bs.y/2),   im(255,215,80), bstr);
        if (fXl) ImGui::PopFont();
        if (app.swing > 0) {
            if (fTiny) ImGui::PushFont(fTiny);
            char swg[16]; snprintf(swg, sizeof(swg), "SW %d%%", app.swing);
            dl->AddText(ImVec2(cx+bs.x/2+8, bpmY+bpmH/2-6), im(100,95,140), swg);
            if (fTiny) ImGui::PopFont();
        }
        float btnW = 36.f, btnH = 36.f;
        float mpx = cx - bs.x/2 - btnW - 10.f;
        float ppx = cx + bs.x/2 + (app.swing > 0 ? 62.f : 10.f);
        float bby = bpmY + bpmH/2 - btnH/2;
        if (mpx < 4.f) mpx = 4.f;
        if (ppx + btnW > leftW - 4.f) ppx = leftW - 4.f - btnW;
        DrawPill(dl, mpx, bby, btnW, btnH, im(35,33,50), 10);
        DrawPill(dl, ppx, bby, btnW, btnH, im(35,33,50), 10);
        if (fBd) ImGui::PushFont(fBd);
        ImVec2 mts = ImGui::CalcTextSize("-"), pts = ImGui::CalcTextSize("+");
        dl->AddText(ImVec2(mpx+btnW/2-mts.x/2, bby+btnH/2-mts.y/2), im(180,170,220), "-");
        dl->AddText(ImVec2(ppx+btnW/2-pts.x/2, bby+btnH/2-pts.y/2), im(180,170,220), "+");
        if (fBd) ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(mpx, bby));
        ImGui::InvisibleButton("##bpm_m", ImVec2(btnW, btnH));
        if (!app.showMidi && ImGui::IsItemClicked()) app.bpm = std::max(20.f, app.bpm - 1.f);
        if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(mpx,bby), ImVec2(mpx+btnW,bby+btnH), im(80,75,120), 10);
        ImGui::SetCursorScreenPos(ImVec2(ppx, bby));
        ImGui::InvisibleButton("##bpm_p", ImVec2(btnW, btnH));
        if (!app.showMidi && ImGui::IsItemClicked()) app.bpm = std::min(300.f, app.bpm + 1.f);
        if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(ppx,bby), ImVec2(ppx+btnW,bby+btnH), im(80,75,120), 10);
        ImGui::SetCursorScreenPos(ImVec2(cx-40.f, bpmY+2.f));
        ImGui::InvisibleButton("##bpm_drag", ImVec2(80.f, bpmH-4.f));
        if (!app.showMidi && ImGui::IsItemActive() && ImGui::IsMouseDragging(0))
            app.bpm = std::clamp(app.bpm - io.MouseDelta.y * 0.4f, 20.f, 300.f);
        if (!app.showMidi && ImGui::IsItemHovered() && io.MouseWheel != 0)
            app.bpm = std::clamp(app.bpm + io.MouseWheel * 0.5f, 20.f, 300.f);
    }
    // ── Octave shift / CT drum page (below BPM, only when MIDI out is selected) ──
    if (app.midi.curOut() >= 0) {
        float octY = contentY + 6 + 52 + 8;
        float octH = 44.f;
        float cx2  = leftW / 2.f;
        float obW  = 72.f, obH = 34.f;
        float oby  = octY + octH/2 - obH/2;
        float omx  = cx2 - obW - 36.f - 8.f;
        float opx  = cx2 + 36.f + 8.f;
        bool isCT  = app.midi.outName().find("Circuit Tracks") != std::string::npos;
        bool isCTDrum = isCT && app.ctTrack >= 2;
        if (isCTDrum) {
            // Drum page selector instead of octave
            int pgStart = app.ctDrumPage * 16;
            char pgLbl[16]; snprintf(pgLbl, sizeof(pgLbl), "%d-%d", pgStart+1, pgStart+16);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ols = ImGui::CalcTextSize(pgLbl);
            dl->AddText(ImVec2(cx2 - ols.x/2, octY + octH/2 - ols.y/2), im(200,160,100), pgLbl);
            if (fSm) ImGui::PopFont();
            DrawPill(dl, omx, oby, obW, obH, im(35,33,50), 10);
            if (fSm) ImGui::PushFont(fSm);
            { ImVec2 s=ImGui::CalcTextSize("PG-"); dl->AddText(ImVec2(omx+obW/2-s.x/2,oby+obH/2-s.y/2),im(180,170,220),"PG-"); }
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(omx, oby));
            ImGui::InvisibleButton("##dpg_m", ImVec2(obW, obH));
            if (!app.showMidi && ImGui::IsItemClicked()) app.ctDrumPage = std::max(0, app.ctDrumPage-1);
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(omx,oby),ImVec2(omx+obW,oby+obH),im(80,75,120),10);
            DrawPill(dl, opx, oby, obW, obH, im(35,33,50), 10);
            if (fSm) ImGui::PushFont(fSm);
            { ImVec2 s=ImGui::CalcTextSize("PG+"); dl->AddText(ImVec2(opx+obW/2-s.x/2,oby+obH/2-s.y/2),im(180,170,220),"PG+"); }
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(opx, oby));
            ImGui::InvisibleButton("##dpg_p", ImVec2(obW, obH));
            if (!app.showMidi && ImGui::IsItemClicked()) app.ctDrumPage = std::min(3, app.ctDrumPage+1);
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(opx,oby),ImVec2(opx+obW,oby+obH),im(80,75,120),10);
        } else {
            // Normal octave buttons
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 ols = ImGui::CalcTextSize("OCTAVE");
            dl->AddText(ImVec2(cx2 - ols.x/2, octY + octH/2 - ols.y/2), im(120,115,160), "OCTAVE");
            if (fSm) ImGui::PopFont();
            DrawPill(dl, omx, oby, obW, obH, im(35,33,50), 10);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 mls = ImGui::CalcTextSize("OCT-");
            dl->AddText(ImVec2(omx+obW/2-mls.x/2, oby+obH/2-mls.y/2), im(180,170,220), "OCT-");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(omx, oby));
            ImGui::InvisibleButton("##oct_m", ImVec2(obW, obH));
            if (!app.showMidi && ImGui::IsItemClicked())
                for (int t2=0;t2<NUM_TRACKS;++t2) app.tracks[t2].note = std::clamp(app.tracks[t2].note-12,0,127);
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(omx,oby),ImVec2(omx+obW,oby+obH),im(80,75,120),10);
            DrawPill(dl, opx, oby, obW, obH, im(35,33,50), 10);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 pls2 = ImGui::CalcTextSize("OCT+");
            dl->AddText(ImVec2(opx+obW/2-pls2.x/2, oby+obH/2-pls2.y/2), im(180,170,220), "OCT+");
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(opx, oby));
            ImGui::InvisibleButton("##oct_p", ImVec2(obW, obH));
            if (!app.showMidi && ImGui::IsItemClicked())
                for (int t2=0;t2<NUM_TRACKS;++t2) app.tracks[t2].note = std::clamp(app.tracks[t2].note+12,0,127);
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(opx,oby),ImVec2(opx+obW,oby+obH),im(80,75,120),10);
        }
    }
    for (int p = 0; p < 16; ++p) {
        int col = p % 4, row = p / 4;
        float px = padGX + col * (padS + padGap);
        float py = padGY + row * (padS + padGap);
        ImU32 tc = TCLR[p];
        bool sel = (p == app.selTrk);
        float flt = (float)std::max(0.0, 1.0 - (now - app.padFlash[p]) / FLASH_DUR);
        bool fl = flt > 0.01f;

        // Shadow
        dl->AddRectFilled(ImVec2(px+2,py+2), ImVec2(px+padS+2,py+padS+2), im(0,0,0,70), 8);

        float br = fl ? (0.35f + 0.65f * flt) : (sel ? 0.32f : 0.13f);
        ImU32 faceTop = dimC(tc, br * 1.3f);
        ImU32 faceBot = dimC(tc, br * 0.75f);
        GradV(dl, ImVec2(px,py), ImVec2(px+padS,py+padS), faceTop, faceBot);

        // Inner shadow
        dl->AddRectFilled(ImVec2(px+1,py+1), ImVec2(px+padS-1,py+3), im(0,0,0, sel?15:35), 7);
        // Bottom catch
        dl->AddRectFilled(ImVec2(px+3,py+padS-2), ImVec2(px+padS-3,py+padS), withA(tc, sel?35:15), 2);

        dl->AddRect(ImVec2(px,py), ImVec2(px+padS,py+padS), im(0,0,0,70), 8, 0, 1.0f);
        dl->AddRect(ImVec2(px+1,py+1), ImVec2(px+padS-1,py+padS-1), withA(im(255,255,255), sel?16:5), 7);

        if (sel) DrawGlow(dl, px, py, (float)padS, (float)padS, tc, 4, 8);
        if (fl) DrawGlow(dl, px, py, (float)padS, (float)padS, tc, 5*flt, 8);

        // Pad name / note label
        bool hasMidiOut = app.midi.curOut() >= 0;
        bool isCTDrumPad = hasMidiOut &&
            app.midi.outName().find("Circuit Tracks") != std::string::npos &&
            app.ctTrack >= 2;
        std::string nn = noteName(app.tracks[p].note);
        char patchNumBuf[8];
        if (isCTDrumPad) snprintf(patchNumBuf, sizeof(patchNumBuf), "%d", app.ctDrumPage*16 + p);
        const char* mainLabel = isCTDrumPad ? patchNumBuf
                              : hasMidiOut  ? nn.c_str()
                              : app.tracks[p].name.c_str();
        // Highlight pads whose stored patch matches the current page
        bool isPatchSel = isCTDrumPad && (app.tracks[p].synthP[0] == app.ctDrumPage*16 + p);
        if (fSm) ImGui::PushFont(fSm);
        ImVec2 ns = ImGui::CalcTextSize(mainLabel);
        ImGui::PushClipRect(ImVec2(px+2,py), ImVec2(px+padS-2,py+padS), true);
        dl->AddText(ImVec2(px + padS/2.f - std::min(ns.x,(float)padS-6)/2.f, py + padS*0.38f - ns.y/2.f),
                    fl ? im(255,255,255) : (isPatchSel ? im(255,220,100) : (sel ? im(240,240,250) : im(180,180,200))), mainLabel);
        // Sub-label: stored patch value for CT drum pads, else track name in MIDI mode
        if (isCTDrumPad) {
            if (fTiny) { ImGui::PopFont(); ImGui::PushFont(fTiny); }
            char storedBuf[16]; snprintf(storedBuf, sizeof(storedBuf), "[%d]", app.tracks[p].synthP[0]);
            ImVec2 dns = ImGui::CalcTextSize(storedBuf);
            dl->AddText(ImVec2(px + padS/2.f - std::min(dns.x,(float)padS-6)/2.f, py + padS*0.62f - dns.y/2.f),
                        withA(tc, sel?90:55), storedBuf);
            if (fTiny) { ImGui::PopFont(); ImGui::PushFont(fSm); }
        } else if (hasMidiOut) {
            if (fTiny) { ImGui::PopFont(); ImGui::PushFont(fTiny); }
            ImVec2 dns = ImGui::CalcTextSize(app.tracks[p].name.c_str());
            dl->AddText(ImVec2(px + padS/2.f - std::min(dns.x,(float)padS-6)/2.f, py + padS*0.62f - dns.y/2.f),
                        withA(tc, sel?70:40), app.tracks[p].name.c_str());
            if (fTiny) { ImGui::PopFont(); ImGui::PushFont(fSm); }
        }
        ImGui::PopClipRect();
        if (fSm) ImGui::PopFont();

        // Pad number + note (bottom strip)
        if (fTiny) ImGui::PushFont(fTiny);
        char pn[4]; snprintf(pn, sizeof(pn), "%d", p+1);
        dl->AddText(ImVec2(px+4, py+padS-13), withA(tc, sel?110:50), pn);
        if (!hasMidiOut) {
            ImVec2 nns = ImGui::CalcTextSize(nn.c_str());
            dl->AddText(ImVec2(px+padS-nns.x-3, py+padS-13), withA(tc, sel?90:45), nn.c_str());
        }
        if (fTiny) ImGui::PopFont();

        ImGui::SetCursorScreenPos(ImVec2(px,py));
        char pid[16]; snprintf(pid,sizeof(pid),"##pad%d",p);
        ImGui::InvisibleButton(pid, ImVec2((float)padS,(float)padS));
        // Press: start hold timer
        if (!app.showMidi && !app.showPadEdit && !ctDropOpen && !pgDropOpen && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
            app.padHoldT = p;
            app.padHoldStart = ImGui::GetTime();
            app.padHoldTriggered = false;
        }
        // Hold: open track properties popup after 400ms
        if (app.padHoldT == p && !app.padHoldTriggered && ImGui::IsMouseDown(0)) {
            if (ImGui::GetTime() - app.padHoldStart >= 0.40) {
                app.selTrk = p;
                app.showPadEdit = true;
                app.padHoldTriggered = true;
            }
        }
        // Release: short tap = select + play note
        if (app.padHoldT == p && ImGui::IsMouseReleased(0)) {
            if (!app.padHoldTriggered && !app.showMidi) {
                app.selTrk = p; app.padFlash[p] = now;
                if (isCTDrumPad) {
                    int patchVal = app.ctDrumPage * 16 + p;
                    app.tracks[p].synthP[0] = patchVal;
                    int drumIdx = app.ctTrack - 2;
                    int drumNote = app.tracks[p].note;
                    // NoteOff first to clear voice, then select patch via CC, then fire
                    app.midi.noteOff(9, drumNote);
                    app.midi.sendCC(9, CT_DRUM_CC[drumIdx][0], patchVal);
                    app.midi.noteOn(9, drumNote, app.tracks[p].vel);
                    fprintf(stderr, "[CT DRUM tap ] drumIdx=%d CC=%d patch=%d note=%d\n",
                        drumIdx, CT_DRUM_CC[drumIdx][0], patchVal, drumNote);
                    app.pendingOffs.push_back({9, drumNote, now + 0.15});
                } else {
                    app.midi.noteOn(app.midiCh, app.tracks[p].note, app.tracks[p].vel);
                    app.pendingOffs.push_back({app.midiCh, app.tracks[p].note, now + 0.15});
                    if (app.midi.synthEnabled()) synthTrigger(app, p, (float)app.tracks[p].vel);
                }
            }
            app.padHoldT = -1;
        }
        if (ImGui::IsItemHovered() && !sel && !fl) {
            dl->AddRect(ImVec2(px,py), ImVec2(px+padS,py+padS), withA(tc, 70), 8, 0, 1.5f);
        }
    }

    // ── Bottom strip: CT track dropdown + param page dropdown ──
    {
        ImU32 ttc = TCLR[app.selTrk];
        Track& tst = app.tracks[app.selTrk];
        float tlY = synthTopY;
        GradV(dl, ImVec2(0,tlY), ImVec2(leftW,H-statusH), im(14,13,22), im(10,9,17));
        dl->AddLine(ImVec2(0,tlY), ImVec2(leftW,tlY), im(50,45,70));

        bool isCT   = app.midi.curOut() >= 0 &&
                      app.midi.outName().find("Circuit Tracks") != std::string::npos;
        bool isCTDr = isCT && app.ctTrack >= 2;

        // ctDropOpen / pgDropOpen declared in outer scope above — sync with popup state
        ctDropOpen = ImGui::IsPopupOpen("##ctpop");
        pgDropOpen = ImGui::IsPopupOpen("##pgpop");

        static const char* ctNames[]  = {"(none)","SY 1","SY 2","DR 1","DR 2","DR 3","DR 4"};
        static const int CT_DRUM_NOTES[4] = {60, 62, 64, 65};
        static const char* pgSynth[]  = {"SYNTH","EXT","FX","FX2"};
        static const char* pgMidi[]   = {"Page 1","Page 2","Page 3","Page 4"};
        static const char* pgDrum[]   = {"CTRL 1","CTRL 2"};

        const float dh   = 36.f;
        const float dpad = 6.f;
        float bx   = dpad, bw = leftW - 2*dpad;
        float row1Y = tlY + 8.f;
        float row2Y = row1Y + dh + dpad;

        // ── Row 1: CT track dropdown (CT only) or track info label ──
        if (isCT) {
            int ctIdx = app.ctTrack + 1;
            DrawPill(dl, bx, row1Y, bw, dh,
                ctDropOpen ? im(40,35,65) : im(22,20,36), 8);
            dl->AddRect(ImVec2(bx,row1Y),ImVec2(bx+bw,row1Y+dh),
                ctDropOpen ? im(110,100,180) : im(50,46,72), 8, 0, 1.2f);
            if (fSm) ImGui::PushFont(fSm);
            { char lbl[24]; snprintf(lbl,sizeof(lbl),"TRACK: %s",ctNames[ctIdx]);
              ImVec2 ls=ImGui::CalcTextSize(lbl);
              dl->AddText(ImVec2(bx+12,row1Y+dh/2-ls.y/2),im(180,175,225),lbl); }
            { ImVec2 as=ImGui::CalcTextSize("v");
              dl->AddText(ImVec2(bx+bw-as.x-10,row1Y+dh/2-as.y/2),im(130,125,170),"v"); }
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(bx,row1Y));
            ImGui::InvisibleButton("##ctdrop_btn",ImVec2(bw,dh));
            if (!app.showMidi && ImGui::IsItemClicked())
                ImGui::OpenPopup("##ctpop");

            // Position popup: prefer below, flip above if off-screen
            const int nI = 7;
            const float lh = 34.f;
            float totalH = nI*lh + 8;
            float ly = row1Y + dh + 2;
            if (ly + totalH > H - statusH) ly = row1Y - totalH - 2;
            ImGui::SetNextWindowPos(ImVec2(bx, ly));
            ImGui::SetNextWindowSize(ImVec2(bw, totalH));
            ImGui::SetNextWindowBgAlpha(0.f);
            if (ImGui::BeginPopup("##ctpop",
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoScrollWithMouse)) {
                ImDrawList* pdl = ImGui::GetWindowDrawList();
                pdl->AddRectFilled(ImVec2(bx,ly),ImVec2(bx+bw,ly+totalH),im(28,26,44,250),8);
                pdl->AddRect      (ImVec2(bx,ly),ImVec2(bx+bw,ly+totalH),im(80,75,120),8);
                for (int i=0; i<nI; ++i) {
                    float iy = ly + 4 + i*lh;
                    bool isSel = (app.ctTrack+1 == i);
                    ImGui::SetCursorScreenPos(ImVec2(bx+2, iy));
                    char iid[24]; snprintf(iid,sizeof(iid),"##ctitem%d",i);
                    ImGui::InvisibleButton(iid, ImVec2(bw-4, lh-1));
                    bool isHov = ImGui::IsItemHovered();
                    if (isSel)      pdl->AddRectFilled(ImVec2(bx+2,iy),ImVec2(bx+bw-2,iy+lh-1),im(45,42,80),6);
                    else if (isHov) pdl->AddRectFilled(ImVec2(bx+2,iy),ImVec2(bx+bw-2,iy+lh-1),im(38,35,60),6);
                    if (fSm) ImGui::PushFont(fSm);
                    ImVec2 ts=ImGui::CalcTextSize(ctNames[i]);
                    pdl->AddText(ImVec2(bx+14, iy+lh/2-ts.y/2),
                        isSel ? im(255,255,255) : im(170,165,210), ctNames[i]);
                    if (fSm) ImGui::PopFont();
                    if (ImGui::IsItemClicked()) {
                        app.ctTrack = i - 1;
                        if (app.ctTrack == -1) {
                            // (none) — restore default notes/channels
                            app.midiCh = 9;
                            for (int t = 0; t < NUM_TRACKS; ++t) {
                                app.tracks[t].ch   = TDEFS[t].note < 0 ? 9 : 9;
                                app.tracks[t].note = TDEFS[t].note;
                            }
                            app.synthPage = 0; app.ctDrumPage = 0;
                        } else if (app.ctTrack == 0) {
                            app.midiCh = 0;
                            // Restore default notes; channel set to MIDI ch1 for all
                            for (int t = 0; t < NUM_TRACKS; ++t) {
                                app.tracks[t].ch   = 0;
                                app.tracks[t].note = TDEFS[t].note;
                            }
                            app.synthPage = 0; app.ctDrumPage = 0;
                        } else if (app.ctTrack == 1) {
                            app.midiCh = 1;
                            for (int t = 0; t < NUM_TRACKS; ++t) {
                                app.tracks[t].ch   = 1;
                                app.tracks[t].note = TDEFS[t].note;
                            }
                            app.synthPage = 0; app.ctDrumPage = 0;
                        } else if (app.ctTrack >= 2) {
                            app.midiCh = 9;
                            app.synthPage = 0; app.ctDrumPage = 0;
                            // All pads trigger the same drum note for the selected drum voice
                            int drumNote = CT_DRUM_NOTES[app.ctTrack - 2];
                            for (int t = 0; t < NUM_TRACKS; ++t) {
                                app.tracks[t].ch   = 9;
                                app.tracks[t].note = drumNote;
                            }
                        }
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }
        } else {
            // No CT output — show track name/note info only
            if (fSm) ImGui::PushFont(fSm);
            char tlab[48]; snprintf(tlab,sizeof(tlab),"%s  CH %d  %s",
                tst.name.c_str(), app.midiCh+1, noteName(tst.note).c_str());
            ImVec2 tlabs=ImGui::CalcTextSize(tlab);
            dl->AddText(ImVec2(leftW/2-tlabs.x/2, row1Y+dh/2-7), ttc, tlab);
            if (fSm) ImGui::PopFont();
        }

        // ── Row 2: Param page dropdown ──
        {
            // CT drum: no page selector (all 9 shown at once); CT synth: 6 named pages; generic: 6 pages
            bool isCTSy = isCT && app.ctTrack >= 0 && app.ctTrack <= 1;
            bool isMF   = !isCT && app.midi.curOut() >= 0 &&
                          app.midi.outName().find("MicroFreak") != std::string::npos;
            const char** pgNames;
            int nPages;
            int& curPg = app.synthPage;
            if (isCTDr) {
                // Drums: no page needed — hide the dropdown row
                pgNames = nullptr; nPages = 0;
            } else if (isCTSy) {
                pgNames = CT_SYNTH_PAGE_NAMES; nPages = 19;
            } else if (isMF) {
                pgNames = MF_PAGE_NAMES; nPages = 4;
            } else {
                pgNames = (app.midi.curOut()>=0 ? pgMidi : pgSynth); nPages = 4;
            }
            if (curPg >= nPages && nPages > 0) curPg = 0;

            if (nPages > 0) {
            DrawPill(dl, bx, row2Y, bw, dh,
                pgDropOpen ? im(40,35,65) : im(22,20,36), 8);
            dl->AddRect(ImVec2(bx,row2Y),ImVec2(bx+bw,row2Y+dh),
                pgDropOpen ? withA(ttc,200) : im(50,46,72), 8, 0, 1.2f);
            if (fSm) ImGui::PushFont(fSm);
            { char lbl[28]; snprintf(lbl,sizeof(lbl),"PARAMS: %s",pgNames[curPg]);
              ImVec2 ls=ImGui::CalcTextSize(lbl);
              dl->AddText(ImVec2(bx+12,row2Y+dh/2-ls.y/2),im(180,175,225),lbl); }
            { ImVec2 as=ImGui::CalcTextSize("v");
              dl->AddText(ImVec2(bx+bw-as.x-10,row2Y+dh/2-as.y/2),im(130,125,170),"v"); }
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(bx,row2Y));
            ImGui::InvisibleButton("##pgdrop_btn",ImVec2(bw,dh));
            if (!app.showMidi && ImGui::IsItemClicked())
                ImGui::OpenPopup("##pgpop");

            const float lh = 34.f;
            float totalH = nPages*lh + 8;
            float ly = row2Y + dh + 2;
            if (ly + totalH > H - statusH) ly = row2Y - totalH - 2;
            ImGui::SetNextWindowPos(ImVec2(bx, ly));
            ImGui::SetNextWindowSize(ImVec2(bw, totalH));
            ImGui::SetNextWindowBgAlpha(0.f);
            if (ImGui::BeginPopup("##pgpop",
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoScrollWithMouse)) {
                ImDrawList* pdl = ImGui::GetWindowDrawList();
                pdl->AddRectFilled(ImVec2(bx,ly),ImVec2(bx+bw,ly+totalH),im(28,26,44,250),8);
                pdl->AddRect      (ImVec2(bx,ly),ImVec2(bx+bw,ly+totalH),im(80,75,120),8);
                for (int i=0; i<nPages; ++i) {
                    float iy = ly + 4 + i*lh;
                    bool isSel = (curPg == i);
                    ImGui::SetCursorScreenPos(ImVec2(bx+2, iy));
                    char pgid[24]; snprintf(pgid,sizeof(pgid),"##pgitem%d",i);
                    ImGui::InvisibleButton(pgid, ImVec2(bw-4, lh-1));
                    bool isHov = ImGui::IsItemHovered();
                    if (isSel)      pdl->AddRectFilled(ImVec2(bx+2,iy),ImVec2(bx+bw-2,iy+lh-1),im(45,42,80),6);
                    else if (isHov) pdl->AddRectFilled(ImVec2(bx+2,iy),ImVec2(bx+bw-2,iy+lh-1),im(38,35,60),6);
                    if (fSm) ImGui::PushFont(fSm);
                    ImVec2 ts=ImGui::CalcTextSize(pgNames[i]);
                    pdl->AddText(ImVec2(bx+14, iy+lh/2-ts.y/2),
                        isSel ? im(255,255,255) : im(170,165,210), pgNames[i]);
                    if (fSm) ImGui::PopFont();
                    if (ImGui::IsItemClicked()) { curPg = i; ImGui::CloseCurrentPopup(); }
                }
                ImGui::EndPopup();
            }
        }
    }
    }   // ── end Bottom strip block

    // ═══════════════════ SEQUENCER AREA ═════════════════════════════
    GradV(dl, ImVec2(divX+1, contentY), ImVec2(W, H-statusH), im(14,13,21), im(10,9,16));

    // ── Column header ──
    {
        float shY = contentY;
        GradV(dl, ImVec2(divX+1,shY), ImVec2(W,shY+seqHdrH), im(22,20,32), im(18,17,26));
        // Small page label in track-label column area
        {
            char pgStr[10]; snprintf(pgStr, sizeof(pgStr), "P%d/%d", app.viewPage+1, app.numPages);
            if (fTiny) ImGui::PushFont(fTiny);
            ImVec2 pls2 = ImGui::CalcTextSize(pgStr);
            dl->AddText(ImVec2(divX+lblW/2-pls2.x/2, shY+seqHdrH/2-pls2.y/2), im(80,75,115), pgStr);
            if (fTiny) ImGui::PopFont();
        }
        int vpOff = app.viewPage * NUM_STEPS;
        if (fTiny) ImGui::PushFont(fTiny);
        for (int s = 0; s < NUM_STEPS; ++s) {
            int gs = vpOff + s;  // global step index
            float sx2 = seqX + s * stepW + stepW/2;
            bool isCur = (app.playing || app.wasPaused) && (gs == app.curStep);
            if (s % 4 == 0 && s > 0) {
                dl->AddLine(ImVec2(seqX + s*stepW, contentY+seqHdrH),
                            ImVec2(seqX + s*stepW, H-statusH), im(35,33,50));
            }
            ImU32 nc = isCur ? im(255,220,80) : (s%4==0 ? im(110,105,150) : im(55,53,78));
            char nb[4]; snprintf(nb, sizeof(nb), "%d", gs+1);
            ImVec2 ns = ImGui::CalcTextSize(nb);
            dl->AddText(ImVec2(sx2-ns.x/2, shY+seqHdrH/2-ns.y/2), nc, nb);
        }
        if (fTiny) ImGui::PopFont();
    }

    // ── Playhead column ──
    if (app.playing) {
        int vpOff2 = app.viewPage * NUM_STEPS;
        int localStep = app.curStep - vpOff2;
        if (localStep >= 0 && localStep < NUM_STEPS) {
            float phX = seqX + localStep * stepW;
            GradV(dl, ImVec2(phX, contentY+seqHdrH), ImVec2(phX+stepW, H-statusH),
                   im(55,50,90,40), im(35,30,60,15));
            dl->AddRectFilled(ImVec2(phX, contentY+seqHdrH), ImVec2(phX+stepW, contentY+seqHdrH+2),
                              im(255,220,80,150));
        }
    }

    // ── Track rows & step cells ──
    for (int t = 0; t < NUM_TRACKS; ++t) {
        float ry = contentY + seqHdrH + t * rowH;
        const Track& tr = app.tracks[t];
        ImU32 tc = TCLR[t];
        bool sel = (t == app.selTrk);

        ImU32 rbg = sel ? im(24,22,40) : (t%2 ? im(13,12,20) : im(15,14,23));
        dl->AddRectFilled(ImVec2(divX+1, ry), ImVec2(W, ry+rowH), rbg);

        if (sel) {
            dl->AddRectFilled(ImVec2(divX+1, ry), ImVec2(divX+3, ry+rowH), tc);
            dl->AddRectFilled(ImVec2(divX+1, ry), ImVec2(W, ry+0.5f), withA(tc, 25));
            dl->AddRectFilled(ImVec2(divX+1, ry+rowH-0.5f), ImVec2(W, ry+rowH), withA(tc, 12));
        }

        // Track label
        {
            float lx = divX + 3, lw = lblW - 6, lh = rowH - 3, ly = ry + 1.5f;
            ImU32 lbg = sel ? withA(tc, 40) : withA(tc, 12);
            dl->AddRectFilled(ImVec2(lx,ly), ImVec2(lx+lw,ly+lh), lbg, 4);
            if (fTiny) ImGui::PushFont(fTiny);
            const char* sn[16] = {"KK","SN","HH","OH","LP","HT","MT","LT",
                                  "CL","SH","CB","RD","CR","T1","T2","PC"};
            ImVec2 ns = ImGui::CalcTextSize(sn[t]);
            dl->AddText(ImVec2(lx+lw/2-ns.x/2, ly+lh/2-ns.y/2), sel ? tc : dimC(tc, 0.7f), sn[t]);
            if (fTiny) ImGui::PopFont();
        }
        ImGui::SetCursorScreenPos(ImVec2(divX, ry));
        char tlid[16]; snprintf(tlid,sizeof(tlid),"##tl%d",t);
        ImGui::InvisibleButton(tlid, ImVec2(lblW, rowH));
        if (!app.showMidi && ImGui::IsItemClicked()) app.selTrk = t;

        // ── Step cells ── use viewPage offset
        int vpOff3 = app.viewPage * NUM_STEPS;
        for (int s = 0; s < NUM_STEPS; ++s) {
            int gs = vpOff3 + s;  // global step index
            float bx = seqX + s * stepW + 1.0f;
            float by2 = ry + 1.5f;
            float bw = stepW - 2.0f;
            float bh = rowH - 3.0f;
            const Step& step = tr.steps[gs];
            bool on = step.on;
            bool past = (gs >= tr.patLen);
            bool isCur = (app.playing || app.wasPaused) && (gs == app.curStep);
            float flt2 = (float)std::max(0.0, 1.0 - (now - app.stepFlash[t][gs]) / FLASH_DUR);
            bool fl = flt2 > 0.01f;

            if (past) {
                dl->AddRectFilled(ImVec2(bx,by2), ImVec2(bx+bw,by2+bh), im(8,7,13), 3);
                continue;
            }

            // Cell background
            bool beatHead = (s % 4 == 0);
            ImU32 cellBg;
            if (on) {
                cellBg = isCur ? withA(tc, 40) : withA(tc, 20);
            } else {
                cellBg = isCur ? im(28,26,44) : (beatHead ? im(19,18,28) : im(14,13,21));
            }
            dl->AddRectFilled(ImVec2(bx,by2), ImVec2(bx+bw,by2+bh), cellBg, 4);

            if (on) {
                // ── Velocity fill bar (modern: bottom-up gradient) ──
                float fillFrac = (float)step.vel / 127.f;
                float fillH = std::max(2.f, fillFrac * (bh - 4));
                float fillTop = by2 + bh - 2 - fillH;

                ImU32 vcTop = fl ? withA(tc, (int)(120+135*flt2)) : (isCur ? lerpC(tc, im(255,255,255), 0.15f) : dimC(tc, 0.55f));
                ImU32 vcBot = fl ? tc : (isCur ? tc : dimC(tc, 0.8f));
                GradV(dl, ImVec2(bx+2, fillTop), ImVec2(bx+bw-2, by2+bh-2), withA(vcTop,160), vcBot);

                // Border
                ImU32 bc = fl ? lerpC(tc, im(255,255,255), 0.4f*flt2) : (isCur ? lerpC(tc,im(255,255,255),0.15f) : dimC(tc, 0.4f));
                dl->AddRect(ImVec2(bx,by2), ImVec2(bx+bw,by2+bh), bc, 4, 0, 1.0f);

                if (fl) DrawGlow(dl, bx, by2, bw, bh, tc, 3*flt2, 4);

                // ── Probability indicator (if < 100%) ──
                if (step.prob < 100) {
                    // Small circle in top-right: filled proportionally to probability
                    float dotR = 3.0f;
                    float dotX = bx + bw - dotR - 2;
                    float dotY = by2 + dotR + 2;
                    ImU32 dotBg = im(0,0,0,100);
                    dl->AddCircleFilled(ImVec2(dotX, dotY), dotR+0.5f, dotBg, 12);
                    // Partial fill using arc approximation — just dim the dot based on prob
                    int alpha = 80 + (int)(step.prob * 1.75f);
                    ImU32 dotFg = im(255, 200, 60, alpha);
                    dl->AddCircleFilled(ImVec2(dotX, dotY), dotR, dotFg, 12);
                    // If prob very low, show text
                    if (bw > 28 && step.prob <= 50) {
                        if (fTiny) ImGui::PushFont(fTiny);
                        char pb[5]; snprintf(pb, sizeof(pb), "%d", step.prob);
                        ImVec2 pbs = ImGui::CalcTextSize(pb);
                        if (pbs.x < bw - 6)
                            dl->AddText(ImVec2(bx + bw/2 - pbs.x/2, by2 + 1), im(255,200,60,160), pb);
                        if (fTiny) ImGui::PopFont();
                    }
                }

                // ── Ratchet indicator (if > 1) ──
                if (step.ratchet > 1) {
                    // Small horizontal lines at bottom-left
                    float rx = bx + 3;
                    float rby = by2 + bh - 4;
                    for (int ri = 0; ri < step.ratchet && ri < 4; ++ri) {
                        float lx = rx + ri * 4;
                        dl->AddLine(ImVec2(lx, rby), ImVec2(lx+2.5f, rby), im(180,100,255,200), 1.5f);
                    }
                }
            } else {
                // Inactive cell
                dl->AddRect(ImVec2(bx,by2), ImVec2(bx+bw,by2+bh), im(28,26,42), 4);
            }

            // Current step top indicator
            if (isCur) {
                dl->AddRectFilled(ImVec2(bx+1,ry), ImVec2(bx+bw-1,ry+1.5f),
                                  on ? lerpC(tc,im(255,255,255),0.35f) : im(160,150,210,140), 1);
            }

            // Click  — left click toggles on/off; hold opens step editor
            char sid[24]; snprintf(sid,sizeof(sid),"##s%d_%d",t,gs);
            ImGui::SetCursorScreenPos(ImVec2(bx, by2));
            ImGui::InvisibleButton(sid, ImVec2(bw, bh));
            // Press: start hold timer (ignore if click is inside the popup)
            if (!popupConsumedClick && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
                app.holdT = t; app.holdS = gs;
                app.holdStart = ImGui::GetTime();
                app.holdTriggered = false;
            }
            // Hold: open popup after 400 ms (active steps only)
            if (app.holdT == t && app.holdS == gs && !app.holdTriggered && ImGui::IsMouseDown(0) && on) {
                if (ImGui::GetTime() - app.holdStart >= 0.40) {
                    if (app.editT == t && app.editS == gs)
                        { app.editT = -1; app.editS = -1; }
                    else
                        { app.editT = t; app.editS = gs; }
                    app.holdTriggered = true;
                }
            }
            // Release: toggle on/off (short tap only)
            if (app.holdT == t && app.holdS == gs && ImGui::IsMouseReleased(0)) {
                if (!app.holdTriggered) {
                    if (!on) {
                        app.tracks[t].steps[gs].on = true;
                    } else {
                        app.tracks[t].steps[gs].on = false;
                        if (app.editT == t && app.editS == gs) { app.editT = -1; app.editS = -1; }
                    }
                }
                app.holdT = -1; app.holdS = -1;
            }
            if (ImGui::IsItemClicked(1) && on) {
                app.velDrag = true; app.vdT = t; app.vdS = s;
                app.vdY0 = (int)io.MousePos.y; app.vdV0 = step.vel;
            }
            if (ImGui::IsItemHovered() && !on)
                dl->AddRect(ImVec2(bx,by2), ImVec2(bx+bw,by2+bh), withA(tc, 45), 4);
            // Velocity drag tooltip
            if (app.velDrag && app.vdT == t && app.vdS == s) {
                if (fSm) ImGui::PushFont(fSm);
                char vd[32]; snprintf(vd, sizeof(vd), "V:%d P:%d%% R:%dx",
                    app.tracks[t].steps[s].vel, app.tracks[t].steps[s].prob, app.tracks[t].steps[s].ratchet);
                ImVec2 vs2 = ImGui::CalcTextSize(vd);
                float ttx = bx + bw/2 - vs2.x/2 - 4;
                float tty = by2 - 20;
                dl->AddRectFilled(ImVec2(ttx, tty), ImVec2(ttx+vs2.x+8, tty+16), im(0,0,0,210), 4);
                dl->AddText(ImVec2(ttx+4, tty+1), im(255,220,80), vd);
                if (fSm) ImGui::PopFont();
            }
        }
        dl->AddLine(ImVec2(divX+1, ry+rowH-0.5f), ImVec2(W, ry+rowH-0.5f), im(22,20,35));
    }

    // Velocity drag motion
    if (app.velDrag) {
        if (ImGui::IsMouseDown(1)) {
            int dy = app.vdY0 - (int)io.MousePos.y;
            int nv = std::clamp(app.vdV0 + dy, 1, 127);
            app.tracks[app.vdT].steps[app.vdS].vel = nv;
        } else { app.velDrag = false; }
    }

    // ═══════════════════ STEP EDITOR POPUP ═════════════════════════
    if (app.editT >= 0 && app.editS >= 0) {
        Step& est = app.tracks[app.editT].steps[app.editS];
        ImU32 etc = TCLR[app.editT];

        if (!est.on) { app.editT = -1; app.editS = -1; }
        else {
            // Use page-relative index for cell position
            float popW = 310, popH = 282;
            int localSE = app.editS - app.viewPage * NUM_STEPS;
            float cellX = seqX + localSE * stepW;
            float cellY = contentY + seqHdrH + app.editT * rowH;
            float popX = cellX + stepW/2.f - popW/2.f;
            float popY = cellY - popH - 10;

            if (popX < 4) popX = 4;
            if (popX + popW > W - 4) popX = W - popW - 4;
            if (popY < hdrH + 2) popY = cellY + rowH + 10;

            // Arrow pointing to step cell
            float arrowX = std::clamp(cellX + stepW/2.f, popX+12, popX+popW-12);
            bool above = (popY < cellY);
            if (above) {
                ImVec2 tri[3]={{arrowX-8,popY+popH},{arrowX+8,popY+popH},{arrowX,popY+popH+8}};
                dl->AddTriangleFilled(tri[0],tri[1],tri[2],im(28,26,42));
            } else {
                ImVec2 tri[3]={{arrowX-8,popY},{arrowX+8,popY},{arrowX,popY-8}};
                dl->AddTriangleFilled(tri[0],tri[1],tri[2],im(28,26,42));
            }

            // Panel bg + shadow
            dl->AddRectFilled(ImVec2(popX+4,popY+4), ImVec2(popX+popW+4,popY+popH+4), im(0,0,0,130), 12);
            GradV(dl, ImVec2(popX,popY), ImVec2(popX+popW,popY+popH), im(32,30,48), im(22,20,36));
            dl->AddRect(ImVec2(popX,popY), ImVec2(popX+popW,popY+popH), withA(etc,90), 12, 0, 1.5f);
            dl->AddRectFilled(ImVec2(popX+2,popY+1), ImVec2(popX+popW-2,popY+2), withA(etc,40), 2);

            app.editPopupX = popX; app.editPopupY = popY;
            app.editPopupW = popW; app.editPopupH = popH;

            // ── Title stripe (44px) + large close button ──
            const float titleH = 44.f;
            GradV(dl, ImVec2(popX,popY), ImVec2(popX+popW,popY+titleH), im(38,35,55), im(28,26,42));
            dl->AddRectFilled(ImVec2(popX,popY), ImVec2(popX+3,popY+titleH), etc);
            dl->AddRectFilled(ImVec2(popX+2,popY+titleH-1), ImVec2(popX+popW-2,popY+titleH), withA(etc,30), 1);

            if (fBd) ImGui::PushFont(fBd);
            char stitle[24]; snprintf(stitle,sizeof(stitle),"STEP %d",app.editS+1);
            ImVec2 sts = ImGui::CalcTextSize(stitle);
            dl->AddText(ImVec2(popX+12, popY+titleH/2-sts.y/2), etc, stitle);
            if (fBd) ImGui::PopFont();

            // Close button — 48×36 touch target
            {
                float obw=48, obh=36, obx=popX+popW-obw-6, oby=popY+titleH/2-obh/2;
                bool chov=ImGui::IsMouseHoveringRect(ImVec2(obx,oby),ImVec2(obx+obw,oby+obh),false);
                dl->AddRectFilled(ImVec2(obx,oby),ImVec2(obx+obw,oby+obh),
                                  chov?im(100,90,130,240):im(50,45,70,180),8);
                if (chov) dl->AddRect(ImVec2(obx,oby),ImVec2(obx+obw,oby+obh),withA(etc,150),8);
                if (fSm) ImGui::PushFont(fSm);
                ImVec2 xs=ImGui::CalcTextSize("X");
                dl->AddText(ImVec2(obx+obw/2-xs.x/2,oby+obh/2-xs.y/2),im(210,200,230),"X");
                if (fSm) ImGui::PopFont();
                if (chov&&ImGui::IsMouseClicked(0)){app.editT=-1;app.editS=-1;}
            }

            float py3 = popY + titleH + 14;

            // ── VELOCITY ──
            if (app.editT >= 0) {
                if (fSm) ImGui::PushFont(fSm);
                dl->AddText(ImVec2(popX+12, py3), im(100,95,140), "VELOCITY");
                char vt[8]; snprintf(vt,sizeof(vt),"%d",est.vel);
                ImVec2 vts=ImGui::CalcTextSize(vt);
                dl->AddText(ImVec2(popX+popW-12-vts.x, py3), im(220,210,255), vt);
                if (fSm) ImGui::PopFont();
                py3 += 22;

                {
                    float slx=popX+12, slw=popW-24, slh=36;
                    dl->AddRectFilled(ImVec2(slx,py3),ImVec2(slx+slw,py3+slh),im(18,16,28),8);
                    dl->AddRect(ImVec2(slx,py3),ImVec2(slx+slw,py3+slh),im(35,33,52),8);
                    float fillW=(float)est.vel/127.f*(slw-4);
                    if (fillW>0) GradH(dl,ImVec2(slx+2,py3+3),ImVec2(slx+2+fillW,py3+slh-3),dimC(etc,0.4f),etc);
                    float knobX=slx+2+fillW;
                    dl->AddCircleFilled(ImVec2(knobX,py3+slh/2),10.f,im(225,220,245));
                    dl->AddCircle(ImVec2(knobX,py3+slh/2),10.f,withA(etc,160),16,2.0f);
                    ImGui::SetCursorScreenPos(ImVec2(slx,py3));
                    ImGui::InvisibleButton("##vel_sl",ImVec2(slw,slh));
                    if (ImGui::IsItemActive()){
                        float rel=(io.MousePos.x-slx-2)/(slw-4);
                        est.vel=std::clamp((int)(rel*127),1,127);
                    }
                }
                py3 += 48;

                // ── PROBABILITY ──
                if (fSm) ImGui::PushFont(fSm);
                dl->AddText(ImVec2(popX+12, py3), im(100,95,140), "PROBABILITY");
                if (fSm) ImGui::PopFont();
                py3 += 22;

                {
                    static const int pvals[]={25,50,75,100};
                    static const char* plbls[]={"25%","50%","75%","100%"};
                    float pbtnW=(popW-24-9)/4.f, pbtnH=44;
                    for (int pi=0; pi<4; ++pi) {
                        float bx2=popX+12+pi*(pbtnW+3);
                        bool hov2=ImGui::IsMouseHoveringRect(ImVec2(bx2,py3),ImVec2(bx2+pbtnW,py3+pbtnH),false);
                        bool sel3=(est.prob==pvals[pi]);
                        dl->AddRectFilled(ImVec2(bx2,py3),ImVec2(bx2+pbtnW,py3+pbtnH),
                            sel3?im(200,160,40):(hov2?im(50,46,68):im(30,28,45)),8);
                        if (sel3) dl->AddRect(ImVec2(bx2,py3),ImVec2(bx2+pbtnW,py3+pbtnH),im(255,220,60),8,0,1.5f);
                        else if (hov2) dl->AddRect(ImVec2(bx2,py3),ImVec2(bx2+pbtnW,py3+pbtnH),im(200,160,40,100),8);
                        if (fSm) ImGui::PushFont(fSm);
                        ImVec2 pls=ImGui::CalcTextSize(plbls[pi]);
                        dl->AddText(ImVec2(bx2+pbtnW/2-pls.x/2,py3+pbtnH/2-pls.y/2),
                            sel3?im(30,25,10):im(185,180,215),plbls[pi]);
                        if (fSm) ImGui::PopFont();
                        if (hov2&&ImGui::IsMouseClicked(0)) est.prob=pvals[pi];
                    }
                }
                py3 += 56;

                // ── RATCHET ──
                if (fSm) ImGui::PushFont(fSm);
                dl->AddText(ImVec2(popX+12, py3), im(100,95,140), "RATCHET");
                if (fSm) ImGui::PopFont();
                py3 += 22;

                {
                    static const int rvals[]={1,2,3,4};
                    static const char* rlbls[]={"1x","2x","3x","4x"};
                    float rbtnW=(popW-24-9)/4.f, rbtnH=44;
                    for (int ri=0; ri<4; ++ri) {
                        float bx2=popX+12+ri*(rbtnW+3);
                        bool hov2=ImGui::IsMouseHoveringRect(ImVec2(bx2,py3),ImVec2(bx2+rbtnW,py3+rbtnH),false);
                        bool sel3=(est.ratchet==rvals[ri]);
                        dl->AddRectFilled(ImVec2(bx2,py3),ImVec2(bx2+rbtnW,py3+rbtnH),
                            sel3?im(140,80,220):(hov2?im(50,46,68):im(30,28,45)),8);
                        if (sel3) dl->AddRect(ImVec2(bx2,py3),ImVec2(bx2+rbtnW,py3+rbtnH),im(180,120,255),8,0,1.5f);
                        else if (hov2) dl->AddRect(ImVec2(bx2,py3),ImVec2(bx2+rbtnW,py3+rbtnH),im(140,80,220,100),8);
                        if (fSm) ImGui::PushFont(fSm);
                        ImVec2 rls=ImGui::CalcTextSize(rlbls[ri]);
                        dl->AddText(ImVec2(bx2+rbtnW/2-rls.x/2,py3+rbtnH/2-rls.y/2),
                            sel3?im(240,220,255):im(185,180,215),rlbls[ri]);
                        if (fSm) ImGui::PopFont();
                        if (hov2&&ImGui::IsMouseClicked(0)) est.ratchet=rvals[ri];
                    }
                }
            }

            if (ImGui::IsMouseClicked(0) && app.editT >= 0) {
                ImVec2 mp2=io.MousePos;
                bool inPop=(mp2.x>=popX&&mp2.x<=popX+popW&&mp2.y>=popY&&mp2.y<=popY+popH);
                bool inSeq=(mp2.x>=seqX&&mp2.x<=W&&mp2.y>=contentY+seqHdrH&&mp2.y<=contentY+seqHdrH+NUM_TRACKS*rowH);
                if (!inPop&&!inSeq){app.editT=-1;app.editS=-1;}
            }
        }
    }

    // ═══════════════════ DRUM SYNTH PANEL ═════════════════════════
    {
        Track& st = app.tracks[app.selTrk];
        ImU32 tc = TCLR[app.selTrk];
        int dt = st.dtype;
        ImU32 ac = tc;  // slider accent matches pad color
        float spX = divX + 1;                 // panel left
        float spW = W - spX;                  // panel width
        float spY = synthTopY;                // panel top
        float spH = H - statusH - spY;        // panel height

        // Background
        GradV(dl, ImVec2(spX, spY), ImVec2(W, spY+spH), im(18,17,28), im(12,11,20));
        dl->AddLine(ImVec2(spX, spY), ImVec2(W, spY), im(40,38,58));

        bool isCT2   = app.midi.curOut() >= 0 &&
                        app.midi.outName().find("Circuit Tracks") != std::string::npos;
        bool isCTDr2 = isCT2 && app.ctTrack >= 2;
        bool isCTSy2 = isCT2 && app.ctTrack >= 0 && app.ctTrack <= 1;
        int  ctDrumIdx2 = app.ctTrack - 2; // 0-3 when isCTDr2
        bool isMF2  = !isCT2 && app.midi.curOut() >= 0 &&
                      app.midi.outName().find("MicroFreak") != std::string::npos;

        // ── CT DRUM: 3-column × 3-row grid, all 9 params on one panel ──
        if (isCTDr2) {
            const int CT_DRUM_TOTAL = 9;
            const int CT_DRUM_COLS  = 3;
            const int CT_DRUM_ROWS  = 3;
            float colW3 = (spW - (CT_DRUM_COLS+1)*8.f) / CT_DRUM_COLS;
            float paramRH3 = (spH - 12.f) / CT_DRUM_ROWS;
            if (paramRH3 > 36) paramRH3 = 36;
            float paramY0 = spY + 6;

            for (int pi = 0; pi < CT_DRUM_TOTAL; ++pi) {
                int col = pi % CT_DRUM_COLS;
                int row = pi / CT_DRUM_COLS;
                float cx = spX + 8 + col * (colW3 + 8);
                float cy = paramY0 + row * paramRH3;

                if (row % 2 == 0)
                    dl->AddRectFilled(ImVec2(cx-2,cy),ImVec2(cx+colW3+2,cy+paramRH3-2),im(20,18,32,120),3);

                const char* pname = CT_DRUM_PNAMES[pi];
                int& pval = st.synthP[pi]; // reuse synthP[0..8] for drum params
                // PATCH (pi==0) valid range is 0-63; clamp stored value
                const int pmax = (pi == 0) ? 63 : 127;
                pval = std::min(pval, pmax);

                if (fSm) ImGui::PushFont(fSm);
                dl->AddText(ImVec2(cx+2, cy+paramRH3/2-7), im(210,195,255), pname);
                if (fSm) ImGui::PopFont();

                if (fTiny) ImGui::PushFont(fTiny);
                char pvs[8]; snprintf(pvs,sizeof(pvs),"%d",pval);
                ImVec2 pvSz = ImGui::CalcTextSize(pvs);
                dl->AddText(ImVec2(cx+colW3-pvSz.x-2, cy+paramRH3/2-6), im(200,195,230), pvs);
                if (fTiny) ImGui::PopFont();

                float lblOff = 48.f, valOff = 28.f;
                float slx = cx + lblOff;
                float slw = colW3 - lblOff - valOff;
                float slh = 14;
                float sly = cy + paramRH3/2 - slh/2;

                dl->AddRectFilled(ImVec2(slx,sly),ImVec2(slx+slw,sly+slh),im(14,12,24),7);
                dl->AddRect      (ImVec2(slx,sly),ImVec2(slx+slw,sly+slh),im(30,28,45),7);
                float fillFrac = (float)pval / (float)pmax;
                float fillW = fillFrac * (slw-4);
                if (fillW>0) GradH(dl,ImVec2(slx+2,sly+2),ImVec2(slx+2+fillW,sly+slh-2),dimC(ac,0.35f),dimC(ac,0.85f));
                float knobX = slx+2+fillW;
                dl->AddCircleFilled(ImVec2(knobX,sly+slh/2),5.5f,im(210,205,235));
                dl->AddCircle      (ImVec2(knobX,sly+slh/2),5.5f,withA(ac,140),12,1.2f);

                char spid[24]; snprintf(spid,sizeof(spid),"##dsp%d",pi);
                ImGui::SetCursorScreenPos(ImVec2(slx-4,sly-16));
                ImGui::InvisibleButton(spid,ImVec2(slw+8,slh+32));
                if (!app.showMidi && ImGui::IsItemActive()) {
                    float rel = (io.MousePos.x - slx - 2) / (slw - 4);
                    int nv = std::clamp((int)(rel * pmax), 0, pmax);
                    if (nv != pval) {
                        pval = nv;
                        int sendCh  = CT_DRUM_PARAM_CH[pi];
                        int sendCC  = CT_DRUM_CC[ctDrumIdx2][pi];
                        app.midi.sendCC(sendCh, sendCC, pval);
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    dl->AddRect(ImVec2(slx-1,sly-1),ImVec2(slx+slw+1,sly+slh+1),withA(ac,80),8);
                }
            }
        } else {
            // ── SYNTH / generic paged panel ──
            const float tH = 0.f;
            float paramY0 = spY + 6;
            float colW = (spW - 24) / 2.f;
            float paramRH = (spH - tH - 12) / 3.f;
            if (paramRH > 36) paramRH = 36;

            int pageOff = app.synthPage * SYNTH_PARAMS_PER_PAGE;
            for (int pi = 0; pi < SYNTH_PARAMS_PER_PAGE; ++pi) {
                int actualIdx = pageOff + pi;
                int col = pi / 3;
                int row = pi % 3;
                float cx = spX + 8 + col * (colW + 8);
                float cy = paramY0 + row * paramRH;

                if (row % 2 == 0)
                    dl->AddRectFilled(ImVec2(cx-2,cy),ImVec2(cx+colW+2,cy+paramRH-2),im(20,18,32,120),3);

                const char* pname;
                char ccLabel[16];
                const char* labelStr;
                if (isMF2 && MF_CC[app.synthPage][pi] < 0) continue; // skip unused MF slot
                bool isCTModPage = isCTSy2 && app.synthPage >= 9;
                if (isCTModPage && pi >= 4) continue; // only 4 params on MOD pages
                if (isCTSy2) {
                    // CT synth: CC pages 0-8, NRPN mod pages 9-18
                    pname = isCTModPage ? CT_MOD_PNAMES[pi] : CT_SYNTH_PNAMES[app.synthPage][pi];
                    labelStr = pname;
                } else if (isMF2) {
                    // MicroFreak: use MF-specific names
                    pname = MF_PNAMES[app.synthPage][pi];
                    // Show oscillator type label for OSC page, TYPE param
                    if (app.synthPage == 0 && pi == 0) {
                        int oscType = st.synthP[0];
                        labelStr = MF_OSC_TYPE_NAMES[std::clamp(oscType,0,(int)(sizeof(MF_OSC_TYPE_NAMES)/sizeof(MF_OSC_TYPE_NAMES[0])-1))];
                    } else {
                        labelStr = pname;
                    }
                } else if (app.midi.curOut() >= 0) {
                    // Generic MIDI out: show CC numbers
                    snprintf(ccLabel, sizeof(ccLabel), "CC%d", SYNTH_CC[actualIdx]);
                    labelStr = ccLabel;
                    pname = ccLabel;
                } else {
                    // Internal synth: use synth type names
                    pname = app.synthPage == 0 ? SYNTH_PNAMES[dt][pi] :
                            app.synthPage == 1 ? SYNTH_PNAMES_EXT[dt][pi] :
                            app.synthPage == 2 ? SYNTH_PNAMES_P2[pi] :
                                                 SYNTH_PNAMES_P3[pi];
                    labelStr = pname;
                }
                int& pval = st.synthP[actualIdx];

                // MOD pages: SRC/DEST use < > picker, DEPTH uses slider
                if (isCTModPage && pi != 2) {
                    int maxVal = (pi == 3) ? 17 : 12; // DEST 0-17, SRC 0-12
                    pval = std::clamp(pval, 0, maxVal);
                    const char* valLbl = (pi == 3) ? CT_MOD_DEST[pval] : CT_MOD_SRC[pval];
                    // label
                    if (fSm) ImGui::PushFont(fSm);
                    ImVec2 lbs = ImGui::CalcTextSize(labelStr);
                    dl->AddText(ImVec2(cx+2, cy+paramRH/2-lbs.y/2), im(210,195,255), labelStr);
                    if (fSm) ImGui::PopFont();
                    // < button
                    float bW = 22.f, bH = 20.f;
                    float bY = cy + paramRH/2 - bH/2;
                    float lbx = cx + 54;
                    float rbx = cx + colW - bW - 2;
                    bool lhv = ImGui::IsMouseHoveringRect(ImVec2(lbx,bY),ImVec2(lbx+bW,bY+bH),false);
                    bool rhv = ImGui::IsMouseHoveringRect(ImVec2(rbx,bY),ImVec2(rbx+bW,bY+bH),false);
                    dl->AddRectFilled(ImVec2(lbx,bY),ImVec2(lbx+bW,bY+bH),lhv?im(55,50,80):im(28,26,44),6);
                    dl->AddRectFilled(ImVec2(rbx,bY),ImVec2(rbx+bW,bY+bH),rhv?im(55,50,80):im(28,26,44),6);
                    if (fBd) ImGui::PushFont(fBd);
                    ImVec2 lcs=ImGui::CalcTextSize("<"),rcs=ImGui::CalcTextSize(">");
                    dl->AddText(ImVec2(lbx+bW/2-lcs.x/2,bY+bH/2-lcs.y/2),lhv?im(230,225,255):im(130,125,165),"<");
                    dl->AddText(ImVec2(rbx+bW/2-rcs.x/2,bY+bH/2-rcs.y/2),rhv?im(230,225,255):im(130,125,165),">");
                    if (fBd) ImGui::PopFont();
                    // value label centered between buttons
                    if (fTiny) ImGui::PushFont(fTiny);
                    ImVec2 vls = ImGui::CalcTextSize(valLbl);
                    float midX = lbx + bW + (rbx - lbx - bW)/2.f;
                    dl->AddText(ImVec2(midX - vls.x/2, cy+paramRH/2-vls.y/2), im(220,215,245), valLbl);
                    if (fTiny) ImGui::PopFont();
                    // hit areas
                    char lid[24], rid[24];
                    snprintf(lid,sizeof(lid),"##ml%d_%d",actualIdx,pi);
                    snprintf(rid,sizeof(rid),"##mr%d_%d",actualIdx,pi);
                    ImGui::SetCursorScreenPos(ImVec2(lbx,bY)); ImGui::InvisibleButton(lid,ImVec2(bW,bH));
                    if (!app.showMidi && ImGui::IsItemClicked()) {
                        pval = std::clamp(pval-1,0,maxVal);
                        int slot=app.synthPage-9;
                        app.midi.sendNRPN(app.midiCh,CT_MOD_NRPN[slot][pi][0],CT_MOD_NRPN[slot][pi][1],pval);
                    }
                    ImGui::SetCursorScreenPos(ImVec2(rbx,bY)); ImGui::InvisibleButton(rid,ImVec2(bW,bH));
                    if (!app.showMidi && ImGui::IsItemClicked()) {
                        pval = std::clamp(pval+1,0,maxVal);
                        int slot=app.synthPage-9;
                        app.midi.sendNRPN(app.midiCh,CT_MOD_NRPN[slot][pi][0],CT_MOD_NRPN[slot][pi][1],pval);
                    }
                    continue;
                }

                if (fSm) ImGui::PushFont(fSm);
                dl->AddText(ImVec2(cx+2, cy+paramRH/2-7), im(210,195,255), labelStr);
                if (fSm) ImGui::PopFont();

                if (fTiny) ImGui::PushFont(fTiny);
                char pvs[8]; snprintf(pvs,sizeof(pvs),"%d",pval);
                ImVec2 pvSz = ImGui::CalcTextSize(pvs);
                dl->AddText(ImVec2(cx+colW-pvSz.x-2, cy+paramRH/2-6), im(200,195,230), pvs);
                if (fTiny) ImGui::PopFont();

                float lblOff = 56.f, valOff = 32.f;
                float slx = cx + lblOff;
                float slw = colW - lblOff - valOff;
                float slh = 14;
                float sly = cy + paramRH/2 - slh/2;

                dl->AddRectFilled(ImVec2(slx,sly),ImVec2(slx+slw,sly+slh),im(14,12,24),7);
                dl->AddRect      (ImVec2(slx,sly),ImVec2(slx+slw,sly+slh),im(30,28,45),7);
                float fillFrac = (float)pval/127.f;
                float fillW = fillFrac*(slw-4);
                if (fillW>0) GradH(dl,ImVec2(slx+2,sly+2),ImVec2(slx+2+fillW,sly+slh-2),dimC(ac,0.35f),dimC(ac,0.85f));
                float knobX = slx+2+fillW;
                dl->AddCircleFilled(ImVec2(knobX,sly+slh/2),5.5f,im(210,205,235));
                dl->AddCircle      (ImVec2(knobX,sly+slh/2),5.5f,withA(ac,140),12,1.2f);

                char spid[24]; snprintf(spid,sizeof(spid),"##sp%d",actualIdx);
                ImGui::SetCursorScreenPos(ImVec2(slx-4,sly-16));
                ImGui::InvisibleButton(spid,ImVec2(slw+8,slh+32));
                if (!app.showMidi && ImGui::IsItemActive()) {
                    float rel = (io.MousePos.x - slx - 2) / (slw - 4);
                    int nv = std::clamp((int)(rel*127),0,127);
                    if (nv != pval) {
                        pval = nv;
                        if (isCTModPage) {
                            // Mod matrix: send NRPN
                            int slot = app.synthPage - 9;
                            app.midi.sendNRPN(app.midiCh, CT_MOD_NRPN[slot][pi][0], CT_MOD_NRPN[slot][pi][1], pval);
                        } else {
                            int sendCC_num = isCTSy2 ? CT_SYNTH_CC[app.synthPage][pi]
                                           : isMF2   ? MF_CC[app.synthPage][pi]
                                                     : SYNTH_CC[actualIdx];
                            int sendCh = app.midiCh;
                            // FX page (page 6): slots 2-5 route to ch16 for global FX sends/master filter
                            if (isCTSy2 && app.synthPage == 6 && pi >= 2) {
                                sendCh = 15; // ch16 (0-indexed)
                                if (pi == 2) sendCC_num = (app.ctTrack == 1) ? 89 : 88;
                                if (pi == 3) sendCC_num = (app.ctTrack == 1) ? 112 : 111;
                            }
                            app.midi.sendCC(sendCh, sendCC_num, pval);
                        }
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    dl->AddRect(ImVec2(slx-1,sly-1),ImVec2(slx+slw+1,sly+slh+1),withA(ac,80),8);
                }
            }
        }
    }

    // ═══════════════════ PAD PROPERTIES POPUP ══════════════════════
    if (app.showPadEdit) {
        Track& ptr = app.tracks[app.selTrk];
        ImU32 ptc = TCLR[app.selTrk];

        float popW = 440, pRowH = 64;
        float titleH = 46, cbH = 50;
        bool isCTDrumPopup = (app.ctTrack >= 2);
        float popH = titleH + (isCTDrumPopup ? 5 : 4)*pRowH + 14 + cbH;
        float popX = W/2.f - popW/2.f;
        float popY = H/2.f - popH/2.f;
        if (popX < 8) popX = 8;
        if (popX + popW > W - 8) popX = W - popW - 8;
        if (popY < hdrH + 8) popY = (float)hdrH + 8;
        if (popY + popH > H - 8) popY = H - popH - 8;

        app.padEditPopupX = popX; app.padEditPopupY = popY;
        app.padEditPopupW = popW; app.padEditPopupH = popH;

        // Close on click outside
        if (ImGui::IsMouseClicked(0) &&
            !(io.MousePos.x >= popX && io.MousePos.x <= popX+popW &&
              io.MousePos.y >= popY && io.MousePos.y <= popY+popH)) {
            app.showPadEdit = false;
        } else {
            static const char* PP_NL[] = {"1/16","1/8","1/4","1/2","1/1"};
            int pnli=0; { int tmp=ptr.noteLen; while(tmp>1&&pnli<4){tmp>>=1;++pnli;} }
            char ppnstr[32]; snprintf(ppnstr,sizeof(ppnstr),"%s (%d)",noteName(ptr.note).c_str(),ptr.note);
            char ppvstr[8];  snprintf(ppvstr,sizeof(ppvstr),"%d",ptr.vel);
            char pppstr[12]; snprintf(pppstr,sizeof(pppstr),"%d",ptr.patLen);
            const char* ppLabels[4] = {"NOTE","VEL","STEPS","LENGTH"};
            const char* ppVals[4]   = {ppnstr,ppvstr,pppstr,PP_NL[pnli]};

            // Shadow + background
            dl->AddRectFilled(ImVec2(popX+5,popY+5),ImVec2(popX+popW+5,popY+popH+5),im(0,0,0,140),14);
            GradV(dl,ImVec2(popX,popY),ImVec2(popX+popW,popY+popH),im(32,30,48),im(22,20,36));
            dl->AddRect(ImVec2(popX,popY),ImVec2(popX+popW,popY+popH),withA(ptc,90),14,0,1.5f);
            dl->AddRectFilled(ImVec2(popX+2,popY+1),ImVec2(popX+popW-2,popY+2),withA(ptc,40),2);

            // Title stripe
            GradV(dl,ImVec2(popX,popY),ImVec2(popX+popW,popY+titleH),im(38,35,55),im(28,26,42));
            dl->AddRectFilled(ImVec2(popX,popY),ImVec2(popX+3,popY+titleH),ptc);
            dl->AddRectFilled(ImVec2(popX+2,popY+titleH-1),ImVec2(popX+popW-2,popY+titleH),withA(ptc,30),1);

            if (fBd) ImGui::PushFont(fBd);
            ImVec2 nms=ImGui::CalcTextSize(ptr.name.c_str());
            dl->AddText(ImVec2(popX+14,popY+titleH/2-nms.y/2),ptc,ptr.name.c_str());
            if (fBd) ImGui::PopFont();
            if (fSm) ImGui::PushFont(fSm);
            char ppnum[32]; snprintf(ppnum,sizeof(ppnum),"TRK %d  CH %d",app.selTrk+1,app.midiCh+1);
            ImVec2 ppns=ImGui::CalcTextSize(ppnum);
            dl->AddText(ImVec2(popX+popW-ppns.x-14,popY+titleH/2-ppns.y/2),im(100,95,130),ppnum);
            if (fSm) ImGui::PopFont();

            float pp3 = popY + titleH;
            float btnW2=62, btnH2=48;

            for (int i=0; i<4; ++i) {
                if (i%2==0)
                    dl->AddRectFilled(ImVec2(popX+4,pp3+i*pRowH),
                                      ImVec2(popX+popW-4,pp3+i*pRowH+pRowH-1),
                                      im(18,17,28,140),4);

                // Label
                if (fSm) ImGui::PushFont(fSm);
                ImVec2 lbs=ImGui::CalcTextSize(ppLabels[i]);
                dl->AddText(ImVec2(popX+14,pp3+i*pRowH+pRowH/2-lbs.y/2),im(90,85,120),ppLabels[i]);
                if (fSm) ImGui::PopFont();

                // Value — centered
                if (fBd) ImGui::PushFont(fBd);
                ImVec2 pvs2=ImGui::CalcTextSize(ppVals[i]);
                dl->AddText(ImVec2(popX+popW/2-pvs2.x/2,pp3+i*pRowH+pRowH/2-pvs2.y/2),
                            im(220,215,245),ppVals[i]);
                if (fBd) ImGui::PopFont();

                // < > buttons
                float lbx2=popX+popW-136, rbx2=popX+popW-68;
                float bby3=pp3+i*pRowH+(pRowH-btnH2)/2;
                bool lhov=ImGui::IsMouseHoveringRect(ImVec2(lbx2,bby3),ImVec2(lbx2+btnW2,bby3+btnH2),false);
                bool rhov=ImGui::IsMouseHoveringRect(ImVec2(rbx2,bby3),ImVec2(rbx2+btnW2,bby3+btnH2),false);

                dl->AddRectFilled(ImVec2(lbx2,bby3),ImVec2(lbx2+btnW2,bby3+btnH2),lhov?im(55,50,75):im(30,28,45),10);
                dl->AddRectFilled(ImVec2(rbx2,bby3),ImVec2(rbx2+btnW2,bby3+btnH2),rhov?im(55,50,75):im(30,28,45),10);
                if (lhov) dl->AddRect(ImVec2(lbx2,bby3),ImVec2(lbx2+btnW2,bby3+btnH2),withA(ptc,120),10);
                if (rhov) dl->AddRect(ImVec2(rbx2,bby3),ImVec2(rbx2+btnW2,bby3+btnH2),withA(ptc,120),10);

                if (fBd) ImGui::PushFont(fBd);
                ImVec2 lcs3=ImGui::CalcTextSize("<"), rcs3=ImGui::CalcTextSize(">");
                dl->AddText(ImVec2(lbx2+btnW2/2-lcs3.x/2,bby3+btnH2/2-lcs3.y/2),
                            lhov?im(230,225,255):im(140,135,170),"<");
                dl->AddText(ImVec2(rbx2+btnW2/2-rcs3.x/2,bby3+btnH2/2-rcs3.y/2),
                            rhov?im(230,225,255):im(140,135,170),">");
                if (fBd) ImGui::PopFont();

                if (lhov&&ImGui::IsMouseClicked(0)) {
                    switch(i) {
                        case 0: ptr.note=std::clamp(ptr.note-1,0,127);
                                app.midi.noteOn(app.midiCh,ptr.note,ptr.vel);
                                app.pendingOffs.push_back({app.midiCh,ptr.note,now+0.25}); break;
                        case 1: ptr.vel=std::clamp(ptr.vel-1,1,127); break;
                        case 2: ptr.patLen=std::clamp(ptr.patLen-1,1,app.numPages*NUM_STEPS); break;
                        case 3: { static int lens[]={1,2,4,8,16}; int idx=0; while(idx<4&&lens[idx]<ptr.noteLen)++idx; idx=std::max(0,idx-1); ptr.noteLen=lens[idx]; } break;
                    }
                }
                if (rhov&&ImGui::IsMouseClicked(0)) {
                    switch(i) {
                        case 0: ptr.note=std::clamp(ptr.note+1,0,127);
                                app.midi.noteOn(app.midiCh,ptr.note,ptr.vel);
                                app.pendingOffs.push_back({app.midiCh,ptr.note,now+0.25}); break;
                        case 1: ptr.vel=std::clamp(ptr.vel+1,1,127); break;
                        case 2: ptr.patLen=std::clamp(ptr.patLen+1,1,app.numPages*NUM_STEPS); break;
                        case 3: { static int lens[]={1,2,4,8,16}; int idx=0; while(idx<4&&lens[idx]<ptr.noteLen)++idx; idx=std::min(4,idx+1); ptr.noteLen=lens[idx]; } break;
                    }
                }
            }

            // PATCH row — CT drum only
            if (isCTDrumPopup) {
                int drumIdx2 = app.ctTrack - 2;
                int pi5 = 4;
                dl->AddRectFilled(ImVec2(popX+4,pp3+pi5*pRowH),
                                  ImVec2(popX+popW-4,pp3+pi5*pRowH+pRowH-1),im(18,17,28,140),4);
                char patchLbl[16]; snprintf(patchLbl,sizeof(patchLbl),"CC%d VAL",CT_DRUM_CC[drumIdx2][0]);
                if (fSm) ImGui::PushFont(fSm);
                ImVec2 plbl=ImGui::CalcTextSize(patchLbl);
                dl->AddText(ImVec2(popX+14,pp3+pi5*pRowH+pRowH/2-plbl.y/2),im(90,85,120),patchLbl);
                if (fSm) ImGui::PopFont();
                char pchstr[8]; snprintf(pchstr,sizeof(pchstr),"%d",ptr.synthP[0]);
                if (fBd) ImGui::PushFont(fBd);
                ImVec2 pvsp=ImGui::CalcTextSize(pchstr);
                dl->AddText(ImVec2(popX+popW/2-pvsp.x/2,pp3+pi5*pRowH+pRowH/2-pvsp.y/2),im(220,215,245),pchstr);
                if (fBd) ImGui::PopFont();
                float lbxP=popX+popW-136, rbxP=popX+popW-68;
                float bbyP=pp3+pi5*pRowH+(pRowH-btnH2)/2;
                bool lhovP=ImGui::IsMouseHoveringRect(ImVec2(lbxP,bbyP),ImVec2(lbxP+btnW2,bbyP+btnH2),false);
                bool rhovP=ImGui::IsMouseHoveringRect(ImVec2(rbxP,bbyP),ImVec2(rbxP+btnW2,bbyP+btnH2),false);
                dl->AddRectFilled(ImVec2(lbxP,bbyP),ImVec2(lbxP+btnW2,bbyP+btnH2),lhovP?im(55,50,75):im(30,28,45),10);
                dl->AddRectFilled(ImVec2(rbxP,bbyP),ImVec2(rbxP+btnW2,bbyP+btnH2),rhovP?im(55,50,75):im(30,28,45),10);
                if (lhovP) dl->AddRect(ImVec2(lbxP,bbyP),ImVec2(lbxP+btnW2,bbyP+btnH2),withA(ptc,120),10);
                if (rhovP) dl->AddRect(ImVec2(rbxP,bbyP),ImVec2(rbxP+btnW2,bbyP+btnH2),withA(ptc,120),10);
                if (fBd) ImGui::PushFont(fBd);
                ImVec2 lcP=ImGui::CalcTextSize("<"),rcP=ImGui::CalcTextSize(">");
                dl->AddText(ImVec2(lbxP+btnW2/2-lcP.x/2,bbyP+btnH2/2-lcP.y/2),lhovP?im(230,225,255):im(140,135,170),"<");
                dl->AddText(ImVec2(rbxP+btnW2/2-rcP.x/2,bbyP+btnH2/2-rcP.y/2),rhovP?im(230,225,255):im(140,135,170),">");
                if (fBd) ImGui::PopFont();
                if (lhovP&&ImGui::IsMouseClicked(0)) {
                    ptr.synthP[0]=std::clamp(ptr.synthP[0]-1,0,63);
                    app.midi.sendCC(9,CT_DRUM_CC[drumIdx2][0],ptr.synthP[0]);
                }
                if (rhovP&&ImGui::IsMouseClicked(0)) {
                    ptr.synthP[0]=std::clamp(ptr.synthP[0]+1,0,63);
                    app.midi.sendCC(9,CT_DRUM_CC[drumIdx2][0],ptr.synthP[0]);
                }
            }

            // Close button
            float cbY2=pp3+(isCTDrumPopup?5:4)*pRowH+12;
            float cbX2=popX+popW/2-80, cbW2=160;
            bool cchov=ImGui::IsMouseHoveringRect(ImVec2(cbX2,cbY2),ImVec2(cbX2+cbW2,cbY2+cbH),false);
            dl->AddRectFilled(ImVec2(cbX2,cbY2),ImVec2(cbX2+cbW2,cbY2+cbH),
                              cchov?im(80,70,110,240):im(45,42,64,200),10);
            dl->AddRect(ImVec2(cbX2,cbY2),ImVec2(cbX2+cbW2,cbY2+cbH),
                        withA(ptc,cchov?130u:60u),10,0,1.2f);
            if (fSm) ImGui::PushFont(fSm);
            ImVec2 pcls=ImGui::CalcTextSize("CLOSE");
            dl->AddText(ImVec2(cbX2+cbW2/2-pcls.x/2,cbY2+cbH/2-pcls.y/2),im(210,205,235),"CLOSE");
            if (fSm) ImGui::PopFont();
            if (cchov&&ImGui::IsMouseClicked(0)) app.showPadEdit=false;
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);

    // ═══════════════════ MIDI SETTINGS PANEL ════════════════════════
    if (app.showMidi) {
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::SetNextWindowSize(ImVec2(W,H));
        ImGui::SetNextWindowFocus();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::Begin("##midi_overlay", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNavFocus);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Full-screen dim overlay
        dl->AddRectFilled(ImVec2(0,0), ImVec2(W,H), im(0,0,0,210));

        const float pw = W - 40.f, ph = H - 40.f;
        const float px = 20.f,     py2 = 20.f;

        // Panel bg
        dl->AddRectFilled(ImVec2(px+5,py2+5), ImVec2(px+pw+5,py2+ph+5), im(0,0,0,80), 14);
        GradV(dl, ImVec2(px,py2), ImVec2(px+pw,py2+ph), im(22,20,34), im(14,13,22));
        dl->AddRect(ImVec2(px,py2), ImVec2(px+pw,py2+ph), im(55,50,85), 12, 0, 1.5f);
        dl->AddRectFilled(ImVec2(px+20,py2+1), ImVec2(px+pw-20,py2+2), im(90,80,150,80));

        // Header strip
        const float mhdrH = 54.f;
        GradV(dl, ImVec2(px,py2), ImVec2(px+pw,py2+mhdrH), im(28,26,40), im(20,18,32));
        dl->AddLine(ImVec2(px,py2+mhdrH), ImVec2(px+pw,py2+mhdrH), im(45,42,65));
        if (fLg) ImGui::PushFont(fLg);
        { ImVec2 s = ImGui::CalcTextSize("MIDI SETTINGS");
          dl->AddText(ImVec2(px+pw/2-s.x/2, py2+mhdrH/2-s.y/2), im(180,170,230), "MIDI SETTINGS"); }
        if (fLg) ImGui::PopFont();

        // CLOSE button — large, right of header
        {
            float cbw = 100, cbh = 38, cbx = px+pw-cbw-12, cby = py2+mhdrH/2-cbh/2;
            DrawPill(dl, cbx, cby, cbw, cbh, im(72,28,28), 10);
            dl->AddRect(ImVec2(cbx,cby), ImVec2(cbx+cbw,cby+cbh), im(130,48,48), 10);
            if (fSm) ImGui::PushFont(fSm);
            { ImVec2 s = ImGui::CalcTextSize("CLOSE");
              dl->AddText(ImVec2(cbx+cbw/2-s.x/2, cby+cbh/2-s.y/2), im(220,160,160), "CLOSE"); }
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(cbx,cby));
            ImGui::InvisibleButton("##mclose", ImVec2(cbw,cbh));
            if (ImGui::IsItemClicked()) app.showMidi = false;
            if (ImGui::IsItemHovered()) {
                DrawPill(dl, cbx, cby, cbw, cbh, im(140,45,45), 10);
                if (fSm) ImGui::PushFont(fSm);
                { ImVec2 s = ImGui::CalcTextSize("CLOSE");
                  dl->AddText(ImVec2(cbx+cbw/2-s.x/2, cby+cbh/2-s.y/2), im(255,210,210), "CLOSE"); }
                if (fSm) ImGui::PopFont();
            }
        }

        // Layout: port lists on top, options strip on bottom
        const float moptH    = 242.f;
        const float moptY    = py2 + ph - moptH;
        const float mlistTop = py2 + mhdrH + 6.f;
        const float mcolW    = (pw - 48.f) / 2.f;
        const float mox      = px + 16.f;
        const float mix      = mox + mcolW + 16.f;

        // Port column headers
        if (fSm) ImGui::PushFont(fSm);
        { ImVec2 s = ImGui::CalcTextSize("OUTPUT PORT");
          dl->AddText(ImVec2(mox+mcolW/2-s.x/2, mlistTop+5), im(80,150,220), "OUTPUT PORT"); }
        { ImVec2 s = ImGui::CalcTextSize("INPUT PORT");
          dl->AddText(ImVec2(mix+mcolW/2-s.x/2, mlistTop+5), im(70,200,130), "INPUT PORT"); }
        if (fSm) ImGui::PopFont();
        dl->AddLine(ImVec2(mox+mcolW+8, mlistTop), ImVec2(mox+mcolW+8, moptY-6), im(38,36,55));

        const float mrowH = 44.f;
        const float mlt   = mlistTop + 26.f;
        const int maxVis  = std::max(2, (int)((moptY - 6.f - mlt) / mrowH));

        const auto& op = app.midi.outPorts();
        const auto& ip = app.midi.inPorts();

        // OUT port list
        {
            struct R { std::string lbl; int idx; };
            std::vector<R> rows;
            rows.push_back({"None", -1});
            rows.push_back({"Virtual  (IAC / Loopback)", -2});
            for (int i = 0; i < (int)op.size(); ++i) rows.push_back({op[i], i});
            for (int r = 0; r < (int)rows.size() && r < maxVis; ++r) {
                float ry2  = mlt + r * mrowH;
                bool  sel2 = (rows[r].idx==-2 && app.midi.curOut()==-2 && app.midi.outOpen())
                          || (rows[r].idx>=0  && rows[r].idx==app.midi.curOut())
                          || (rows[r].idx==-1 && !app.midi.outOpen());
                dl->AddRectFilled(ImVec2(mox,ry2+1), ImVec2(mox+mcolW,ry2+mrowH-1),
                                  sel2 ? im(25,60,130) : im(18,17,28), 6);
                if (sel2) {
                    dl->AddRect(ImVec2(mox,ry2+1), ImVec2(mox+mcolW,ry2+mrowH-1), im(50,115,210), 6, 0, 1.5f);
                    dl->AddRectFilled(ImVec2(mox+1,ry2+2), ImVec2(mox+5,ry2+mrowH-2), im(65,150,235), 3);
                }
                if (fSm) ImGui::PushFont(fSm);
                ImGui::PushClipRect(ImVec2(mox+8,ry2+1), ImVec2(mox+mcolW-6,ry2+mrowH-1), true);
                dl->AddText(ImVec2(mox+12, ry2+mrowH/2-7), sel2 ? im(140,195,255) : im(120,115,155), rows[r].lbl.c_str());
                ImGui::PopClipRect();
                if (fSm) ImGui::PopFont();
                char oid[16]; snprintf(oid,sizeof(oid),"##mop%d",r);
                ImGui::SetCursorScreenPos(ImVec2(mox,ry2+1));
                ImGui::InvisibleButton(oid, ImVec2(mcolW,mrowH-2));
                if (ImGui::IsItemClicked()) {
                    if (rows[r].idx==-1)      app.midi.closeOut();
                    else if (rows[r].idx==-2) app.midi.openVirtual();
                    else                      app.midi.openOut(rows[r].idx);
                    // Reset CT track selector whenever output port changes
                    app.ctTrack = -1;
                    app.ctDrumPage = 0;
                    app.synthPage = 0;
                    // Restore default notes/channels
                    app.midiCh = 9;
                    for (int t = 0; t < NUM_TRACKS; ++t) {
                        app.tracks[t].ch   = 9;
                        app.tracks[t].note = TDEFS[t].note;
                    }
                }
                if (ImGui::IsItemHovered() && !sel2)
                    dl->AddRect(ImVec2(mox,ry2+1), ImVec2(mox+mcolW,ry2+mrowH-1), im(55,85,165,90), 6);
            }
        }

        // IN port list
        {
            struct R { std::string lbl; int idx; };
            std::vector<R> rows;
            rows.push_back({"None", -1});
            for (int i = 0; i < (int)ip.size(); ++i) rows.push_back({ip[i], i});
            for (int r = 0; r < (int)rows.size() && r < maxVis; ++r) {
                float ry2  = mlt + r * mrowH;
                bool  sel2 = (rows[r].idx>=0  && rows[r].idx==app.midi.curIn())
                          || (rows[r].idx==-1 && !app.midi.inOpen());
                dl->AddRectFilled(ImVec2(mix,ry2+1), ImVec2(mix+mcolW,ry2+mrowH-1),
                                  sel2 ? im(18,72,44) : im(18,17,28), 6);
                if (sel2) {
                    dl->AddRect(ImVec2(mix,ry2+1), ImVec2(mix+mcolW,ry2+mrowH-1), im(40,175,100), 6, 0, 1.5f);
                    dl->AddRectFilled(ImVec2(mix+1,ry2+2), ImVec2(mix+5,ry2+mrowH-2), im(55,195,115), 3);
                }
                if (fSm) ImGui::PushFont(fSm);
                ImGui::PushClipRect(ImVec2(mix+8,ry2+1), ImVec2(mix+mcolW-6,ry2+mrowH-1), true);
                dl->AddText(ImVec2(mix+12, ry2+mrowH/2-7), sel2 ? im(110,235,160) : im(120,115,155), rows[r].lbl.c_str());
                ImGui::PopClipRect();
                if (fSm) ImGui::PopFont();
                char iid[16]; snprintf(iid,sizeof(iid),"##mip%d",r);
                ImGui::SetCursorScreenPos(ImVec2(mix,ry2+1));
                ImGui::InvisibleButton(iid, ImVec2(mcolW,mrowH-2));
                if (ImGui::IsItemClicked()) {
                    if (rows[r].idx==-1) app.midi.closeIn();
                    else                 app.midi.openIn(rows[r].idx);
                }
                if (ImGui::IsItemHovered() && !sel2)
                    dl->AddRect(ImVec2(mix,ry2+1), ImVec2(mix+mcolW,ry2+mrowH-1), im(42,115,78,90), 6);
            }
        }

        // ── Options strip ──
        dl->AddLine(ImVec2(px+10,moptY), ImVec2(px+pw-10,moptY), im(38,36,55));
        const float moy    = moptY + 14.f;
        const float mlx    = mox;
        const float mrx    = mix;

        // Left column: CHANNEL selector + SWING slider + REFRESH button
        {
            // CHANNEL
            if (fTiny) ImGui::PushFont(fTiny);
            dl->AddText(ImVec2(mlx, moy+2), im(90,85,125), "OUTPUT CHANNEL");
            if (fTiny) ImGui::PopFont();

            const float chBtnW = 58, chBtnH = 52;
            const float chValW = 76;
            const float chBy   = moy + 18;

            // < button
            DrawPill(dl, mlx, chBy, chBtnW, chBtnH, im(32,30,52), 10);
            if (fBd) ImGui::PushFont(fBd);
            { ImVec2 s = ImGui::CalcTextSize("<");
              dl->AddText(ImVec2(mlx+chBtnW/2-s.x/2, chBy+chBtnH/2-s.y/2), im(160,155,205), "<"); }
            if (fBd) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(mlx, chBy));
            ImGui::InvisibleButton("##chdn", ImVec2(chBtnW, chBtnH));
            if (ImGui::IsItemClicked()) app.midiCh = std::clamp(app.midiCh-1,0,15);
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(mlx,chBy), ImVec2(mlx+chBtnW,chBy+chBtnH), im(100,95,155), 10);

            // value box
            float cvx = mlx + chBtnW + 6;
            dl->AddRectFilled(ImVec2(cvx,chBy), ImVec2(cvx+chValW,chBy+chBtnH), im(18,17,28), 8);
            dl->AddRect(ImVec2(cvx,chBy), ImVec2(cvx+chValW,chBy+chBtnH), im(55,50,82), 8, 0, 1.f);
            { char chv[4]; snprintf(chv, sizeof(chv), "%d", app.midiCh+1);
              if (fXl) ImGui::PushFont(fXl);
              ImVec2 cvs = ImGui::CalcTextSize(chv);
              dl->AddText(ImVec2(cvx+chValW/2-cvs.x/2, chBy+chBtnH/2-cvs.y/2), im(220,215,255), chv);
              if (fXl) ImGui::PopFont(); }

            // > button
            float crx = cvx + chValW + 6;
            DrawPill(dl, crx, chBy, chBtnW, chBtnH, im(32,30,52), 10);
            if (fBd) ImGui::PushFont(fBd);
            { ImVec2 s = ImGui::CalcTextSize(">");
              dl->AddText(ImVec2(crx+chBtnW/2-s.x/2, chBy+chBtnH/2-s.y/2), im(160,155,205), ">"); }
            if (fBd) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(crx, chBy));
            ImGui::InvisibleButton("##chup", ImVec2(chBtnW, chBtnH));
            if (ImGui::IsItemClicked()) app.midiCh = std::clamp(app.midiCh+1,0,15);
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(crx,chBy), ImVec2(crx+chBtnW,chBy+chBtnH), im(100,95,155), 10);

            // SWING
            float swY = chBy + chBtnH + 16;
            if (fTiny) ImGui::PushFont(fTiny);
            dl->AddText(ImVec2(mlx, swY+2), im(90,85,125), "SWING");
            if (fTiny) ImGui::PopFont();
            float slx = mlx, slw = mcolW - 72, slh = 30;
            float sly = swY + 18;
            dl->AddRectFilled(ImVec2(slx,sly), ImVec2(slx+slw,sly+slh), im(18,17,28), 8);
            dl->AddRect(ImVec2(slx,sly), ImVec2(slx+slw,sly+slh), im(40,38,60), 8);
            float filled = (float)app.swing / 50.f * (slw - 8);
            if (filled > 0)
                GradH(dl, ImVec2(slx+4,sly+4), ImVec2(slx+4+filled,sly+slh-4), im(45,55,150), im(75,95,210));
            float kx = slx + 4 + filled;
            dl->AddCircleFilled(ImVec2(kx, sly+slh/2), 12.f, im(200,195,230));
            dl->AddCircle(ImVec2(kx, sly+slh/2), 12.f, im(120,115,160), 16, 1.5f);
            if (fSm) ImGui::PushFont(fSm);
            { char swb[8]; snprintf(swb, sizeof(swb), "%d%%", app.swing);
              dl->AddText(ImVec2(slx+slw+12, sly+slh/2-7), im(175,170,205), swb); }
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(slx, sly-12));
            ImGui::InvisibleButton("##swing", ImVec2(slw, slh+24));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                float rel = (io.MousePos.x - slx - 4) / (slw - 8);
                app.swing = std::clamp((int)(rel * 50), 0, 50);
            }
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            // REFRESH PORTS button
            const float rfH = 44, rfW = 170;
            float rfy = py2 + ph - rfH - 14;
            DrawPill(dl, mlx, rfy, rfW, rfH, im(26,24,42), 10);
            dl->AddRect(ImVec2(mlx,rfy), ImVec2(mlx+rfW,rfy+rfH), im(50,46,78), 10);
            if (fSm) ImGui::PushFont(fSm);
            { ImVec2 s = ImGui::CalcTextSize("REFRESH PORTS");
              dl->AddText(ImVec2(mlx+rfW/2-s.x/2, rfy+rfH/2-s.y/2), im(148,144,190), "REFRESH PORTS"); }
            if (fSm) ImGui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(mlx, rfy));
            ImGui::InvisibleButton("##refresh", ImVec2(rfW, rfH));
            if (ImGui::IsItemClicked()) app.midi.refresh();
            if (ImGui::IsItemHovered()) dl->AddRect(ImVec2(mlx,rfy), ImVec2(mlx+rfW,rfy+rfH), im(80,75,120), 10);
        }

        // Right column: 3 large toggle buttons
        {
            const float togH = 52.f, togGap = 9.f;

            auto drawBigTog = [&](float tx, float ty, bool& val, const char* lbl) {
                ImU32 bg  = val ? im(28,105,45)  : im(22,20,36);
                ImU32 bdc = val ? im(45,185,75)  : im(48,45,72);
                DrawPill(dl, tx, ty, mcolW, togH, bg, 10);
                dl->AddRect(ImVec2(tx,ty), ImVec2(tx+mcolW,ty+togH), bdc, 10, 0, 1.2f);
                if (val) DrawGlow(dl, tx, ty, mcolW, togH, im(50,190,80), 2, 10);
                float cr = togH * 0.27f;
                dl->AddCircleFilled(ImVec2(tx+togH/2.f, ty+togH/2.f), cr, val ? im(80,255,120) : im(45,42,68));
                if (val) dl->AddCircle(ImVec2(tx+togH/2.f, ty+togH/2.f), cr, im(160,255,180), 16, 1.2f);
                if (fMd) ImGui::PushFont(fMd);
                ImVec2 ls = ImGui::CalcTextSize(lbl);
                dl->AddText(ImVec2(tx+togH+6, ty+togH/2-ls.y/2), val ? im(185,255,200) : im(120,115,155), lbl);
                if (fMd) ImGui::PopFont();
                char tid[48]; snprintf(tid,sizeof(tid),"##mbt%.32s",lbl);
                ImGui::SetCursorScreenPos(ImVec2(tx,ty));
                ImGui::InvisibleButton(tid, ImVec2(mcolW,togH));
                if (ImGui::IsItemClicked()) val = !val;
                if (ImGui::IsItemHovered() && !val)
                    dl->AddRect(ImVec2(tx,ty), ImVec2(tx+mcolW,ty+togH), im(72,68,112), 10);
            };

            drawBigTog(mrx, moy,                  app.midiThru, "MIDI Thru");
            drawBigTog(mrx, moy+togH+togGap,       app.clockOut, "Clock Out  (24ppq)");
            drawBigTog(mrx, moy+2*(togH+togGap),   app.syncIn,   "Sync In  (ext clock)");
        }

        ImGui::End();
        ImGui::PopStyleVar(3);
    }

    // ── Keyboard ─────────────────────────────────────────────────────
    if (ImGui::IsKeyPressed(ImGuiKey_Space) && !app.showMidi) {
        app.playing = !app.playing;
        if (app.playing) { app.curStep = 0; app.lastStep = get_time_sec(); if (app.clockOut) app.midi.sendRaw(0xFA); }
        else { if (app.clockOut) app.midi.sendRaw(0xFC); }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M)) {
        if (app.showMidi) app.showMidi = false;
        else { app.showMidi = true; app.showMidiOpenFrame = app.frameCount; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
        if (app.showMidi) app.showMidi = false;
        else { app.showMidi = true; app.showMidiOpenFrame = app.frameCount; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (app.showMidi) app.showMidi = false;
        else {
#ifndef __ANDROID__
            glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
#else
            g_App->destroyRequested = 1;
#endif
        }
    }
    if (!app.showMidi) {
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
            app.bpm = std::min(300.f, app.bpm + 1.f);
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
            app.bpm = std::max(20.f, app.bpm - 1.f);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) app.bpm = std::min(300.f, app.bpm + 5.f);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) app.bpm = std::max(20.f, app.bpm - 5.f);
    }
}

// ═══════════════════════════════════════════════════════════════════════

static void InitSharedApp(App& app) {
    srand((unsigned)time(nullptr));

    #ifndef __ANDROID__
    if (!g_synth.init()) {
        fprintf(stderr, "Warning: audio device init failed \u2014 synth disabled\n");
    }
#endif
    if (false) {
        fprintf(stderr, "Warning: audio device init failed — synth disabled\n");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0; st.FrameRounding = 6; st.GrabRounding = 4;
    st.WindowPadding = ImVec2(0,0);
    st.AntiAliasedLines = true; st.AntiAliasedFill = true;
    ImGui::StyleColorsDark();

    auto loadEmbedded = [&](const unsigned char* data, unsigned int size, float sz) -> ImFont* {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), (int)size, sz, &cfg);
    };
    fTiny = loadEmbedded(RobotoMono_Regular_data, RobotoMono_Regular_size, 13.f);
    fSm   = loadEmbedded(RobotoMono_Regular_data, RobotoMono_Regular_size, 15.f);
    fMd   = loadEmbedded(RobotoMono_Regular_data, RobotoMono_Regular_size, 17.f);
    fBd   = loadEmbedded(RobotoMono_Bold_data,    RobotoMono_Bold_size,    17.f);
    fLg   = loadEmbedded(RobotoMono_Bold_data,    RobotoMono_Bold_size,    23.f);
    fXl   = loadEmbedded(RobotoMono_Bold_data,    RobotoMono_Bold_size,    36.f);
    if (!fSm) fSm = io.Fonts->AddFontDefault();
    if (!fTiny) fTiny = fSm;
    io.Fonts->Build();

    for (int t = 0; t < NUM_TRACKS; ++t) {
        app.tracks[t].name = TDEFS[t].n;
        app.tracks[t].note = TDEFS[t].note;
        app.tracks[t].dtype = TDEFS[t].dtype;
        app.tracks[t].ch = 0;
        app.tracks[t].vel = 100;
        app.tracks[t].synthP[0] = t;
    }
    {
        auto sp = [&](int t, int p0, int p1, int p2, int p3, int p4, int p5,
                       int pe0, int pe1, int pe2, int pe3, int pe4, int pe5) {
            int* p = app.tracks[t].synthP;
            p[0]=p0; p[1]=p1; p[2]=p2; p[3]=p3; p[4]=p4; p[5]=p5;
            p[6]=pe0; p[7]=pe1; p[8]=pe2; p[9]=pe3; p[10]=pe4; p[11]=pe5;
        };
        sp( 0,   64,   75,  30,  40,  50,  64,   20,  64,   0,   0,  10,  30);
        sp( 1,   50,   55,  70,  75,  60,  15,    0,   0,  40,  30,  30,  45);
        sp( 2,   70,   15,  64,  85,  85,   5,    0,   0,  40,  55,   0,   0);
        sp( 3,   55,   85,  45,  60,  50, 115,    0,   0,  15,  15,   0,  18);
        sp( 4,   20,   45,  75,  55,  35,  85,   45,  35,  12,   0,  22,  12);
        sp( 5,  105,   48,  60,  58,  48,  22,   12,   0,  42,  22,   0,   0);
        sp( 6,   64,   55,  55,  52,  55,  16,    8,   0,  35,  16,   0,   6);
        sp( 7,   18,   68,  42,  45,  68,  10,    5,   0,  22,  10,   0,  12);
        sp( 8,   64,   50,  64,  30,  20,  40,    0,  30,  40,  30,  20,   0);
        sp( 9,   75,   35,  55,  95,  12,  85,   50,  45,  65,  28,   0,  25);
        sp(10,   64,   32,  25,  50,  22,  18,   22,  42,   0,  35,   0,  12);
        sp(11,   68,   72,  90,  65,  55,  28,   18,  25,  35,  45,  72,  20);
        sp(12,   45,   57,  72,  75,  38,  55,   65,  58,  42,  55,  85,  18);
        sp(13,   88,   50,  58,  54,  46,  20,   10,   0,  38,  20,   0,   0);
        sp(14,   38,   62,  48,  48,  62,  12,    6,   0,  28,  12,   0,   8);
        sp(15,   64,   50,  64,  55,  25,  50,   50,  42,   0,  10,  16,   0);
    }
    {
        auto hit = [&](int t, int s) { app.tracks[t].steps[s].on = true; };
        int kickPat = rand() % 6;
        switch (kickPat) {
            case 0: hit(0,0); hit(0,8);                         break;
            case 1: hit(0,0); hit(0,4); hit(0,8); hit(0,12);    break;
            case 2: hit(0,0); hit(0,6); hit(0,10);              break;
            case 3: hit(0,0); hit(0,10); hit(0,12);             break;
            case 4: hit(0,0); hit(0,4); hit(0,10);              break;
            case 5: hit(0,0); hit(0,3); hit(0,8); hit(0,11);   break;
        }
        int snarePat = rand() % 4;
        switch (snarePat) {
            case 0: hit(1,4); hit(1,12);                         break;
            case 1: hit(1,4); hit(1,10); hit(1,12);              break;
            case 2: hit(1,4); hit(1,12); hit(1,14);              break;
            case 3: hit(1,8);                                     break;
        }
        if (rand() % 2) {
            for (int s = 0; s < 16; s += 2) hit(2, s);
            if (rand() % 2) { int oh = (rand() % 4) * 4 + 2; hit(3, oh); }
        } else {
            for (int s = 0; s < 16; s += 4) hit(2, s);
            for (int i = 0; i < 1 + rand() % 2; ++i) {
                int s = (rand() % 8) * 2 + 1;
                hit(2, s % 16);
            }
        }
        if (rand() % 5 < 2) {
            int clapStep = (rand() % 2) ? 4 : 12;
            hit(8, clapStep);
        }
        app.bpm = 85.0f + (rand() % 51);
    }
}

#ifndef __ANDROID__
int main(int, char**) {
        if (!glfwInit()) { fprintf(stderr, "GLFW init failed\n"); return 1; }

#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, 1);
    const char* glsl_version = "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    const char* glsl_version = "#version 130";
#endif
    glfwWindowHint(GLFW_RESIZABLE, 0);

    GLFWwindow* window = glfwCreateWindow(WIN_W, WIN_H, "SELEKT", nullptr, nullptr);
    if (!window) { fprintf(stderr, "Window creation failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    App app;
    InitSharedApp(app);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        renderFrame(app);
        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.04f, 0.04f, 0.07f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    g_synth.shutdown();
    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(0, nullptr);
}
#endif
#endif // __ANDROID__


#ifdef __ANDROID__

#include <jni.h>
extern "C" JNIEXPORT jint JNICALL JNI_GetCreatedJavaVMs(JavaVM** vmBuf, jsize bufLen, jsize* nVMs) {
    if (g_App && g_App->activity && g_App->activity->vm) {
        if (bufLen > 0) {
            vmBuf[0] = g_App->activity->vm;
            if (nVMs) *nVMs = 1;
            return JNI_OK;
        }
    }
    if (nVMs) *nVMs = 0;
    return JNI_OK;
}
// We use a global for the app state on Android
static App* g_selektApp = nullptr;

static EGLDisplay           g_EglDisplay = EGL_NO_DISPLAY;
static EGLSurface           g_EglSurface = EGL_NO_SURFACE;
static EGLContext           g_EglContext = EGL_NO_CONTEXT;
static bool                 g_Initialized = false;
static char                 g_LogTag[] = "SelektApp";

static void MainLoopStep()
{
    ImGuiIO& io = ImGui::GetIO();
    if (g_EglDisplay == EGL_NO_DISPLAY || !g_selektApp)
        return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    renderFrame(*g_selektApp);

    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.04f, 0.04f, 0.07f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(g_EglDisplay, g_EglSurface);
}

static void Shutdown()
{
    if (!g_Initialized)
        return;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();
    g_synth.shutdown();
    if (g_selektApp) {
        delete g_selektApp;
        g_selektApp = nullptr;
    }

    if (g_EglDisplay != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(g_EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (g_EglContext != EGL_NO_CONTEXT)
            eglDestroyContext(g_EglDisplay, g_EglContext);

        if (g_EglSurface != EGL_NO_SURFACE)
            eglDestroySurface(g_EglDisplay, g_EglSurface);

        eglTerminate(g_EglDisplay);
    }

    g_EglDisplay = EGL_NO_DISPLAY;
    g_EglContext = EGL_NO_CONTEXT;
    g_EglSurface = EGL_NO_SURFACE;
    ANativeWindow_release(g_App->window);

    g_Initialized = false;
}

static void Init(struct android_app* app)
{
    if (g_Initialized)
        return;

    g_App = app;
    ANativeWindow_acquire(g_App->window);

    {
        g_EglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_EglDisplay == EGL_NO_DISPLAY)
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglGetDisplay(EGL_DEFAULT_DISPLAY) returned EGL_NO_DISPLAY");

        if (eglInitialize(g_EglDisplay, 0, 0) != EGL_TRUE)
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglInitialize() returned with an error");

        const EGLint egl_attributes[] = {
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,

            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_NONE
        };
        EGLint num_configs = 0;
        if (eglChooseConfig(g_EglDisplay, egl_attributes, nullptr, 0, &num_configs) != EGL_TRUE)
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglChooseConfig() returned with an error");
        if (num_configs == 0)
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglChooseConfig() returned 0 matching config");

        EGLConfig egl_config;
        eglChooseConfig(g_EglDisplay, egl_attributes, &egl_config, 1, &num_configs);
        EGLint egl_format;
        eglGetConfigAttrib(g_EglDisplay, egl_config, EGL_NATIVE_VISUAL_ID, &egl_format);
        ANativeWindow_setBuffersGeometry(g_App->window, 0, 0, egl_format);

        const EGLint egl_context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        g_EglContext = eglCreateContext(g_EglDisplay, egl_config, EGL_NO_CONTEXT, egl_context_attributes);

        if (g_EglContext == EGL_NO_CONTEXT)
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglCreateContext() returned EGL_NO_CONTEXT");

        g_EglSurface = eglCreateWindowSurface(g_EglDisplay, egl_config, g_App->window, nullptr);
        eglMakeCurrent(g_EglDisplay, g_EglSurface, g_EglSurface, g_EglContext);
    }

    g_selektApp = new App();
    InitSharedApp(*g_selektApp);

    ImGui_ImplAndroid_Init(g_App->window);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Setup scaling based on DPI/density can be done here.
    float main_scale = 1.0f; // Could adjust this later if needed
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);


    g_Initialized = true;
}

static int32_t handleInputEvent(struct android_app* app, AInputEvent* inputEvent)
{
    if (!g_Initialized) return 0;
    return ImGui_ImplAndroid_HandleInputEvent(inputEvent);
}

static void handleAppCmd(struct android_app* app, int32_t appCmd)
{
    switch (appCmd)
    {
    case APP_CMD_SAVE_STATE:
        break;
    case APP_CMD_INIT_WINDOW:
        Init(app);
        break;
    case APP_CMD_TERM_WINDOW:
        Shutdown();
        break;
    case APP_CMD_GAINED_FOCUS:
    case APP_CMD_LOST_FOCUS:
        break;
    }
}

extern "C" JNIEXPORT void JNICALL android_main(struct android_app* app)
{
    g_App = app;
    app->onAppCmd = handleAppCmd;
    app->onInputEvent = handleInputEvent;

    while (true)
    {
        int out_events;
        struct android_poll_source* out_data;
        while (ALooper_pollOnce(g_Initialized ? 0 : -1, nullptr, &out_events, (void**)&out_data) >= 0)
        {
            if (out_data != nullptr)
                out_data->process(app, out_data);
            if (app->destroyRequested != 0)
            {
                if (!g_Initialized) Shutdown();
                return;
            }
        }
        MainLoopStep();
    }
}
#endif
