# Notes

How things work and what was found, one section per area. Addresses are guest virtual addresses in the USA `default.xbe`.

## Toolchain

- Visual Studio 2022 Build Tools (MSVC 14.44, Windows SDK 10.0.26100), with the CMake 3.31 and Ninja 1.12 it ships. They are not on `PATH`: configure from a `vcvarsall.bat x64` environment and pass `-G Ninja`.
- xboxrecomp v0.18.0 builds with MSVC x64 and Ninja with no patches; its C regression tests (`ctest`) pass.
- Python: repo-local `.venv` with capstone 5.0.7.

## Game data

- USA disc: 2,527 files, 3,023,531,374 bytes. Two XMV movies, 892 DDS textures, about 2 GB of `.dat`.
- `default.xbe`: 2,314,240 bytes, SHA-1 `314d00c38881e92c9a5cf4e1063336e0ec3a1003`.
- extract-xiso patches every `.xbe` when it builds an `.xiso` (the jump after a media check, `E8 CA FD FF FF 85 C0 7D` becomes `... EB`). Otogi 2's XBE does not contain that sequence, so its hash is the same from a patched or an unpatched image. The first game's does, at file offset `0xD594E`: its reference SHA-1 `abf12bb66fcbd9f694c223419a2309c1a5f72293` is the unpatched file.

## XBE

| | Otogi 2 (USA) |
|---|---|
| Title ID | `0x46530004` |
| Base, entry point | `0x00010000`, `0x001641F7` |
| Kernel thunk table | `0x001F58E0`, 113 imports |
| Image size | `0x00AFCFA0` |
| XDK | 5849 for all nine libraries (XAPILIB, D3D8LTCG, D3DX8, XGRAPHC, DSOUND, XMV, XBOXKRNL, LIBCMT, LIBCPMT) |

- Sections: `.text`, `D3DX`, `XGRPH`, `DSOUND`, `XMV`, `D3D`, `XPP`, `.rdata`, `.data`, `DOLBY`, `$$XTIMAGE`, `.XTLID`.
- `tools.kernel_audit.coverage`: all 113 imports are routed by the toolkit; none is left to the generic stub.
- `tools.disasm`, every code section (no `--text-only`: the XDK sections are code the game calls): 7,939 functions. `.text` 6,471, DSOUND 510, D3DX 428, D3D 225, XPP 197, XMV 72, XGRPH 36. `DOLBY` has no x86 functions; it is most likely the audio DSP's image.
- Most called kernel imports: `RtlLeaveCriticalSection` (139 call sites), `KfLowerIrql`, `NtClose`, `KeRaiseIrqlToDpcLevel`, `KeCancelTimer`.

## Lift (M1)

- Pipeline, from `xboxrecomp/` with `..\.venv\Scripts\python -m tools.<step>`, outputs under `analysis/otogi2/`:
  `func_id ..\input\default.xbe --functions <disasm>\functions.json --strings <disasm>\strings.json --xrefs <disasm>\xrefs.json -o <func_id>`,
  `abi_analysis ..\input\default.xbe --disasm-dir <disasm> --func-id-dir <func_id> --output-dir <abi>`,
  `recomp ..\input\default.xbe --all --split 250 --game-name "Otogi 2" --gen-dir ..\src\recomp\gen --disasm-dir <disasm> --func-id-dir <func_id> --abi-dir <abi>`.
