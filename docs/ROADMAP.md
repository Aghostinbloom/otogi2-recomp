# Roadmap

One milestone at a time. Each ends with an exit check that a person can run.

## Decisions

- **Host in C++20, built with CMake and clang-cl.** The lifted C is most of the CPU time, so the compiler matters more than the host language; clang-cl gives clang's optimiser with PDBs for the Visual Studio debugger. MSVC stays available as a second compiler.
- **Graphics by intercepting Direct3D 8 calls,** redrawn in OpenGL ES 3 through ANGLE (Direct3D 11, Vulkan or OpenGL underneath). This is the approach proven on the first game's engine, and it is far cheaper than replaying NV2A pushbuffers.
- **One host for the Otogi engine, one address profile per game.** The two games share an engine; a fix found in one should apply to the other by changing addresses, not code.
- **xboxrecomp as a submodule,** pinned to a release tag, updated on purpose.

## M0: Analysis (done)

Toolchain, extraction, XBE analysis, kernel coverage, comparison with the first game. Exit: [NOTES.md](NOTES.md) documents the XBE, and the first game's fixes are located or listed as open.

## M1: Lift

`func_id`, `abi_analysis` and `recomp --all` over Otogi 2; the generated C compiles and links against the toolkit with a stub host. Untranslated instructions are classified by the function that contains them. Exit: the exe links.

## M2: Boot

Port the boot fixes through the Otogi 2 profile: DVD media check, asynchronous reads, D3D fences, vblank, XAPI input, the movie step, audio readiness. Exit: the title reaches the first stage under a scripted pad, with no crash for a minute.

## M3: Renderer

D3D8 interception on OpenGL ES 3: state mirroring, draws, register combiners, render targets, recorded pushbuffers. Exit: menus and the first stage draw correctly.

## M4: Playable

Pacing at the original 30 fps, controller, audio, saves, movies. Exit: a stage played through start to finish.

## M5: Enhancements

Internal resolution, 16:9 with the HUD kept in place, frame interpolation above 30 fps, settings file, and a setup tool that builds the game from the player's disc.
