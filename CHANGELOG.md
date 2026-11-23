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
