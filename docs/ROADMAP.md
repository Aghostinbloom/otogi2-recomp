# Roadmap

One milestone at a time. Each ends with an exit check that a person can run.

## Decisions

- **Host in C++20, built with CMake and clang-cl.** The lifted C is most of the CPU time, so the compiler matters more than the host language; clang-cl gives clang's optimiser with PDBs for the Visual Studio debugger. MSVC stays available as a second compiler.
- **Graphics by intercepting Direct3D 8 calls,** redrawn in OpenGL ES 3 through ANGLE (Direct3D 11, Vulkan or OpenGL underneath). This is the approach proven on the first game's engine, and it is far cheaper than replaying NV2A pushbuffers.
- **One host for the Otogi engine, one address profile per game.** The two games share an engine; a fix found in one should apply to the other by changing addresses, not code.
- **xboxrecomp as a submodule,** pinned to a release tag, updated on purpose.

## M0: Analysis (done)

Toolchain, extraction, XBE analysis, kernel coverage, comparison with the first game. Exit: [NOTES.md](NOTES.md) documents the XBE, and the first game's fixes are located or listed as open.

## M1: Lift (done)

`func_id`, `abi_analysis` and `recomp --all` over Otogi 2; the generated C compiles and links against the toolkit with a stub host. Untranslated instructions are classified by the function that contains them. Exit: the exe links. It also boots as far as D3D initialisation.

## M2: Boot (done)

Port the boot fixes through the Otogi 2 profile: DVD media check, asynchronous reads, D3D fences, vblank, XAPI input, the movie step, audio readiness. Exit: the title reaches the first stage under a scripted pad, with no crash for a minute.

Done: D3D fences, vblank, audio readiness, asynchronous reads, heap reclaim, XAPI input, the movie step (movies are skipped until M4). With the renderer, a pad drives New Game, the settings pages, save creation, the story intro and the in-engine cutscene (`map\m050`) into the first stage (`map\m000`, Seimei), where the game runs and its pause menu works. The DVD media check has not come up.

## M3: Renderer (in progress)

D3D8 interception on OpenGL ES 3: state mirroring, draws, register combiners, render targets, recorded pushbuffers. Exit: menus and the first stage draw correctly.

Done: the renderer, ANGLE and the window; the game's D3D calls and the 2D it writes into the pushbuffer itself reach it; NV2A register-combiner pixel shaders; CopyRects on the host. The logos and menus draw as on the console. The stage's 3D scene renders, with depth stored and read back as on the console and the post-processing passes in their own targets. Next: the rest of the post-processing (dependent texture reads, a pass that greys the stage), then the cutscene's darkness and untextured characters.

## M4: Playable

Pacing at the original 30 fps, controller, audio, saves, movies. Exit: a stage played through start to finish.

Started: live sensors (frame times, hitches, freezes, audio underruns) and `scripts/bench.ps1`, which measures a run into the first stage. Audio now plays at full rate (the host asks Windows for 1 ms timers). The game runs at its own 30 frames a second (presentation interval two; it counts time in frames, so at 60 it ran twice as fast), and the stage's frames come on time. Next: the hitch when a stage starts (programs compiled ahead or cached).

## M5: Enhancements

Internal resolution, 16:9 with the HUD kept in place, frame interpolation above 30 fps, settings file, and a setup tool that builds the game from the player's disc.
