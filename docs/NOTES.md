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

## Boot (M2)

- `scripts\lift.ps1` runs the whole pipeline; `-RecompOnly` re-runs only the lifter after the overrides change. Overrides live in `src/games/otogi2/overrides.cpp`, which `--exclude-manual` reads directly: a line starting `void sub_X(void)` is a replacement, `extern void sub_X_gen(void);` a wrapper.
- Runtime options on by default (`kOtogi2RuntimeDefaults`; the environment still wins): `RECOMP_ASYNC_IO`, `RECOMP_VBLANK`, `RECOMP_AC97_READY`, `RECOMP_HEAP_RECLAIM`, as in the first game.
- Fixes, in boot order:
  1. Without overrides the title hangs in D3D initialisation, in `sub_001E18E0` (`D3D_BlockOnTime`, loop at `0x001E1A50`): it waits for the GPU semaphore `*(device+0x30)` to reach the fence `device+0x2C`, and nothing executes the pushbuffer. Replaced: the semaphore points at the fence counter, so every fence reads as done. Device pointer `0x001EE1F8`. Same fix as the first game.
  2. The intro movie (`movie\OTOGI_h_loop_us.xmv`) then waits in DirectSound forever: the XMV clock only starts once its audio pipeline runs. `sub_00135AB0` is the per-frame movie step (plain `ret`, 1 = ended), called only by `sub_0013F570` (play a movie, skippable by any button; `sub_00135710` opens, `sub_00135DF0` closes). Wrapped: every movie ends at once unless `RECOMP_MOVIES=1`.
  3. With movies ending at once, the attract loop opens the movie every few hundred milliseconds, and each one allocates 1.8 MB of contiguous memory (640x480 YUY2 and ARGB). The title frees it on close, but the runtime only reuses freed contiguous memory under `RECOMP_HEAP_RECLAIM`; without it the 64 MB window was full in 20 s. Now stable at about 25 MB.
  4. Input: XAPI's USB stack never enumerates a device on the runtime's OHCI model (`RECOMP_USB=1`, which needs the host to call `xbox_OhciInit`: interrupts arrive, no control transfer follows). The eight XAPI entry points are replaced and answered from `usb_gamepad_report` (host XInput pad, keyboard, `RECOMP_PAD_SCRIPT`), as in the first game: XInitDevices `0x001F0704`, XGetDevices `0x001F0709`, XGetDeviceChanges `0x001F072B`, XInputOpen `0x001F0402`, XInputClose `0x001F0458`, XInputGetCapabilities `0x001F0464`, XInputGetState `0x001F063C`, XInputSetState `0x001F06AF`.
