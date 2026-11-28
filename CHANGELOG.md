# Changelog

All notable changes to this project are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Analysis of the USA `default.xbe`: sections, XDK libraries, kernel imports and functions.
- Comparison with the first *Otogi*: shared kernel imports, shared library code, and where the first game's fixes land in this one (`scripts/xbe_compare.py`).
- A check that no game file or personal detail is ever committed (`scripts/check_no_game_data.py`, run on every push).
- The first native build: the whole game lifts to C and links into `otogi2.exe` (C++20 host, CMake, clang-cl), which boots as far as Direct3D initialisation.
- `scripts/build.ps1`: one-command build from a Visual Studio x64 environment.
- `scripts/lift.ps1`: the whole lift pipeline in one command.
- Boots through the logos and title screen into the main menu: D3D fence and XMV movie overrides, XAPI pad input from the host pad, keyboard or a scripted pad, and the runtime options the game needs on by default. Movies are skipped for now.
- A game window and the D3D8 renderer (OpenGL ES 3 on ANGLE, from the Sneak King recomp), fed by the game's own D3D calls and by the 2D it writes into the pushbuffer itself. Pixel shaders (NV2A register combiners) and back-buffer copies run on the host, so the logos and menus draw as on the console.
- A new game reaches the first stage: settings, save creation, story intro and cutscene, then the stage runs and its pause menu works. The stage's 3D does not draw yet.
- `OTOGI_STACKS_EVERY`: periodic per-thread stacks named from the symbols, for diagnosing hangs.
- `RECOMP_GL_CAPTURE`: a frame capture on demand, draw by draw, with every render-target pass written as a BMP and diagnostic views of the 3D.
- The stage's 3D scene now renders into the back buffer: vertex shader constants written straight into the pushbuffer reach the renderer, a stage's few hundred vertex programs fit, a recreated program handle gets its new microcode, a draw that samples its own target reads a copy, and vertex-program fog follows the NV2A. The post-processing that follows still hides it.
- Depth as the console stores it: w-buffered draws store w, depth buffers read as textures give their bytes, and a colour target bound as a depth buffer loads from it. Passes that set their target in the pushbuffer draw into it rather than into the back buffer, which removes the rectangle at the top left of the stage's cutscene.
- Live sensors (`RECOMP_PERF`, on by default): frame times every second, each frame over 50 ms with what it did, freezes reported while they last with every thread's stack, audio underruns and how much of real time the device played; the window title shows the main numbers.
- Audio no longer stutters: the host asks Windows for 1 ms timers, without which the emulated audio processor overslept and produced about 40% of real time. The device now plays 100% of real time with no underruns.
- The game runs at its own speed: 30 frames a second, the presentation interval it asks Direct3D for, where it ran at about 58 and so twice as fast. Frames in the first stage now come on time: 6 slow frames in a minute instead of several hundred. `RECOMP_PRESENT_INTERVAL` overrides the interval.
- The live sensors count every slow frame against the presentation interval and say where a late frame's time went, including the main thread's CPU time; `OTOGI_LATE_STACKS` and `OTOGI_GAME_STACKS` take stacks during slow frames.
- `scripts/bench.ps1`: a repeatable run into the first stage that writes a report on frame times, hitches, freezes and audio, with `scripts/audio_report.py` on the recorded output (stereo or mono, dropouts, clicks, clipping).
