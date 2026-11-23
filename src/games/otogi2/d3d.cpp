// Otogi 2's D3D (XDK 5849, D3D8LTCG) intercepted for the renderer.
//
// Addresses and names from XbSymbolDatabase. LTCG passes some arguments in
// registers; each entry reads them as this build does (prologue and `ret` of
// each function checked). Setters mirror their state into the renderer and
// then run the lifted body, which still keeps D3D's own state. Draws are
// replaced: nothing executes the pushbuffer. Every entry first draws what the
// game wrote into the pushbuffer itself since the previous one, while the state
// those primitives were written under is still current.
//
// Scanned by tools.recomp --exclude-manual like overrides.cpp: keep the
// "void sub_X(void)" and "extern void sub_X_gen(void);" lines at column 0.

#include "host/guest.hpp"
#include "render/gl_hle.h"

#include <cstring>

using otogi::guest::arg;
using otogi::guest::ret;

namespace {

float as_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

}  // namespace

extern "C" {

extern void sub_001DC950_gen(void);
extern void sub_001DE200_gen(void);
extern void sub_001DF040_gen(void);
extern void sub_001DEF30_gen(void);
extern void sub_001DF260_gen(void);
extern void sub_001DFCE0_gen(void);
extern void sub_001DF420_gen(void);
extern void sub_001E05E0_gen(void);
extern void sub_001E09F0_gen(void);
extern void sub_001E0370_gen(void);
extern void sub_001E0750_gen(void);
extern void sub_001E07B0_gen(void);
extern void sub_001E0860_gen(void);
extern void sub_001DEC00_gen(void);
extern void sub_001E1DB0_gen(void);

// D3DDevice_CopyRects(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface, pDestPointsArray).
// The menus copy the back buffer into a texture every frame and draw it back
// through a pixel shader.
void sub_001DEC00(void) {
    gl_hle_pushbuffer_flush();
    const uint32_t source = arg(0), rects = arg(1), count = arg(2), destination = arg(3), points = arg(4);
    sub_001DEC00_gen();
    gl_hle_copy_rects(source, rects, count, destination, points);
}
extern void sub_001E1A90_gen(void);

// D3DDevice_BeginPush(esi = dword count) -> eax = where the game writes methods
void sub_001E1DB0(void) {
    gl_hle_pushbuffer_flush();
    sub_001E1DB0_gen();
}

// D3D_MakeRequestedSpace(...): may wrap the pushbuffer ring to its start.
void sub_001E1A90(void) {
    gl_hle_pushbuffer_flush();
    sub_001E1A90_gen();
    gl_hle_pushbuffer_restart();
}

// D3DDevice_Swap(eax = Flags)
void sub_001DC950(void) {
    gl_hle_pushbuffer_flush();
    const uint32_t flags = g_eax;
    const uint32_t back_buffer = gl_hle_back_buffer();
    sub_001DC950_gen();
    gl_hle_swap(flags, back_buffer);
}

// D3DDevice_Clear(Count, pRects, Flags, Color, Z, Stencil)
void sub_001DE200(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_clear(arg(0), arg(1), arg(2), arg(3), as_float(arg(4)), arg(5));
    sub_001DE200_gen();
}

// D3DDevice_SetViewport(pViewport)
void sub_001DF040(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_set_viewport(arg(0));
    sub_001DF040_gen();
}

// D3DDevice_SetTransform(eax = State, edx = pMatrix)
void sub_001DEF30(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_set_transform(g_eax, g_edx);
    sub_001DEF30_gen();
}

// D3DDevice_SetTexture(Stage, pTexture)
void sub_001DF260(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_set_texture(arg(0), arg(1));
    sub_001DF260_gen();
}

// D3DDevice_SetStreamSource(StreamNumber, pStreamData, Stride)
void sub_001DFCE0(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_set_stream_source(arg(0), arg(1), arg(2));
    sub_001DFCE0_gen();
}

// D3DDevice_SetIndices(ebx = pIndexData, BaseVertexIndex)
void sub_001DF420(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_set_indices(g_ebx, arg(0));
    sub_001DF420_gen();
}

// D3DDevice_SetVertexShader(Handle)
void sub_001E05E0(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_set_vertex_shader(arg(0));
    sub_001E05E0_gen();
}

// D3DDevice_SetPixelShader(eax = Handle)
void sub_001E09F0(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_set_pixel_shader(g_eax);
    sub_001E09F0_gen();
}

// D3DDevice_CreateVertexShader(pDeclaration, pFunction, pHandle, Usage)
void sub_001E0370(void) {
    gl_hle_pushbuffer_flush();
    const uint32_t declaration = arg(0), function = arg(1), handle = arg(2);
    sub_001E0370_gen();
    gl_hle_create_vertex_shader(declaration, function, handle);
}

// D3DDevice_SetVertexShaderConstant1(ecx = Register, edx = pConstantData)
void sub_001E0750(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_set_vertex_shader_constants(static_cast<int32_t>(g_ecx), g_edx, 1);
    sub_001E0750_gen();
}

// D3DDevice_SetVertexShaderConstant4(ecx = Register, edx = pConstantData)
void sub_001E07B0(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_set_vertex_shader_constants(static_cast<int32_t>(g_ecx), g_edx, 4);
    sub_001E07B0_gen();
}

// D3DDevice_SetVertexShaderConstantNotInlineFast(ecx = Register, edx = pConstantData, DwordCount)
void sub_001E0860(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_set_vertex_shader_constants(static_cast<int32_t>(g_ecx), g_edx, arg(0) / 4);
    sub_001E0860_gen();
}

// D3DDevice_DrawVerticesUP(PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride)
void sub_001E0F50(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_draw_vertices_up(arg(0), arg(1), arg(2), arg(3));
    ret(16);
}

// D3DDevice_DrawVertices(PrimitiveType, ecx = StartVertex, eax = VertexCount)
void sub_001E1100(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_draw_vertices(arg(0), g_ecx, g_eax);
    ret(4);
}

// D3DDevice_DrawIndexedVertices(PrimitiveType, VertexCount, pIndexData)
void sub_001E11D0(void) {
    gl_hle_pushbuffer_flush();
    gl_hle_draw_indexed_vertices(arg(0), arg(1), arg(2));
    ret(12);
}

}  // extern "C"