- func_id: 9,532 entries (the disassembler's 7,939 plus vtable slots); 4,609 classified, 1,791 of them vtable methods. abi_analysis: 7,939 entries, 533 thiscall.
- recomp: 7,943/7,943 translated, 0 failed, about 1.08 million lines in 32 files (45 MB), 13 s.
- 11 untranslated instructions (7 mnemonics), each emitted as `recomp_unimpl()` so a hit is logged:
  - `sgdt` at `0x0016846C` and a far jump at `0x0016847D`: the GDT write that the first game overrides (`sub_000E88E4` there). Needs an override in M2 if reached.
  - `aaa`/`aas`/`daa` around `0x001A8C95`, in XMV: most likely data lifted as code.
  - `cli`/`sti` at `0x00165AB5`: harmless as no-ops.
- Build: `scripts\build.ps1` (clang-cl 23, Ninja, RelWithDebInfo with embedded debug info). The whole tree, toolkit included, builds in about 30 s; the exe is 55 MB. The toolkit builds under clang-cl with no patches.

## First boot

`build\otogi2.exe <game folder>` with no overrides (`RECOMP_WATCHDOG_SECS=10`):
- The XBE maps (12 sections, 113/113 thunks bridged), the main thread starts, the CRT initialises, and the save partition mounts (`TDATA`/`UDATA\46530004`).
- About 200 kernel calls, all successful. D3D initialisation starts: 7.7 MB of contiguous memory (pushbuffer), GPU interrupt connected (vector 3, routine `0x001E7C00`).
- Then it hangs in D3D: the last indirect calls are in `0x001DCB30`-`0x001DD300` and the NV2A pushbuffer is idle (`PUT == GET`). This is where the first game needed its D3D fence fix (`D3D_BlockOnTime`) and the pushbuffer kick override.

## Comparison with the first game

The first *Otogi* (USA, title ID `0x46530002`) runs natively in [Otogi XUnchain](https://github.com/Guuzzeji/otogi-xunchain). Both XBEs were analysed the same way and compared with `scripts/xbe_compare.py`, which matches functions on their instructions with addresses masked, then along the call graph from the pairs already found.

- Kernel imports: Otogi 2's 113 are all imported by Otogi 1 as well. Otogi 1 has two more, `XcVerifyPKCS1Signature` (#344) and `XePublicKeyData` (#355).
- XDK: Otogi 1 is 5455 throughout and links plain `D3D8`; Otogi 2 is 5849 and links `D3D8LTCG`.
- Otogi 1 functions found in Otogi 2:

| Section | Found | |
|---|---|---|
| XGRPH, XMV | 100% | |
| D3DX, XPP | 97% | |
| DSOUND | 81% | |
| D3D | 24% | LTCG reorganised the library |
| `.text` | 22% (1,022) | Most game code was rebuilt or rewritten; these are anchors |

- Fixes the first port applies, located in Otogi 2 with confidence (exact match, call graph, or a fixed offset from a matched neighbour):

| Function | Otogi 1 | Otogi 2 |
|---|---|---|
| XInitDevices | `0x0016E924` | `0x001F0704` |
| XGetDevices | `0x0016E929` | `0x001F0709` |
| XGetDeviceChanges | `0x0016E94B` | `0x001F072B` |
| XInputOpen | `0x0016EA1A` | `0x001F0402` |
| XInputClose | `0x0016EA70` | `0x001F0458` |
| XInputGetCapabilities | `0x0016EA7C` | `0x001F0464` |
| XInputGetState | `0x0016EC54` | `0x001F063C` |
| XInputSetState | `0x0016ECC7` | `0x001F06AF` |
| D3DXMatrixPerspectiveFovLH (widescreen) | `0x000E983E` | `0x00169585` |
| D3DDevice_CreateVertexShader | `0x000F6EC0` | `0x001E0370` |
| D3DDevice_SetVertexShaderConstant1 | `0x000F7020` | `0x001E0750` |
| D3DDevice_SetVertexShaderConstant4 | `0x000F7080` | `0x001E07B0` |
| D3DDevice_SetTexture | `0x000F8590` | `0x001DF260` |
| Lock2DSurface | `0x000FE580` | `0x001E5F00` |
| Sphere-in-frustum test | `0x000167B0` | `0x00051950` (0.99 similar) |

- Still to locate: most of the D3D device (Swap, Clear, SetRenderTarget, CopyRects, the draw calls, the pushbuffer functions, BlockOnTime). Instruction similarity is unreliable across 5455 and 5849 LTCG; an XDK signature database (XbSymbolDatabase) is the right tool. `BeginPush` and `EndPush` have no counterpart and may have been inlined.
- Game-specific fixes to find by behaviour: the DVD media check (XAPI 5849 differs), the XMV per-frame step, the overlapped stream loader and the sound-stream worker.