- `sub_001E1D80` is the PFB flush wait (sets `0x100410` bit 16 and spins until it clears); the runtime's ack thread clears it, so it costs time rather than hanging.
- Startup order seen in the file log: FromSoftware logo, title screen (`menu\fe_ti*`), Sega logo, menu music (`sound\jpn\stream_menu`), attract movie. Probed and missing on the disc, harmlessly: `z:\OTOGI-HTE9 MAP 01..30` (cache partition, first run), `sfx\sNNN.dds`.
- With `RECOMP_PAD_SCRIPT` pressing Start every 0.7 s, the title opens the main menu (`menu\bin\mainmenu_locz.med`, `menu\fe_me*`, `menu\fe_bg*`) and loads `map\m200\` and `menu\m000prm.dat` about 3 s into the script, before any A press; most likely the menu's 3D background rather than a stage. 74 further presses load nothing else. No crash or untranslated instruction in 85 s.
- XPP's USB code reaches `cli`/`sti` at `0x001F456B`/`0x001F457C` when the OHCI model is on; harmless as no-ops.

## Renderer (M3)

- D3D names and globals from XbSymbolDatabase v3.1.160 (`XbSymbolDatabaseCLI.exe default.xbe -e`, 380 symbols). It confirms 18 of the first game's D3D fixes the comparison left unconfirmed. Globals: `D3D_g_pDevice` `0x001EE1F8`, `D3D_g_RenderState` `0x001EE208` (deferred block at index 92, complex at 136, as in the renderer's XDK 5849 enum), `D3D_g_DeferredTextureState` `0x001EBB40`, `D3D_g_Stream` `0x001EF1C8`. Not found: `CreatePixelShader` and the pushbuffer recording functions (`BeginPush`, `EndPush`, `BeginPushBuffer`, `RunPushBuffer`).
- The renderer is the Sneak King recomp's `gl_hle.c` (XDK 5849, fixed function and NV2A vertex programs on OpenGL ES 3), in `src/render/`, with its guest entry points replaced by the `gl_hle.h` API. `src/games/otogi2/d3d.cpp` intercepts the game's D3D and calls it. ANGLE is the Sneak King release build of commit feebf60, downloaded by `cmake/angle.cmake` and checked against its SHA-256.
- D3D8LTCG passes some arguments in registers. Each convention was read from the prologue and `ret` of the function (`analysis` script: bytes popped, registers read before written), then checked against the XbSymbolDatabase name:

| Function | Address | Arguments |
|---|---|---|
| Swap | `0x001DC950` | eax = Flags |
| Clear | `0x001DE200` | 6 on the stack |
| SetRenderTarget | `0x001DE900` | 2 on the stack |
| CopyRects | `0x001DEC00` | 5 on the stack |
| SetTransform | `0x001DEF30` | eax = State, edx = pMatrix |
| SetViewport | `0x001DF040` | 1 on the stack |
| SetTexture | `0x001DF260` | 2 on the stack |
| SetIndices | `0x001DF420` | ebx = pIndexData, BaseVertexIndex on the stack |
| SetStreamSource | `0x001DFCE0` | 3 on the stack |
| CreateVertexShader | `0x001E0370` | 4 on the stack |
| SetVertexShader | `0x001E05E0` | 1 on the stack |
| SetVertexShaderConstant1 / 4 | `0x001E0750` / `0x001E07B0` | ecx = Register, edx = pConstantData |
| SetVertexShaderConstantNotInlineFast | `0x001E0860` | ecx, edx, dword count on the stack |
| SetPixelShader | `0x001E09F0` | eax = Handle |
| DrawVerticesUP | `0x001E0F50` | 4 on the stack |
| DrawVertices | `0x001E1100` | PrimitiveType on the stack, ecx = StartVertex, eax = VertexCount |
| DrawIndexedVertices | `0x001E11D0` | 3 on the stack |

- The device layout matches the renderer's: render target at `device+0x1A04`, depth at `+0x1A08` (what the game's Clear reads), back buffer at `+0x1A14`.
- The host opens its own window (`src/host/window.cpp`). The toolkit's framebuffer window would need two functions only the Sneak King recomp's patched toolkit has.
- First run: ANGLE on Direct3D 11, 640x480 targets rendered at 2x, about 40 presents a second, no crash, but black frames: the hooked draws saw 0 to 25 calls a second.
- Most 2D is written into the pushbuffer by the game itself. `sub_000507F0` (a textured quad, 54 callers) sets texture, pixel shader and FVF, reserves space with `sub_001E1DB0` (`D3DDevice_BeginPush`, esi = dword count, returns the write pointer), writes `SET_BEGIN_END(8)` (quads), `INLINE_ARRAY` of 52 dwords (4 vertices of FVF `0x444`: XYZRHW, diffuse, four texture coordinates) and `SET_BEGIN_END(0)`, then stores the new pointer to `device+0` itself. 17 sites write `SET_BEGIN_END`; two (`0x00050FC6`, `0x00052648`) do not go through BeginPush and only call `D3D_MakeRequestedSpace` (`0x001E1A90`) when the ring is full. `D3DDevice_End` (`0x001E1510`) has one caller.
- So the renderer decodes the pushbuffer itself: everything written between `device+0` at the previous flush and now, drawing `SET_BEGIN_END`/`INLINE_ARRAY` primitives in the current vertex format and skipping other methods. A flush runs at the entry of every intercepted D3D function (before it changes state), in BeginPush, and in MakeRequestedSpace before the ring wraps (the scan restarts after it).
- Black frames after that: every draw had a 2147483647x2147483647 viewport. The game passes `0x7FFFFFFF` for "the whole target" (the first game does too), which D3D clips to the render target; the pre-transformed vertex path used the raw size. The renderer now clips it for the shaders as it already did for `glViewport`.
- With both, the FromSoftware logo, the menu transitions and the Game Settings screen draw. Text and some icons had black boxes behind them: their pixel shaders (NV2A register combiners) fell back to texture stages.
- Pixel shaders: `CreatePixelShader` is gone under LTCG, but `SetPixelShader` (`0x001E09F0`, eax = handle) finds the `D3DPIXELSHADERDEF` at `[handle+8]` and copies its first 57 dwords into `D3D_g_RenderState[0..56]` and its texture modes into `PSTEXTUREMODES` (`0x001EE428`, index 136). The renderer reads the combiner program from those render states at draw time, so constants set later reach it too. The combiners are translated to GLSL after the first game's generator (Otogi XUnchain's `gl_hle.zig`, itself after the toolkit's `nv2a_combiner.c`): the program words are part of the program key, the constants are uniforms. Text and icons now draw cleanly. Bump and dot-product texture modes still sample as 2D (logged once if seen).
- `CopyRects` (`0x001DEC00`, 5 on the stack): every menu frame copies the back buffer into a 640x480 texture and draws it back through a pixel shader, as in the first game. Guest memory holds nothing the host drew, so after the guest's copy the renderer blits between the host targets and records the destination's guest hash, so its CPU-side resync does not overwrite the blit. With it the main menu shows its character art and the settings their painted background.
- `RECOMP_GL_DUMP_SHADERS=<dir>` writes each generated program as `prog_NNNN.vert` and `.frag`; the draw log gives the program number of each draw.
- Frame capture: `RECOMP_GL_CAPTURE=<file>`. Each time the file is written, the next frame is logged draw by draw on stderr (`[CAP]`: caller, program, primitive, shaders, texture modes, target, viewport, depth, blend, cull, textures; the draws dropped and why; the NV2A methods seen in the pushbuffer). Each target is written as `<file>_<n>_pass<k>_<address>.bmp` when the draws move on to another one, and every target drawn into at the present. A digit in `<file>.view` switches vertex-program draws to a diagnostic view: 1 coverage (flat magenta, on top), 2 texture 0, 3 diffuse, 4 pixel shader colour, 5 its alpha.
- Frame dumps: `RECOMP_D3D9_DUMP=<prefix>`, `RECOMP_D3D9_DUMP_EVERY=<frames>` (BMP); `RECOMP_D3D9_STATS=1` prints presents, draws, texture creations and program compilations each second; `RECOMP_GL_DRAWLOG=<n>` prints the first n draws (primitive, count, vertex format, stride, target, viewport, first vertex).

## Into the first stage

- Route: Start through the logos and title, A on New Game, then Game Settings and Sound Settings (four times down to Next Page, A each), Controller Config (A), save slot 01 (A), "Create a new Save Game?" YES (A), A through the story text; A skips the `map\m050` cutscene, then `map\m000` loads. A main menu left alone returns to the title after about 15 s. With a save present the menu starts on Load.
- Saves are created (`UDATA\46530004\<id>\game.xsv`, `map.xsv` 82 MB, `SaveMeta.xbx`, `saveimage.xbx`), but the Load screen then reports "No Save Game found". The game also looks for the first game's saves (`UDATA\46530002`).
- Most vertex shader constants are not set through `SetVertexShaderConstant`: the title writes them into the pushbuffer itself (`SET_TRANSFORM_CONSTANT_LOAD` `0x1EA4`, then `SET_TRANSFORM_CONSTANT` `0x0B80..0x0BFC`, about 7,000 a frame in `m000`). The pushbuffer decoder applies them. A stage creates a few hundred vertex programs and frees them on unload; a freed handle comes back with other microcode, so programs are keyed on their creation, not their handle.
- With that, the stage's scene renders into the back buffer (the cave, its pillars and rocks, textured). A post-processing chain then replaces it: the depth buffer (`8065C000`) is read as an `A8R8G8B8` texture and reduced to 160x120, blurred and composed back, and the final pass reads the scene through a distortion map with pixel shader texture mode `0x10` (dependent read on green and blue). The renderer has no depth in guest memory and samples mode `0x10` as 2D, so the result is a flat dark colour.
- In `m000` the screen is black apart from the HUD's fan, but the stage runs: `sub_0004B5A0` (play a stage) is in its loop at `0x0004BA50`, and Start opens the pause menu (map, Seimei level 10, "Go to the deepest area of the cave").
- The in-engine cutscene draws its sky, terrain and torii, but dark and washed out, with untextured grey characters and a dark rectangle at the top left. Pixel shader texture mode `0x10` is sampled as 2D.
- Audio plays, choppy: XAudio2 reports hundreds of underruns.
- `RECOMP_SAVE_DIR=<dir>` keeps a run's saves apart. `RECOMP_PAD_LIVE=<file>` presses each line appended to the file; `RECOMP_PAD_SCRIPT=@<file>` reads a timeline.
- `OTOGI_STACKS_EVERY=<seconds>` (host) prints, for every thread in the exe, the function it is in and the exe addresses on its stack, named from the symbols (`sub_XXXXXXXX`). It only copies while a thread is suspended; printing then could deadlock on a lock the suspended thread holds.
- A runtime default can be turned off with `0` in the environment (the runtime tests most options for presence only).

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
