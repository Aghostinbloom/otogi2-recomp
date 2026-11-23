# otogi2-recomp

A native Windows port of *Otogi 2: Immortal Warriors* (FromSoftware, 2004, original Xbox), made by static recompilation. The game's `default.xbe` is translated from x86 to C function by function and compiled for the PC, with a small model of the Xbox around it. No emulator.

> **Status: pre-alpha.** The executable has been analysed and compared with the first game; nothing boots yet. See [docs/ROADMAP.md](docs/ROADMAP.md).

**This repository contains no game code or data.** You build the game from a dump of your own disc. Nothing from the disc, and nothing generated from it (lifted C, disassembly, extracted assets), is ever committed; a check enforces it on every push.

## Plan

- Native Windows exe, no emulator
- Higher internal resolution, 16:9 widescreen, frame rates above the original 30
- Modern controller handling and keyboard support

## How it works

1. **Lift.** The [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit (a submodule in `xboxrecomp/`) finds the game's ~7,900 functions and turns each into C.
2. **Runtime.** The toolkit's libraries stand in for the Xbox kernel, audio processor and USB pads. All 113 kernel imports of Otogi 2 are already routed by the toolkit.
3. **Graphics.** The game's Direct3D 8 calls are intercepted and redrawn through OpenGL ES 3 ([ANGLE](https://chromium.googlesource.com/angle/angle) on Direct3D 11, Vulkan or OpenGL), following the approach proven on the first game.
4. **Host.** C++20, built with CMake and clang-cl. The host is written for the Otogi engine, with one address profile per game, so fixes found for one game carry over to the other.

[docs/NOTES.md](docs/NOTES.md) has the findings so far.

## Requirements

- Windows 10 or 11, 64-bit
- A disc image of your own **Otogi 2: Immortal Warriors, USA** disc
- Visual Studio 2022 Build Tools (MSVC, CMake, Ninja), LLVM (clang-cl), Python 3.10+ with `capstone`

## Analysis

```
git clone --recurse-submodules https://github.com/Aghostinbloom/otogi2-recomp
cd otogi2-recomp
python -m venv .venv
.venv\Scripts\pip install capstone
# put default.xbe from your disc in input\
cd xboxrecomp
..\.venv\Scripts\python -m tools.xbe_parser ..\input\default.xbe --json ..\input\default_analysis.json
..\.venv\Scripts\python -m tools.disasm ..\input\default.xbe -o ..\analysis\otogi2\disasm -v
```

`scripts/xbe_compare.py` compares two analysed XBEs and maps functions from one to the other; it is how the first game's fixes are located in this one.

## Contributing

Pull requests are not accepted at the moment. Bug reports and findings are welcome as issues; see [CONTRIBUTING.md](CONTRIBUTING.md).

## Thank you

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and its [contributors](https://github.com/sp00nznet/xboxrecomp/blob/main/CONTRIBUTORS.md): XBE parsing, the x86-to-C lifter and the Xbox runtime.
- [Otogi XUnchain](https://github.com/Guuzzeji/otogi-xunchain), the recomp of the first *Otogi*: the reference for this engine's boot, rendering and input fixes.
- [Sneak King recomp](https://github.com/Guuzzeji/sneaky-whopper-recomp): the D3D8-to-OpenGL ES layer this port builds on.
- [extract-xiso](https://github.com/XboxDev/extract-xiso): disc image extraction.

## License

The project's own code and docs are under the [MIT License](LICENSE). The `xboxrecomp/` submodule and other third-party components keep their own licenses, and the game is not covered; see [NOTICE](NOTICE).

---

*Otogi 2: Immortal Warriors is a trademark of FromSoftware, Inc. This is an unofficial fan preservation project, not affiliated with FromSoftware or Microsoft. You must own the game.*
