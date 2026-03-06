# SELEKT

A GPU-accelerated drum machine and step sequencer with a built-in synthesizer engine, designed for touch and mouse. Runs standalone on macOS, Windows, and Linux — no DAW required.

![SELEKT UI](docs/screenshot.png)

---

## Features

### Sequencer
- **16 tracks** × up to **128 steps** (1–8 pages of 16 steps each)
- Per-step **velocity**, **probability** (0–100%), and **ratchet** (1–4 subdivisions)
- Per-track **pattern length**, **note length**, and **MIDI channel**
- **Beat Repeat** — loops the current 4-step group while held
- **Mutate** — randomises velocities and step toggles for instant variation
- **Swing** — global swing amount applied to every other step
- MIDI **Save/Load** — exports and imports standard `.mid` files

### Built-in Drum Synth
- 11 drum voice types: Kick, Snare, Hi-Hat, Tom, Clap, Perc, Cymbal, Shaker, Cowbell, Ride, Crash
- **24 parameters per track** across 4 pages:
  - **SYNTH** — core synthesis (pitch, decay, drive, per-type timbres)
  - **EXT** — extended per-type controls (ring modulation, FM, harmonics, grain)
  - **FX** — shaping chain (filter, resonance, compression, saturation, bit crush, level)
  - **FX2** — spatial effects (reverb + size, delay + time, chorus + speed)
- Synth engine auto-disables when a real MIDI output is selected

### MIDI
- Select any system MIDI output — synth params display as **CC numbers** (CC70–CC93) when a hardware device is active
- **Virtual port** (macOS/Linux IAC loopback) for routing to DAWs without hardware
- MIDI input support — receive clock and control
- **Clock output** — sends MIDI Start/Stop/Continue and 24 PPQN clock
- Configurable **output channel**
- **Octave shift** (OCT− / OCT+) when a MIDI device is selected — shifts all 16 track notes by ±12 semitones

### Interface
- 1280×800 touch-friendly UI built with Dear ImGui + OpenGL
- 16 colour-coded **pad buttons** — tap to trigger, hold to edit track properties (note, velocity, steps, note length)
- Per-step popup editor — adjust velocity, probability, and ratchet inline
- Page navigation for patterns up to 128 steps

---

## Building

### macOS
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
open build/selekt.app
```

### Windows (cross-compile from macOS/Linux via MinGW)
```bash
cmake -B build-win64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win64 --parallel
```
Requires `x86_64-w64-mingw32-gcc` (e.g. `brew install mingw-w64`).

### Linux (via Docker)
```bash
docker build -f Dockerfile.linux -t selekt-linux .
docker run --rm -v "$PWD/build-linux64:/out" selekt-linux sh -c "cp /build/selekt /out/selekt"
```

---

## Dependencies

All fetched automatically via CMake FetchContent at configure time:

| Library | Purpose |
|---------|---------|
| [Dear ImGui](https://github.com/ocornut/imgui) | GPU-accelerated UI |
| [GLFW](https://github.com/glfw/glfw) | Window/input |
| [RtMidi](https://github.com/thestk/rtmidi) | MIDI I/O |
| [miniaudio](https://github.com/mackron/miniaudio) | Audio output (header-only) |

---

## Requirements

- **macOS**: 10.15+ (Apple Silicon & Intel)
- **Windows**: 10+ x64
- **Linux**: Any distro with ALSA (`libasound2`) and OpenGL

---

## License

MIT
