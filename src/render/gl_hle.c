/*
 * gl_hle.c: the game's Xbox D3D8 (XDK 5849) mapped onto OpenGL ES 3.0 --
 * WebGL2 in the browser, ANGLE on Windows, which runs it on Direct3D 11,
 * Vulkan or desktop GL (RECOMP_GFX). The guest side (wrapped entry points,
 * guest state, vertex declarations, the NV2A microcode decoder) is in
 * d3d_hle_shared.h.
 *
 * D3D8 has a fixed-function pipeline; GL ES has none, so this generates
 * GLSL per state combination: the vertex half from the FVF, lighting and
 * texture-coordinate state (or the title's own vertex program, translated
 * from NV2A microcode), the fragment half from the texture stages, fog and
 * alpha test. Programs are cached on that key.
 *
 * Orientation: render targets keep the guest's row order, row 0 at the top,
 * so guest memory, textures and render targets agree without flips. That
 * makes GL's window space the guest's screen space: clip y is negated after
 * projection, a D3D clockwise triangle is GL counter-clockwise, and only the
 * blit to the window flips.
 *
 * Xbox D3D keeps most state as the NV2A's own values, which are OpenGL's:
 * compare functions, blend factors and equations, stencil operations and
 * cull faces go to GL unchanged.
 *
 * From the Sneak King recomp (https://github.com/Guuzzeji/sneaky-whopper-recomp),
 * MIT License, Copyright (c) 2026 The Sneak King recomp contributors. Adapted
 * for otogi2-recomp: Windows only, no overlay or mods, and the guest entry
 * points replaced by the gl_hle.h API, which each game's glue calls with the
 * arguments its D3D build passes.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <windows.h>
#include <intrin.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>

#include "d3d_hle_shared.h"
#include "d3d8_swizzle.h"
#include "gl_hle.h"

/* -- Platform: lock, clock, window --------------------------------------- */

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* The lock also times the renderer: s_in_ms is the time spent inside its
 * entry points this frame (outermost lock to unlock), the rest of a frame is
 * the game's own code. */
static CRITICAL_SECTION s_lock_cs;
static int s_lock_depth;
static double s_lock_t0, s_in_ms;
static void lock_init(void) { InitializeCriticalSection(&s_lock_cs); }
static void lock(void)
{
    EnterCriticalSection(&s_lock_cs);
    if (!s_lock_depth++)
        s_lock_t0 = now_ms();
}
static void unlock(void)
{
    if (!--s_lock_depth)
        s_in_ms += now_ms() - s_lock_t0;
    LeaveCriticalSection(&s_lock_cs);
}
static void sleep_ms(double ms) { if (ms >= 1) Sleep((DWORD)ms); else Sleep(0); }

/* Per-frame counters, printed in [HITCH] lines (RECOMP_PERF). */
typedef struct {
    unsigned draws, prims, tex_creates, vs_compiles, prog_loads, rt_uploads, depth_convs;
} RenderStats;

static int s_ready, s_failed, s_s3tc;
static uint32_t s_scale = 2;
static RenderStats s_st;
static unsigned s_hd_hits;
static uint32_t s_frame;
static GLuint s_vao, s_vbo, s_ibo, s_fbo, s_fbo_read, s_up_tex, s_samp[4];
static uint32_t s_up_w, s_up_h;

static EGLDisplay s_egl_dpy;
static EGLSurface s_egl_surf;

static int has_ext(const char *name)
{
    GLint i, n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (i = 0; i < n; i++)
        if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), name))
            return 1;
    return 0;
}

/* RECOMP_GFX=d3d11 (default), vulkan or gl: ANGLE's back end. */
static int ctx_create(void)
{
    static const EGLint cfg_attrs[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE,
    };
    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_CONTEXT_OPENGL_NO_ERROR_KHR, EGL_TRUE,      /* skip ANGLE's validation */
        EGL_NONE,
    };
    const char *gfx = getenv("RECOMP_GFX");
    EGLAttrib dpy_attrs[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, EGL_NONE };
    HWND hwnd = (HWND)gl_hle_host_window();
    EGLConfig cfg;
    EGLContext ctx;
    EGLint n = 0;

    if (!hwnd)
        return -1;                          /* no window yet: try again later */
    if (gfx && !_stricmp(gfx, "vulkan"))
        dpy_attrs[1] = EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE;
    else if (gfx && !_stricmp(gfx, "gl"))
        dpy_attrs[1] = EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE;
    else if (gfx && *gfx && _stricmp(gfx, "d3d11"))
        fprintf(stderr, "[GL] RECOMP_GFX=%s unknown; using d3d11\n", gfx);
    s_egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, (void *)EGL_DEFAULT_DISPLAY, dpy_attrs);
    if (s_egl_dpy == EGL_NO_DISPLAY || !eglInitialize(s_egl_dpy, NULL, NULL)
        || !eglChooseConfig(s_egl_dpy, cfg_attrs, &cfg, 1, &n) || !n) {
        fprintf(stderr, "[GL] EGL display (RECOMP_GFX=%s): error 0x%x\n", gfx ? gfx : "d3d11", eglGetError());
        return 0;
    }
    s_egl_surf = eglCreateWindowSurface(s_egl_dpy, cfg, (EGLNativeWindowType)hwnd, NULL);
    ctx = eglCreateContext(s_egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (s_egl_surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT
        || !eglMakeCurrent(s_egl_dpy, s_egl_surf, s_egl_surf, ctx)) {
        fprintf(stderr, "[GL] EGL context: error 0x%x\n", eglGetError());
        return 0;
    }
    /* RECOMP_VSYNC=1 waits for the display's refresh at each present (no
     * tearing); off by default, as the game paces itself by its vblank. */
    {
        const char *vs = getenv("RECOMP_VSYNC");
        eglSwapInterval(s_egl_dpy, vs && *vs && strcmp(vs, "0") != 0);
    }
    /* ANGLE names DXT1/3/5 three ways; GL_EXT_texture_compression_s3tc where it can. */
    s_s3tc = has_ext("GL_EXT_texture_compression_s3tc")
          || (has_ext("GL_EXT_texture_compression_dxt1") && has_ext("GL_ANGLE_texture_compression_dxt5"));
    {
        /* 640x480 times the render scale, shrunk to fit the screen. */
        RECT wa, wr = { 0, 0, (LONG)(640 * s_scale), (LONG)(480 * s_scale) };
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        while (wr.right > (wa.right - wa.left) * 9 / 10 || wr.bottom > (wa.bottom - wa.top) * 9 / 10) {
            wr.right = wr.right * 3 / 4;
            wr.bottom = wr.bottom * 3 / 4;
        }
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    return 1;
}

static void win_size(int *w, int *h)
{
    RECT cr;
    *w = *h = 0;
    if (GetClientRect((HWND)gl_hle_host_window(), &cr)) {
        *w = cr.right - cr.left;
        *h = cr.bottom - cr.top;
    }
}

static void swap_window(void) { eglSwapBuffers(s_egl_dpy, s_egl_surf); }

static int dev_ready(void)
{
    if (s_ready)
        return 1;
    if (s_failed)
        return 0;
    switch (ctx_create()) {
    case 1: break;
    case -1: return 0;
    default:
        fprintf(stderr, "[GL] context creation failed\n");
        s_failed = 1;
        return 0;
    }
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glGenBuffers(1, &s_ibo);
    glGenFramebuffers(1, &s_fbo);
    glGenFramebuffers(1, &s_fbo_read);
    glGenSamplers(4, s_samp);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    fprintf(stderr, "[GL] %s, %s; rendering at %ux%s\n", (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION), s_scale, s_s3tc ? "" : " (no S3TC)");
    s_ready = 1;
    return 1;
}

/* -- Render targets: one host target per guest surface address ------------ */

typedef struct {
    uint32_t data, w, h;            /* guest address and size (1x) */
    uint32_t xfmt, pitch;
    uint32_t ghash, synced;
    int depth, captured;            /* captured: drawn into during a frame capture */
    GLuint tex;                     /* colour, or depth-stencil, texture */
    GLuint conv;                    /* depth: its bytes as an A8R8G8B8 colour texture */
    uint32_t written, conv_at;      /* s_serial of the last write; written when conv was made */
} HostRt;

#define MAX_RT 32
static HostRt s_rt[MAX_RT];
static int s_nrt;
static HostRt *s_cur_rt, *s_cur_ds;
static uint32_t s_serial;           /* orders writes to targets sharing guest memory */

static HostRt *rt_find_kind(uint32_t data, int depth)
{
    int i;
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i].data == data && s_rt[i].depth == depth)
            return &s_rt[i];
    return NULL;
}

static HostRt *rt_find(uint32_t data) { return rt_find_kind(data, 0); }

/* The last value the title wrote to each NV2A 3D method through the
 * pushbuffer, indexed by method / 4. */
static uint32_t s_nv[0x800];
#define NV(m) s_nv[((m) >> 2) & 0x7FF]

/* The surface was last set in the pushbuffer (SET_SURFACE_COLOR_OFFSET), not
 * by SetRenderTarget: Otogi 2 points some post-processing passes at their
 * targets that way, leaving the device's render target as it was. */
static int s_surf_pb;

static HostRt *rt_get_raw(uint32_t data, uint32_t w, uint32_t h, uint32_t pitch, uint32_t xfmt, int depth)
{
    HostRt *r;
    int i;

    for (i = 0; i < s_nrt; i++) {
        r = &s_rt[i];
        if (r->data == data && r->depth == depth && r->w == w && r->h == h)
            return r;
    }
    /* ponytail: no eviction; past MAX_RT the last slot is reused. */
    r = &s_rt[s_nrt < MAX_RT ? s_nrt++ : MAX_RT - 1];
    if (r->tex) glDeleteTextures(1, &r->tex);
    if (r->conv) glDeleteTextures(1, &r->conv);
    memset(r, 0, sizeof *r);
    r->data = data; r->w = w; r->h = h; r->depth = depth;
    r->xfmt = xfmt;
    r->pitch = pitch ? pitch : w * 4;
    r->synced = (uint32_t)-1;
    glGenTextures(1, &r->tex);
    glBindTexture(GL_TEXTURE_2D, r->tex);
    if (depth) {
        /* A texture rather than a renderbuffer: titles read depth back as a
         * colour texture (see depth_to_colour). */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, (GLsizei)(w * s_scale), (GLsizei)(h * s_scale), 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)(w * s_scale), (GLsizei)(h * s_scale), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    fprintf(stderr, "[GL] %s target %08X %ux%u format %02X\n", depth ? "depth" : "colour", data, w, h, r->xfmt);
    return r;
}

/* The host target for a guest surface resource. */
static HostRt *rt_get(uint32_t res, int depth)
{
    uint32_t w, h, pitch;
    surf_dims(res, &w, &h, &pitch);
    return rt_get_raw(res_data(res), w, h, pitch, (MEM32(res + 0xC) >> 8) & 0xFF, depth);
}

/* The targets the NV2A surface registers name: SET_SURFACE_CLIP_HORIZONTAL
 * and _VERTICAL (size in the high half), SET_SURFACE_FORMAT (colour format in
 * bits 0-3, zeta in 4-7), SET_SURFACE_PITCH (zeta pitch in the high half),
 * SET_SURFACE_COLOR_OFFSET and SET_SURFACE_ZETA_OFFSET (physical addresses). */
static void rt_from_surface_regs(HostRt **c, HostRt **d)
{
    uint32_t w = NV(0x0200) >> 16, h = NV(0x0204) >> 16, fmt = NV(0x0208), pitch = NV(0x020C);
    uint32_t colour = NV(0x0210), zeta = NV(0x0214);

    *c = colour ? rt_get_raw(0x80000000u | colour, w, h, pitch & 0xFFFF, (fmt & 0xF) == 3 ? 0x11 : 0x12, 0) : NULL;
    *d = zeta ? rt_get_raw(0x80000000u | zeta, w, h, pitch >> 16, ((fmt >> 4) & 0xF) == 1 ? 0x30 : 0x2E, 1) : NULL;
}

static uint32_t rt_guest_bytes(const HostRt *r) { return r->pitch * r->h; }

/* Guest memory of a colour target is what the CPU sees of it. Nothing on the
 * host draws there: Clear fills it, the CPU writes the rest (the XMV player
 * decodes movie frames straight into the back buffer). So when its hash has
 * changed since we last looked, the CPU wrote it, and the host target takes
 * the new pixels.
 *
 * ponytail: the sampled hash misses a CPU write of a few pixels; the movie
 * player rewrites the whole frame. GPU results are not copied back to guest
 * memory for CPU readers. */
static uint8_t *s_conv;
static size_t s_conv_cap;

static uint8_t *conv_buf(size_t n)
{
    if (n > s_conv_cap) {
        free(s_conv);
        s_conv_cap = n * 2;
        s_conv = (uint8_t *)malloc(s_conv_cap);
    }
    return s_conv;
}

static void rt_upload(HostRt *r)
{
    uint8_t *d = conv_buf((size_t)r->w * r->h * 4);
    uint32_t x, y, bpp16 = r->xfmt == 0x11 || r->xfmt == 0x05;

    s_st.rt_uploads++;
    for (y = 0; y < r->h; y++) {
        const uint8_t *s = (const uint8_t *)gp(r->data) + (size_t)y * r->pitch;
        uint8_t *o = d + (size_t)y * r->w * 4;
        for (x = 0; x < r->w; x++, o += 4) {
            if (bpp16) {
                uint32_t p = ((const uint16_t *)s)[x];
                uint32_t R = (p >> 11) & 31, G = (p >> 5) & 63, B = p & 31;
                o[0] = (uint8_t)(R << 3 | R >> 2); o[1] = (uint8_t)(G << 2 | G >> 4);
                o[2] = (uint8_t)(B << 3 | B >> 2); o[3] = 255;
            } else {
                o[0] = s[x * 4 + 2]; o[1] = s[x * 4 + 1]; o[2] = s[x * 4]; o[3] = 255;
            }
        }
    }
    if (!s_up_tex || s_up_w != r->w || s_up_h != r->h) {
        if (!s_up_tex) glGenTextures(1, &s_up_tex);
        glBindTexture(GL_TEXTURE_2D, s_up_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)r->w, (GLsizei)r->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        s_up_w = r->w;
        s_up_h = r->h;
    }
    glBindTexture(GL_TEXTURE_2D, s_up_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)r->w, (GLsizei)r->h, GL_RGBA, GL_UNSIGNED_BYTE, d);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo_read);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_up_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, (GLint)r->w, (GLint)r->h, 0, 0, (GLint)(r->w * s_scale), (GLint)(r->h * s_scale),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    s_cur_rt = NULL;                         /* the draw FBO's attachments changed */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    r->written = ++s_serial;
}

static void rt_sync(HostRt *r, int force)
{
    uint32_t h;
    if (!r || r->depth || !r->tex || (!force && r->synced == s_frame))
        return;
    r->synced = s_frame;
    h = hash_bytes((const uint8_t *)gp(r->data), rt_guest_bytes(r));
    if (h != r->ghash) {
        r->ghash = h;
        rt_upload(r);
    }
}

typedef struct { int32_t x1, y1, x2, y2; } Rect;

static void rt_fill(HostRt *r, const Rect *rects, uint32_t n, uint32_t argb)
{
    Rect full = { 0, 0, 0, 0 };
    uint32_t i, x, y, bpp16 = r->xfmt == 0x11 || r->xfmt == 0x05;
    uint16_t c16 = (uint16_t)(((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F));

    full.x2 = (int32_t)r->w;
    full.y2 = (int32_t)r->h;
    if (!n) { rects = &full; n = 1; }
    for (i = 0; i < n; i++) {
        int32_t x1 = rects[i].x1 < 0 ? 0 : rects[i].x1, y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
        int32_t x2 = rects[i].x2 > (int32_t)r->w ? (int32_t)r->w : rects[i].x2;
        int32_t y2 = rects[i].y2 > (int32_t)r->h ? (int32_t)r->h : rects[i].y2;
        for (y = (uint32_t)y1; (int32_t)y < y2; y++) {
            uint8_t *row = (uint8_t *)gp(r->data) + (size_t)y * r->pitch;
            for (x = (uint32_t)x1; (int32_t)x < x2; x++) {
                if (bpp16) ((uint16_t *)row)[x] = c16;
                else ((uint32_t *)row)[x] = argb;
            }
        }
    }
    r->ghash = hash_bytes((const uint8_t *)gp(r->data), rt_guest_bytes(r));
    r->written = ++s_serial;
}

/* -- Depth read as colour, and colour used as depth ------------------------ */

/* Titles read a LIN_D24S8 depth buffer back as an A8R8G8B8 texture, and bind
 * a colour target they drew such bytes into as a depth buffer. Each 32-bit
 * word is depth << 8 | stencil, so as A8R8G8B8: A = depth bits 23..16,
 * R = 15..8, G = 7..0, B = stencil. Host colour textures hold the guest's R,
 * G, B, A in that order.
 *
 * ponytail: stencil reads as 0 (GL ES 3.0 cannot sample it), and a colour
 * target used as depth loads no stencil. */

static GLuint compile(GLenum type, const char *src);

static const char *const k_zconv_vs =
    "#version 300 es\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *const k_depth_to_colour_fs =
    "#version 300 es\n"
    "precision highp float;\nprecision highp int;\n"
    "uniform highp sampler2D u_src;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "  ivec2 c = min(ivec2(gl_FragCoord.xy), textureSize(u_src, 0) - 1);\n"
    "  uint d = uint(texelFetch(u_src, c, 0).r * 16777215.0 + 0.5);\n"
    "  o_color = vec4(float((d >> 8) & 255u), float(d & 255u), 0.0, float(d >> 16)) / 255.0;\n"
    "}\n";

static const char *const k_colour_to_depth_fs =
    "#version 300 es\n"
    "precision highp float;\nprecision highp int;\n"
    "uniform highp sampler2D u_src;\n"
    "void main() {\n"
    "  ivec2 c = min(ivec2(gl_FragCoord.xy), textureSize(u_src, 0) - 1);\n"
    "  uvec4 b = uvec4(texelFetch(u_src, c, 0) * 255.0 + 0.5);\n"
    "  gl_FragDepth = float((b.a << 16) | (b.r << 8) | b.g) / 16777215.0;\n"
    "}\n";

static GLuint s_zfbo, s_zprog[2];       /* depth to colour, colour to depth */

static GLuint zconv_prog(int which)
{
    if (!s_zprog[which]) {
        GLuint vs = compile(GL_VERTEX_SHADER, k_zconv_vs);
        GLuint fs = compile(GL_FRAGMENT_SHADER, which ? k_colour_to_depth_fs : k_depth_to_colour_fs);
        GLint ok = 0;
        GLuint p = glCreateProgram();
        if (vs) glAttachShader(p, vs);
        if (fs) glAttachShader(p, fs);
        glLinkProgram(p);
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        if (!ok) {
            fprintf(stderr, "[GL] depth conversion program %d does not link\n", which);
            glDeleteProgram(p);
            return 0;
        }
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "u_src"), 4);
        s_zprog[which] = p;
    }
    return s_zprog[which];
}

/* One full-target pass from src into s_zfbo, whose attachments the caller
 * set. The caller's draw sets all the state this changes again: bind_targets
 * the framebuffer and viewport, apply_render_state the rest, prepare_draw the
 * program. */
static void zconv_pass(int which, GLuint src, const HostRt *dst)
{
    GLuint p = zconv_prog(which);
    if (!p)
        return;
    s_st.depth_convs++;
    glViewport(0, 0, (GLsizei)(dst->w * s_scale), (GLsizei)(dst->h * s_scale));
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glUseProgram(p);
    glActiveTexture(GL_TEXTURE4);
    glBindSampler(4, 0);
    glBindTexture(GL_TEXTURE_2D, src);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
}

static int depth_is_d24(const HostRt *d) { return d->xfmt == 0x2A || d->xfmt == 0x2E; }

/* d->conv from the host depth, if depth was written since it was made. */
static void depth_to_colour(HostRt *d)
{
    if (!depth_is_d24(d)) {
        static int said;
        if (!said++)
            fprintf(stderr, "[GL] depth format 0x%02X read as a texture: not handled\n", d->xfmt);
        return;
    }
    if (d->conv && d->conv_at == d->written)
        return;
    if (!s_zfbo)
        glGenFramebuffers(1, &s_zfbo);
    if (!d->conv) {
        glGenTextures(1, &d->conv);
        glBindTexture(GL_TEXTURE_2D, d->conv);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)(d->w * s_scale), (GLsizei)(d->h * s_scale), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_zfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->conv, 0);
    glDisable(GL_DEPTH_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    zconv_pass(0, d->tex, d);
    d->conv_at = d->written;
}

/* The host depth d from the colour target c at the same guest address, when
 * c was drawn since d was last written. */
static void colour_to_depth(HostRt *d, HostRt *c)
{
    if (!depth_is_d24(d) || c->written <= d->written)
        return;
    if (!s_zfbo)
        glGenFramebuffers(1, &s_zfbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_zfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d->tex, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    zconv_pass(1, c->tex, d);
    d->written = c->written;                 /* the same bytes: neither is newer */
}

/* Viewport as the title last set it (1x pixels). */
static struct { uint32_t x, y, w, h; float minz, maxz; } s_vp = { 0, 0, 640, 480, 0.0f, 1.0f };

/* The viewport clipped to the render target, as D3D does: titles pass
 * 0x7FFFFFFF for "the whole target". */
static uint32_t s_vp_cw = 1, s_vp_ch = 1;

static void bind_targets(void)
{
    uint32_t dev = xb_device();
    uint32_t rt = MEM32(dev + XB_DEV_RENDER_TARGET), ds = MEM32(dev + XB_DEV_DEPTH_STENCIL);
    HostRt *c, *d;
    uint32_t w = s_vp.w, h = s_vp.h;

    if (s_surf_pb) {
        rt_from_surface_regs(&c, &d);
    } else {
        c = rt ? rt_get(rt, 0) : NULL;
        d = ds ? rt_get(ds, 1) : NULL;
    }

    rt_sync(c, 0);
    if (d) {
        HostRt *dc = rt_find(d->data);
        if (dc && dc != c)
            colour_to_depth(d, dc);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    if (c != s_cur_rt) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c ? c->tex : 0, 0);
        s_cur_rt = c;
    }
    if (d != s_cur_ds) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d ? d->tex : 0, 0);
        s_cur_ds = d;
    }
    if (c) {
        if (s_vp.x + w > c->w) w = c->w > s_vp.x ? c->w - s_vp.x : 0;
        if (s_vp.y + h > c->h) h = c->h > s_vp.y ? c->h - s_vp.y : 0;
    }
    s_vp_cw = w ? w : 1;
    s_vp_ch = h ? h : 1;
    glViewport((GLint)(s_vp.x * s_scale), (GLint)(s_vp.y * s_scale), (GLsizei)(w * s_scale), (GLsizei)(h * s_scale));
    glDepthRangef(s_vp.minz, s_vp.maxz);
}

/* -- Textures: everything but DXT becomes RGBA8 on the CPU ----------------- */

typedef struct {
    uint8_t xfmt, bits, swizzled;
    uint8_t kind;      /* K_* below */
} XFmt;

enum { K_L8, K_AL8, K_A1RGB5, K_X1RGB5, K_ARGB4, K_RGB565, K_ARGB8, K_XRGB8,
       K_P8, K_DXT1, K_DXT3, K_DXT5, K_A8, K_A8L8, K_ABGR8 };

static const XFmt s_fmts[] = {
    { 0x00,  8, 1, K_L8 },     { 0x01,  8, 1, K_AL8 },
    { 0x02, 16, 1, K_A1RGB5 }, { 0x03, 16, 1, K_X1RGB5 },
    { 0x04, 16, 1, K_ARGB4 },  { 0x05, 16, 1, K_RGB565 },
    { 0x06, 32, 1, K_ARGB8 },  { 0x07, 32, 1, K_XRGB8 },
    { 0x0B,  8, 1, K_P8 },
    { 0x0C,  4, 0, K_DXT1 },   { 0x0E,  8, 0, K_DXT3 }, { 0x0F,  8, 0, K_DXT5 },
    { 0x10, 16, 0, K_A1RGB5 }, { 0x11, 16, 0, K_RGB565 },
    { 0x12, 32, 0, K_ARGB8 },  { 0x13,  8, 0, K_L8 },
    { 0x19,  8, 1, K_A8 },     { 0x1A, 16, 1, K_A8L8 },
    { 0x1B,  8, 0, K_AL8 },
    { 0x1C, 16, 0, K_X1RGB5 }, { 0x1D, 16, 0, K_ARGB4 },
    { 0x1E, 32, 0, K_XRGB8 },  { 0x1F,  8, 0, K_A8 },
    { 0x20, 16, 0, K_A8L8 },
    { 0x3A, 32, 1, K_ABGR8 },  { 0x3F, 32, 0, K_ABGR8 },
};

static const XFmt *fmt_find(uint32_t xfmt)
{
    size_t i;
    for (i = 0; i < sizeof s_fmts / sizeof s_fmts[0]; i++)
        if (s_fmts[i].xfmt == xfmt)
            return &s_fmts[i];
    return NULL;
}

static int fmt_is_dxt(const XFmt *f) { return f->kind == K_DXT1 || f->kind == K_DXT3 || f->kind == K_DXT5; }

static uint32_t level_bytes(const XFmt *f, uint32_t w, uint32_t h)
{
    if (fmt_is_dxt(f))
        return ((w + 3) / 4) * ((h + 3) / 4) * (f->bits * 2);
    return w * h * f->bits / 8;
}

static GLenum dxt_gl(const XFmt *f)
{
    return f->kind == K_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
         : f->kind == K_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
}

static uint8_t c5(uint32_t v) { return (uint8_t)(v << 3 | v >> 2); }
static uint8_t c4(uint32_t v) { return (uint8_t)(v << 4 | v); }

/* One texel of a guest format as RGBA bytes. */
static void texel_rgba(const XFmt *f, const uint8_t *s, const uint32_t *pal, uint8_t *o)
{
    uint32_t p;
    switch (f->kind) {
    case K_L8: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
    case K_AL8: o[0] = o[1] = o[2] = o[3] = s[0]; break;
    case K_A8: o[0] = o[1] = o[2] = 0; o[3] = s[0]; break;
    case K_A8L8: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
    case K_A1RGB5: case K_X1RGB5:
        p = *(const uint16_t *)s;
        o[0] = c5((p >> 10) & 31); o[1] = c5((p >> 5) & 31); o[2] = c5(p & 31);
        o[3] = f->kind == K_X1RGB5 || (p & 0x8000) ? 255 : 0; break;
    case K_ARGB4:
        p = *(const uint16_t *)s;
        o[0] = c4((p >> 8) & 15); o[1] = c4((p >> 4) & 15); o[2] = c4(p & 15); o[3] = c4(p >> 12); break;
    case K_RGB565:
        p = *(const uint16_t *)s;
        o[0] = c5((p >> 11) & 31); o[1] = (uint8_t)(((p >> 5) & 63) << 2 | ((p >> 5) & 63) >> 4);
        o[2] = c5(p & 31); o[3] = 255; break;
    case K_ARGB8: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3]; break;
    case K_XRGB8: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = 255; break;
    case K_ABGR8: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
    case K_P8:
        p = pal ? pal[s[0]] : 0xFFFFFFFFu;
        o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p; o[3] = (uint8_t)(p >> 24); break;
    default: o[0] = o[1] = o[2] = o[3] = 255; break;
    }
}

/* One level of a guest texture to `target` (a 2D target or cube face). */
static void upload_level(const XFmt *f, GLenum target, int lv, const uint8_t *src,
                         uint32_t w, uint32_t h, uint32_t src_pitch, const uint32_t *pal)
{
    uint32_t bpp = f->bits / 8, x, y;
    uint8_t *tmp = NULL, *d;

    if (fmt_is_dxt(f)) {
        if (s_s3tc)
            glCompressedTexImage2D(target, lv, dxt_gl(f), (GLsizei)w, (GLsizei)h, 0,
                                   (GLsizei)level_bytes(f, w, h), src);
        /* ponytail: no S3TC means no DXT textures; decode on the CPU when a
         * browser without WEBGL_compressed_texture_s3tc matters. */
        return;
    }
    if (f->swizzled) {
        tmp = (uint8_t *)malloc((size_t)w * h * bpp);
        xbox_unswizzle_rect(tmp, src, w, h, bpp);
        src = tmp;
        src_pitch = w * bpp;
    }
    d = conv_buf((size_t)w * h * 4);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            texel_rgba(f, src + (size_t)y * src_pitch + x * bpp, pal, d + ((size_t)y * w + x) * 4);
    glTexImage2D(target, lv, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
    free(tmp);
}

typedef struct {
    uint32_t data, format, size, palette;
    uint32_t hash, checked;
    uint32_t w, h;
    int cube;
    GLuint tex;
} HostTex;

#define MAX_TEX 2048
static HostTex s_tex[MAX_TEX];
static int s_ntex;

static GLuint tex_create(const XFmt *f, uint32_t data, uint32_t format, uint32_t w, uint32_t h,
                         uint32_t pitch, const uint32_t *pal, int cube)
{
    uint32_t levels = f->swizzled || fmt_is_dxt(f) ? (format >> 16) & 0xF : 1;
    GLenum bind = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    const uint8_t *src = (const uint8_t *)gp(data);
    int faces = cube ? 6 : 1, face, lv;
    GLuint t;

    s_st.tex_creates++;
    if (!levels) levels = 1;
    if (fmt_is_dxt(f) && !s_s3tc)
        return 0;
    glGenTextures(1, &t);
    glBindTexture(bind, t);
    for (face = 0; face < faces; face++) {
        uint32_t lw = w, lh = h, face_bytes = 0;
        GLenum target = cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)face : GL_TEXTURE_2D;
        for (lv = 0; lv < (int)levels; lv++) {
            upload_level(f, target, lv, src, lw, lh, pitch ? pitch : lw * f->bits / 8, pal);
            face_bytes += level_bytes(f, lw, lh);
            src += level_bytes(f, lw, lh);
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        if (cube)                                /* faces start on 128 bytes */
            src += ((face_bytes + 127) & ~127u) - face_bytes;
    }
    glTexParameteri(bind, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    return t;
}

/* A DXT1/3/5 .dds file as a texture, or 0 (needs S3TC). Also mods_mesh.c's. */
GLuint gl_dds_load(const char *path)
{
    uint8_t hdr[128], *buf = NULL;
    uint32_t w, h, mips, fourcc, bpb, lv, off = 0;
    GLenum fmt;
    GLuint t = 0;
    long n;
    FILE *f;

    if (!s_s3tc || !(f = fopen(path, "rb")))
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f) - 128;
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || fread(hdr, 1, 128, f) != 128 || memcmp(hdr, "DDS ", 4)
        || !(buf = (uint8_t *)malloc((size_t)n)) || fread(buf, 1, (size_t)n, f) != (size_t)n)
        goto done;
    memcpy(&h, hdr + 12, 4);
    memcpy(&w, hdr + 16, 4);
    memcpy(&mips, hdr + 28, 4);
    memcpy(&fourcc, hdr + 84, 4);
    if (!mips) mips = 1;
    if (fourcc == 0x31545844u) { fmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; bpb = 8; }        /* DXT1 */
    else if (fourcc == 0x33545844u) { fmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; bpb = 16; }  /* DXT3 */
    else if (fourcc == 0x35545844u) { fmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; bpb = 16; }  /* DXT5 */
    else goto done;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    for (lv = 0; lv < mips; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        uint32_t bytes = (lw + 3) / 4 * bpb * ((lh + 3) / 4);
        if (off + bytes > (uint32_t)n)
            break;
        glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)lv, fmt, (GLsizei)lw, (GLsizei)lh, 0, (GLsizei)bytes, buf + off);
        off += bytes;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)(lv ? lv - 1 : 0));
done:
    free(buf);
    fclose(f);
    return t;
}

/* RECOMP_HD_TEX=<dir>: <dir>/<hash>.dds replaces a guest texture (DXT only,
 * so it needs S3TC; tools/xen_tex.py makes them from the 360 packages). The
 * hash is the one tex_get keys the cache on, so a texture is replaced exactly
 * when its guest bytes match; coordinates of swizzled and DXT textures are
 * normalized, so a bigger texture needs nothing else. */
static GLuint hd_load(uint32_t hash)
{
    static const char *dir = (const char *)-1;
    char path[512];
    GLuint t;

    if (dir == (const char *)-1)
        dir = getenv("RECOMP_HD_TEX");
    if (!dir || !*dir)
        return 0;
    snprintf(path, sizeof path, "%s/%08x.dds", dir, hash);
    t = gl_dds_load(path);
    if (t && s_hd_hits++ < 8)
        fprintf(stderr, "[HD] %08x\n", hash);
    return t;
}

/* The host texture for a guest texture (and palette). *lin_w/*lin_h are set
 * for linear textures, whose coordinates the title gives in texels; *cube for
 * a cube map. */
static GLuint tex_get(uint32_t res, uint32_t pal_res, float *lin_w, float *lin_h, int *cube)
{
    uint32_t data = res_data(res), format = MEM32(res + 0xC), size = MEM32(res + 0x10);
    uint32_t w, h, pitch, xf = (format >> 8) & 0xFF, bytes, hash, pal = 0;
    const XFmt *f;
    HostRt *rt;
    HostTex *t = NULL;
    int i;

    surf_dims(res, &w, &h, &pitch);
    *lin_w = *lin_h = 0;
    *cube = (format >> 2) & 1;
    rt = rt_find(data);
    {
        /* A depth buffer read as a texture: its bytes, converted by
         * prepare_draw, unless a colour target there was drawn since. */
        HostRt *z = rt_find_kind(data, 1);
        if (z && z->conv && (!rt || z->written > rt->written)) {
            if (size) { *lin_w = (float)z->w; *lin_h = (float)z->h; }
            *cube = 0;
            return z->conv;
        }
    }
    if (rt) {
        if (size) { *lin_w = (float)rt->w; *lin_h = (float)rt->h; }
        *cube = 0;
        return rt->tex;
    }
    f = fmt_find(xf);
    if (!f) {
        static uint32_t said[64];
        if (!said[xf & 63]++)
            fprintf(stderr, "[GL] texture format 0x%02X not handled\n", xf);
        return 0;
    }
    if (xf == 0x0B && pal_res)
        pal = res_data(pal_res);
    if (!f->swizzled && !fmt_is_dxt(f)) {
        *lin_w = (float)w;
        *lin_h = (float)h;
    }
    for (i = 0; i < s_ntex; i++)
        if (s_tex[i].data == data && s_tex[i].format == format && s_tex[i].size == size
            && s_tex[i].palette == pal) {
            t = &s_tex[i];
            break;
        }
    if (t && t->checked == s_frame)
        return t->tex;
    bytes = pitch ? pitch * h : level_bytes(f, w, h);
    hash = hash_bytes((const uint8_t *)gp(data), bytes);
    if (pal)
        hash ^= hash_bytes((const uint8_t *)gp(pal), 1024) * 31u;
    if (t && t->hash == hash) {
        t->checked = s_frame;
        return t->tex;
    }
    if (!t)                     /* ponytail: past MAX_TEX the last slot is recycled */
        t = &s_tex[s_ntex < MAX_TEX ? s_ntex++ : MAX_TEX - 1];
    if (t->tex)
        glDeleteTextures(1, &t->tex);
    memset(t, 0, sizeof *t);
    t->data = data; t->format = format; t->size = size; t->palette = pal;
    t->hash = hash; t->checked = s_frame; t->w = w; t->h = h; t->cube = *cube;
    t->tex = !pal && !pitch && !*cube ? hd_load(hash) : 0;
    if (!t->tex)
        t->tex = tex_create(f, data, format, w, h, pitch, pal ? (const uint32_t *)gp(pal) : NULL, *cube);
    return t->tex;
}

/* -- Mirrored state ---------------------------------------------------------- */

typedef struct { float m[16]; } Mat;
typedef struct {
    uint32_t type;
    float diffuse[4], specular[4], ambient[4], position[3], direction[3];
    float range, falloff, att0, att1, att2, theta, phi;
} Light;                                     /* D3DLIGHT8 */
typedef struct { float diffuse[4], ambient[4], specular[4], emissive[4], power; } Material;

static uint32_t s_stage_tex[4], s_stage_pal[4];
static uint32_t s_vshader;                  /* FVF, or a shader handle | 1 */
static uint32_t s_pshader;
static uint32_t s_stream_vb[16], s_stream_stride[16];
static uint32_t s_ib, s_base_vertex;
static Mat s_xf[10];                         /* view, proj, tex0-3, world0-3 */
static Light s_light[8];
static uint32_t s_light_on;
static Material s_material;
static float s_lin[4][2];                    /* 1/texel size of linear textures */
static int s_tex_cube[4];

static void mat_mul(const float *a, const float *b, float *o)   /* row-vector: o = a * b */
{
    int r, c;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

static void argb_vec(uint32_t c, float *o)
{
    o[0] = ((c >> 16) & 0xFF) / 255.0f; o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = (c & 0xFF) / 255.0f; o[3] = (c >> 24) / 255.0f;
}

/* -- Programmable vertex shaders ----------------------------------------- */

#define NV2A_VS_MAX_INSTRUCTIONS 136
#define NV2A_VS_MAX_CONSTANTS    192

typedef struct {
    uint32_t handle, gen;             /* gen: which creation of this handle */
    uint32_t decl[96];
    uint32_t mc[NV2A_VS_MAX_INSTRUCTIONS * 4];
    int nmc;
    uint16_t inputs;
    int uses_vp;
    char *glsl;                       /* translated body, built on first use */
} HostVs;

/* A stage creates a few hundred programs on top of the menus' and frees
 * them on unload; a freed handle comes back with other microcode. */
#define MAX_VS 1024
static HostVs s_vs[MAX_VS];
static int s_nvs;
static uint32_t s_vs_gen;
/* Indexed by NV2A constant slot (D3D register + 96), as the microcode reads them. */
static float s_vsc[NV2A_VS_MAX_CONSTANTS][4];

static HostVs *vs_find(uint32_t handle)
{
    int i;
    for (i = 0; i < s_nvs; i++)
        if (s_vs[i].handle == handle)
            return &s_vs[i];
    return NULL;
}

/* -- Shader generation ------------------------------------------------------ */

/* Everything the generated program depends on. Zeroed, then filled, so it
 * compares with memcmp. */
typedef struct {
    HostVs *vs;                       /* a vertex program, or NULL: fixed function */
    uint32_t vs_gen;
    /* fixed-function vertex half */
    uint8_t rhw, normal, diffuse, specular, ntc, lighting, local_viewer, normalize;
    uint8_t mat_src[4];               /* diffuse, ambient, specular, emissive: 0 material, 1 colour1, 2 colour2 */
    uint8_t light_type[8];            /* 0 off, 1 point, 2 spot, 3 directional */
    uint8_t tc_set[4], tc_gen[4], tc_count[4], tc_proj[4], tc_size[8];
    /* fragment half */
    uint8_t colorop[4], colorarg[4][3], alphaop[4], alphaarg[4][3], result_temp[4];
    uint8_t tex[4];                   /* 0 none, 1 2D, 2 cube */
    uint8_t spec_add, fog, fog_table, alpha_test, alpha_func, flat, point_sprite;
    /* pixel shader: the NV2A combiner program, constants excluded (uniforms) */
    uint8_t ps;
    uint32_t ps_ai[8], ps_ao[8], ps_ri[8], ps_ro[8], ps_f0, ps_f1, ps_ctl, ps_modes;
    uint8_t view;                     /* the frame capture's diagnostic view, 0 normal */
    uint8_t wbuf;                     /* ZENABLE USEW into a 24-bit depth buffer */
} Key;

/* W-buffering (ZENABLE D3DZB_USEW, SET_CONTROL0 Z_PERSPECTIVE): the NV2A
 * stores the fragment's clip-space w, not z. Otogi's projection is scaled so
 * that w reaches 2^24 - 1 at the far plane, and the title reads those values
 * back (depth as a texture), so the host stores w too.
 *
 * ponytail: writing gl_FragDepth bypasses glPolygonOffset, so ZBIAS and solid
 * offset do nothing on w-buffered draws. */
static void gen_wbuf(const Key *k, Sb *s)
{
    if (k->wbuf)
        sb(s, "  gl_FragDepth = clamp(1.0 / (gl_FragCoord.w * 16777215.0), 0.0, 1.0);\n");
}

/* Diagnostic views of vertex-program draws (RECOMP_GL_CAPTURE): 1 flat
 * magenta, unculled and on top (coverage), 2 texture 0, 3 diffuse colour,
 * 4 the pixel shader's colour, 5 its alpha. 2 to 5 are drawn unblended. */
enum { VIEW_NORMAL, VIEW_COVERAGE, VIEW_TEXTURE0, VIEW_DIFFUSE, VIEW_COLOUR, VIEW_ALPHA };
static int s_view;

typedef struct {
    Key key;
    GLuint prog;
    GLint u_wv, u_proj, u_texm, u_mat, u_amb, u_ld, u_ls, u_la, u_lp, u_ldir, u_latt, u_lspot;
    GLint u_rhw, u_fog, u_fogcolor, u_tf, u_aref, u_tscale, u_bump, u_c, u_psize, u_tex[4];
    GLint u_psc, u_psf;
} Prog;

#define MAX_PROG 1024
static Prog s_prog[MAX_PROG];
static int s_nprog;

/* Xbox D3DTOP values: the order differs from PC D3D after BLENDTEXTUREALPHA. */
enum { XOP_DISABLE = 1, XOP_SELECTARG1, XOP_SELECTARG2, XOP_MODULATE, XOP_MODULATE2X, XOP_MODULATE4X,
       XOP_ADD, XOP_ADDSIGNED, XOP_ADDSIGNED2X, XOP_SUBTRACT, XOP_ADDSMOOTH, XOP_BLENDDIFFUSEALPHA,
       XOP_BLENDCURRENTALPHA, XOP_BLENDTEXTUREALPHA, XOP_BLENDFACTORALPHA, XOP_BLENDTEXTUREALPHAPM,
       XOP_PREMODULATE, XOP_MODULATEALPHA_ADDCOLOR, XOP_MODULATECOLOR_ADDALPHA,
       XOP_MODULATEINVALPHA_ADDCOLOR, XOP_MODULATEINVCOLOR_ADDALPHA, XOP_DOTPRODUCT3, XOP_MULTIPLYADD,
       XOP_LERP, XOP_BUMPENVMAP, XOP_BUMPENVMAPLUMINANCE };

/* GLSL for a texture-stage argument (D3DTA: source in the low bits,
 * COMPLEMENT 0x10, ALPHAREPLICATE 0x20). */
static void ff_arg(Sb *s, uint32_t a, int st)
{
    static const char *const src[] = { "dif", "cur", "tx", "tf", "spe", "tmp" };
    char base[32];
    if ((a & 7) == 2)
        snprintf(base, sizeof base, "tx%d", st);
    else
        snprintf(base, sizeof base, "%s", (a & 7) < 6 ? src[a & 7] : "cur");
    if (a & 0x20) {
        if (a & 0x10) sb(s, "vec4(1.0 - %s.a)", base); else sb(s, "vec4(%s.a)", base);
    } else {
        if (a & 0x10) sb(s, "(1.0 - %s)", base); else sb(s, "%s", base);
    }
}

/* One stage operation on a0..a2, into `out` (a vec4 expression's channels). */
static void ff_op(Sb *s, uint32_t op, int st)
{
    switch (op) {
    case XOP_SELECTARG1: sb(s, "a1"); break;
    case XOP_SELECTARG2: sb(s, "a2"); break;
    case XOP_MODULATE: sb(s, "a1 * a2"); break;
    case XOP_MODULATE2X: sb(s, "a1 * a2 * 2.0"); break;
    case XOP_MODULATE4X: sb(s, "a1 * a2 * 4.0"); break;
    case XOP_ADD: sb(s, "a1 + a2"); break;
    case XOP_ADDSIGNED: sb(s, "a1 + a2 - 0.5"); break;
    case XOP_ADDSIGNED2X: sb(s, "(a1 + a2 - 0.5) * 2.0"); break;
    case XOP_SUBTRACT: sb(s, "a1 - a2"); break;
    case XOP_ADDSMOOTH: sb(s, "a1 + a2 - a1 * a2"); break;
    case XOP_BLENDDIFFUSEALPHA: sb(s, "mix(a2, a1, dif.a)"); break;
    case XOP_BLENDCURRENTALPHA: sb(s, "mix(a2, a1, cur.a)"); break;
    case XOP_BLENDTEXTUREALPHA: sb(s, "mix(a2, a1, tx%d.a)", st); break;
    case XOP_BLENDFACTORALPHA: sb(s, "mix(a2, a1, tf.a)"); break;
    case XOP_BLENDTEXTUREALPHAPM: sb(s, "a1 + a2 * (1.0 - tx%d.a)", st); break;
    case XOP_MODULATEALPHA_ADDCOLOR: sb(s, "a1 + a1.a * a2"); break;
    case XOP_MODULATECOLOR_ADDALPHA: sb(s, "a1 * a2 + a1.a"); break;
    case XOP_MODULATEINVALPHA_ADDCOLOR: sb(s, "(1.0 - a1.a) * a2 + a1"); break;
    case XOP_MODULATEINVCOLOR_ADDALPHA: sb(s, "(1.0 - a1) * a2 + a1.a"); break;
    case XOP_DOTPRODUCT3: sb(s, "vec4(clamp(dot((a1.rgb - 0.5) * 2.0, (a2.rgb - 0.5) * 2.0), 0.0, 1.0))"); break;
    case XOP_MULTIPLYADD: sb(s, "a0 + a1 * a2"); break;
    case XOP_LERP: sb(s, "mix(a2, a1, a0)"); break;
    /* ponytail: PREMODULATE and the bump ops pass the first argument;
     * bump mapping is not done. Said once, so a run shows if the title uses them. */
    default: {
        static int said;
        if (!said++)
            fprintf(stderr, "[GL] texture stage op %u not done (bump or premodulate)\n", op);
        sb(s, "a1");
        break;
    }
    }
}

static const char *const k_common =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n";

static void gen_vs(const Key *k, Sb *s)
{
    int i;
    const char *fl = k->flat ? "flat " : "";

    sb(s, "%s", k_common);
    /* WebGL2 promises 256 vertex uniform vectors: a program's 192 constants
     * and the fixed-function lights are never declared together. */
    sb(s, "uniform vec4 u_fog;\nuniform float u_psize;\n");
    if (k->vs)
        sb(s, "uniform vec4 u_c[192];\n");
    else
        sb(s, "uniform mat4 u_wv, u_proj, u_texm[4];\n"
              "uniform vec4 u_mat[5], u_amb, u_ld[8], u_ls[8], u_la[8], u_lp[8], u_ldir[8], u_latt[8], u_lspot[8];\n"
              "uniform vec4 u_rhw;\n");
    sb(s, "%sout vec4 v_d0;\n%sout vec4 v_d1;\nout vec4 v_t0, v_t1, v_t2, v_t3;\nout float v_fog, v_eyez;\n", fl, fl);
    if (k->vs) {
        /* The title's vertex program (d3d_hle_shared.h vsh_body), with the
         * GL clip conversion added. */
        for (i = 0; i < 16; i++)
            if (k->vs->inputs & (1u << i))
                sb(s, "in vec4 v%d;\n", i);
        /* oFog.x is a distance for table fog, and the factor itself with
         * FOGTABLEMODE NONE (Otogi writes 1 - z/far). A NaN factor reads as no
         * fog, as on the NV2A: the title leaves -inf and NaN fog constants
         * where it does not fog. */
        sb(s, "float nv_fog(float f) { float d = abs(f); return u_fog.x == 3.0 ? (u_fog.z - d) / (u_fog.z - u_fog.y)\n"
              "  : u_fog.x == 1.0 ? exp(-d * u_fog.w) : u_fog.x == 2.0 ? exp(-(d * u_fog.w) * (d * u_fog.w)) : f; }\n"
              "float rcc(float x) { float f = 1.0 / x; float m = clamp(abs(f), 5.42101e-20, 1.884467e19); return f < 0.0 ? -m : m; }\n"
              "vec4 nv_log(float x) { float a = abs(x); float e = floor(log2(a)); return a == 0.0 ? vec4(-1e38, 1.0, -1e38, 1.0) : vec4(e, a / exp2(e), log2(a), 1.0); }\n"
              "vec4 nv_lit(vec4 c) { float p = clamp(c.w, -127.9961, 127.9961); return vec4(1.0, max(c.x, 0.0), c.x > 0.0 ? pow(max(c.y, 0.0), p) : 0.0, 1.0); }\n"
              "#define c u_c\n"
              "void main() {\n"
              "  vec4 R0 = vec4(0.0), R1 = R0, R2 = R0, R3 = R0, R4 = R0, R5 = R0, R6 = R0, R7 = R0, R8 = R0, R9 = R0, R10 = R0, R11 = R0, R12 = R0;\n"
              "  vec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = oD0, oFog = vec4(0.0);\n"
              "  vec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = oT0, oT2 = oT0, oT3 = oT0;\n"
              "  int a0 = 0;\n");
        sb(s, "%s", k->vs->glsl);
        sb(s, "  vec4 p = %s;\n", k->vs->uses_vp ? "vec4(R12.xyz * R12.w, R12.w)" : "R12");
        sb(s, "  gl_Position = vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w);\n"
              "  v_d0 = clamp(oD0, 0.0, 1.0); v_d1 = clamp(oD1, 0.0, 1.0);\n"
              "  v_fog = nv_fog(oFog.x); v_fog = isnan(v_fog) ? 1.0 : clamp(v_fog, 0.0, 1.0); v_eyez = 0.0;\n"
              "  v_t0 = oT0; v_t1 = oT1; v_t2 = oT2; v_t3 = oT3;\n"
              "  gl_PointSize = u_psize;\n}\n");
        return;
    }
    sb(s, "in vec4 a_pos;\n");
    if (k->normal) sb(s, "in vec3 a_normal;\n");
    if (k->diffuse) sb(s, "in vec4 a_diffuse;\n");
    if (k->specular) sb(s, "in vec4 a_specular;\n");
    for (i = 0; i < k->ntc; i++)
        sb(s, "in vec4 a_tc%d;\n", i);
    sb(s, "void main() {\n");
    sb(s, "  vec4 dif_in = %s, spe_in = %s;\n",
       k->diffuse ? "a_diffuse.zyxw" : "vec4(1.0)", k->specular ? "a_specular.zyxw" : "vec4(0.0)");
    if (k->rhw) {
        /* Pre-transformed: guest pixels (centres on integers) to clip space. */
        sb(s, "  float w = a_pos.w != 0.0 ? 1.0 / a_pos.w : 1.0;\n"
              "  vec2 ndc = (a_pos.xy + 0.5 - u_rhw.xy) / u_rhw.zw * 2.0 - 1.0;\n"
              "  gl_Position = vec4(ndc * w, (2.0 * a_pos.z - 1.0) * w, w);\n"
              "  vec3 P = vec3(0.0), N = vec3(0.0, 0.0, 1.0);\n"
              "  v_eyez = w;\n");
    } else {
        sb(s, "  vec4 eye = u_wv * vec4(a_pos.xyz, 1.0);\n"
              "  vec4 p = u_proj * eye;\n"
              "  gl_Position = vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w);\n"
              "  vec3 P = eye.xyz;\n"
              "  v_eyez = abs(eye.z);\n");
        sb(s, "  vec3 N = %s;\n", k->normal ? (k->normalize ? "normalize(mat3(u_wv) * a_normal)" : "mat3(u_wv) * a_normal")
                                            : "vec3(0.0, 0.0, 1.0)");
    }
    if (k->lighting && !k->rhw) {
        static const char *const srcs[3] = { "", "dif_in", "spe_in" };
        sb(s, "  vec4 md = %s, ma = %s, ms = %s, me = %s;\n",
           k->mat_src[0] ? srcs[k->mat_src[0]] : "u_mat[0]", k->mat_src[1] ? srcs[k->mat_src[1]] : "u_mat[1]",
           k->mat_src[2] ? srcs[k->mat_src[2]] : "u_mat[2]", k->mat_src[3] ? srcs[k->mat_src[3]] : "u_mat[3]");
        sb(s, "  vec3 amb = u_amb.rgb, dsum = vec3(0.0), ssum = vec3(0.0);\n"
              "  vec3 V = %s;\n", k->local_viewer ? "-normalize(P)" : "vec3(0.0, 0.0, -1.0)");
        for (i = 0; i < 8; i++) {
            if (!k->light_type[i])
                continue;
            sb(s, "  {\n");
            if (k->light_type[i] == 3) {
                sb(s, "    vec3 L = -u_ldir[%d].xyz; float att = 1.0;\n", i);
            } else {
                sb(s, "    vec3 d = u_lp[%d].xyz - P; float dist = length(d); vec3 L = d / max(dist, 1e-6);\n"
                      "    float att = dist > u_lp[%d].w ? 0.0 : 1.0 / max(u_latt[%d].x + u_latt[%d].y * dist + u_latt[%d].z * dist * dist, 1e-6);\n",
                   i, i, i, i, i);
                if (k->light_type[i] == 2)
                    sb(s, "    float rho = dot(-L, u_ldir[%d].xyz);\n"
                          "    att *= rho > u_lspot[%d].x ? 1.0 : rho <= u_lspot[%d].y ? 0.0\n"
                          "         : pow((rho - u_lspot[%d].y) / max(u_lspot[%d].x - u_lspot[%d].y, 1e-6), u_ldir[%d].w);\n",
                       i, i, i, i, i, i, i);
            }
            sb(s, "    amb += att * u_la[%d].rgb;\n"
                  "    float nl = max(dot(N, L), 0.0);\n"
                  "    dsum += att * nl * u_ld[%d].rgb;\n"
                  "    if (nl > 0.0) ssum += att * pow(max(dot(N, normalize(L + V)), 0.0), u_mat[4].x) * u_ls[%d].rgb;\n"
                  "  }\n", i, i, i);
        }
        sb(s, "  v_d0 = vec4(clamp(me.rgb + ma.rgb * amb + md.rgb * dsum, 0.0, 1.0), md.a);\n"
              "  v_d1 = vec4(clamp(ms.rgb * ssum, 0.0, 1.0), spe_in.a);\n");
    } else {
        sb(s, "  v_d0 = dif_in; v_d1 = spe_in;\n");
    }
    for (i = 0; i < 4; i++) {
        const char *in;
        char buf[64];
        if (k->tc_gen[i] == 1) in = "vec4(N, 1.0)";
        else if (k->tc_gen[i] == 2) in = "vec4(P, 1.0)";
        else if (k->tc_gen[i] == 3) in = "vec4(reflect(normalize(P), N), 1.0)";
        else if (k->tc_set[i] < k->ntc) {
            /* D3D pads a 2D coordinate as (u, v, 1, 0) for its texture
             * matrix, so a 2D transform's translation is in the third row. */
            int n = k->tc_size[k->tc_set[i]];
            snprintf(buf, sizeof buf, n == 1 ? "vec4(a_tc%d.x, 1.0, 0.0, 0.0)" : n == 2 ? "vec4(a_tc%d.xy, 1.0, 0.0)"
                     : n == 3 ? "vec4(a_tc%d.xyz, 1.0)" : "a_tc%d", k->tc_set[i]);
            in = buf;
        } else in = "vec4(0.0, 0.0, 0.0, 1.0)";
        if (k->tc_count[i])
            sb(s, "  v_t%d = u_texm[%d] * %s;\n", i, i, in);
        else
            sb(s, "  v_t%d = %s;\n", i, in);
    }
    if (k->fog && !k->fog_table)
        sb(s, "  v_fog = spe_in.a;\n");
    else
        sb(s, "  v_fog = 1.0;\n");
    sb(s, "  gl_PointSize = u_psize;\n}\n");
}

/* -- Pixel shaders: NV2A register combiners to GLSL ---------------------------
 *
 * The D3DPIXELSHADERDEF words are the NV2A's own combiner words; SetPixelShader
 * copies them into D3D_g_RenderState[0..56] and the texture modes into
 * PSTEXTUREMODES. Semantics as in the toolkit's CPU model
 * (src/kernel/nv2a_combiner.c, after xemu's psh.c); the generator follows
 * Otogi XUnchain's gl_hle.zig (MIT, Copyright (c) 2026 The Otogi XUnchain
 * contributors). Registers: 0 zero, 1 c0, 2 c1, 3 fog, 4 v0, 5 v1, 8-11
 * t0-t3, 12 r0, 13 r1, 14 v1+r0 sum and 15 E*F (final combiner only). */

/* An input byte: reg | alpha << 4 | mapping << 5, as RGB (vec3) or alpha (float). */
static void ps_in(Sb *s, uint32_t byte, int alpha_portion)
{
    char x[32];
    snprintf(x, sizeof x, "R[%u]%s", byte & 0xF,
             alpha_portion ? ((byte & 0x10) ? ".a" : ".b") : ((byte & 0x10) ? ".aaa" : ".rgb"));
    switch ((byte >> 5) & 7) {
    case 0: sb(s, "max(%s, 0.0)", x); break;                         /* unsigned identity */
    case 1: sb(s, "(1.0 - clamp(%s, 0.0, 1.0))", x); break;          /* unsigned invert */
    case 2: sb(s, "(2.0 * max(%s, 0.0) - 1.0)", x); break;           /* expand normal */
    case 3: sb(s, "(-2.0 * max(%s, 0.0) + 1.0)", x); break;          /* expand negate */
    case 4: sb(s, "(max(%s, 0.0) - 0.5)", x); break;                 /* half-bias normal */
    case 5: sb(s, "(-max(%s, 0.0) + 0.5)", x); break;                /* half-bias negate */
    case 6: sb(s, "%s", x); break;                                   /* signed identity */
    default: sb(s, "(-%s)", x); break;                               /* signed negate */
    }
}

/* Output scale and bias, applied to `x`. */
static const char *ps_out(uint32_t op)
{
    switch (op) {
    case 0x08: return "(x - 0.5)";
    case 0x10: return "(x * 2.0)";
    case 0x18: return "((x - 0.5) * 2.0)";
    case 0x20: return "(x * 4.0)";
    case 0x30: return "(x * 0.5)";
    }
    return "x";
}

static void gen_ps(const Key *k, Sb *s)
{
    static const char names[] = "ABCD";
    uint32_t st, i, n = k->ps_ctl & 0xFF, flags = k->ps_ctl >> 8;

    sb(s, "  vec4 R[16];\n  for (int i = 0; i < 16; i++) R[i] = vec4(0.0);\n"
          "  R[3] = vec4(u_fogcolor.rgb, v_fog); R[4] = v_d0; R[5] = v_d1;\n");
    for (st = 0; st < 4; st++) {
        uint32_t mode = (k->ps_modes >> (5 * st)) & 0x1F, reg = 8 + st;
        if (mode == 0)
            continue;                                               /* NONE: zero */
        if (mode == 4)                                              /* PASSTHRU */
            sb(s, "  R[%u] = clamp(v_t%u, 0.0, 1.0);\n", reg, st);
        else if (k->tex[st] == 2)
            sb(s, "  R[%u] = texture(u_tex%u, v_t%u.xyz);\n", reg, st, st);
        else if (k->tex[st]) {
            if (mode != 1) {                                        /* 3D, bump, dot modes: as 2D */
                static uint32_t said;
                if (!(said & (1u << mode)))
                    fprintf(stderr, "[GL] pixel shader texture mode 0x%X (stage %u) sampled as 2D\n", mode, st);
                said |= 1u << mode;
            }
            sb(s, "  R[%u] = texture(u_tex%u, v_t%u.xy / (v_t%u.w != 0.0 ? v_t%u.w : 1.0) * u_tscale[%u].xy);\n",
               reg, st, st, st, st, st);
        } else
            sb(s, "  R[%u] = vec4(1.0);\n", reg);
    }
    sb(s, (k->ps_modes & 0x1F) ? "  R[12].a = R[8].a;\n" : "  R[12].a = 1.0;\n");

    for (i = 0; i < n && i < 8; i++) {
        uint32_t icw = k->ps_ri[i], ocw = k->ps_ro[i], aicw = k->ps_ai[i], aocw = k->ps_ao[i];
        uint32_t fl = ocw >> 12, afl = aocw >> 12, j;
        const char *o = ps_out(fl & 0x38), *ao = ps_out(afl & 0x38);

        sb(s, "  { R[1] = u_psc[%u]; R[2] = u_psc[%u];\n", (flags & 0x010) ? i : 0, 8 + ((flags & 0x100) ? i : 0));
        sb(s, (flags & 1) ? "    bool mux = R[12].a >= 0.5;\n" : "    bool mux = (int(R[12].a * 255.0) & 1) != 0;\n");
        /* Both portions read before either writes. */
        for (j = 0; j < 4; j++) {
            uint32_t sh = 24 - 8 * j;
            sb(s, "    vec3 %c = ", names[j]); ps_in(s, (icw >> sh) & 0xFF, 0);
            sb(s, "; float a%c = ", names[j]); ps_in(s, (aicw >> sh) & 0xFF, 1);
            sb(s, ";\n");
        }
        sb(s, "    vec3 pab = %s; vec3 pcd = %s;\n", (fl & 2) ? "vec3(dot(A, B))" : "A * B",
           (fl & 1) ? "vec3(dot(C, D))" : "C * D");
        sb(s, "    vec3 psum = %s; float qab = aA * aB, qcd = aC * aD, qsum = %s;\n",
           (fl & 4) ? "mux ? pcd : pab" : "pab + pcd", (afl & 4) ? "mux ? qcd : qab" : "qab + qcd");
        sb(s, "    vec3 x, ab, cd, ms; float aab, acd, ams;\n"
              "    x = pab; ab = clamp(%s, -1.0, 1.0); x = pcd; cd = clamp(%s, -1.0, 1.0); x = psum; ms = clamp(%s, -1.0, 1.0);\n",
           o, o, o);
        sb(s, "    { float x = qab; aab = clamp(%s, -1.0, 1.0); } { float x = qcd; acd = clamp(%s, -1.0, 1.0); }"
              " { float x = qsum; ams = clamp(%s, -1.0, 1.0); }\n", ao, ao, ao);
        /* RGB destinations; blue-to-alpha also writes the destination's alpha. */
        if ((ocw >> 4) & 0xF) sb(s, "    R[%u].rgb = ab;\n", (ocw >> 4) & 0xF);
        if (((ocw >> 4) & 0xF) && (fl & 0x80)) sb(s, "    R[%u].a = ab.b;\n", (ocw >> 4) & 0xF);
        if (ocw & 0xF) sb(s, "    R[%u].rgb = cd;\n", ocw & 0xF);
        if ((ocw & 0xF) && (fl & 0x40)) sb(s, "    R[%u].a = cd.b;\n", ocw & 0xF);
        if ((ocw >> 8) & 0xF) sb(s, "    R[%u].rgb = ms;\n", (ocw >> 8) & 0xF);
        if ((aocw >> 4) & 0xF) sb(s, "    R[%u].a = aab;\n", (aocw >> 4) & 0xF);
        if (aocw & 0xF) sb(s, "    R[%u].a = acd;\n", aocw & 0xF);
        if ((aocw >> 8) & 0xF) sb(s, "    R[%u].a = ams;\n", (aocw >> 8) & 0xF);
        sb(s, "    R[0] = vec4(0.0);\n  }\n");
    }

    if (k->ps_f0 || k->ps_f1) {
        uint32_t ff = k->ps_f1 & 0xFF;
        sb(s, "  R[1] = u_psf[0]; R[2] = u_psf[1];\n");
        /* V1R0 sum: optional complements (0x40 v1, 0x20 r0), clamp (0x80). */
        sb(s, "  R[14] = vec4(%s%s + %s%s, 0.0);\n", (ff & 0x80) ? "clamp(" : "",
           (ff & 0x40) ? "(1.0 - R[5].rgb)" : "R[5].rgb", (ff & 0x20) ? "(1.0 - R[12].rgb)" : "R[12].rgb",
           (ff & 0x80) ? ", 0.0, 1.0)" : "");
        sb(s, "  { vec3 E = "); ps_in(s, (k->ps_f1 >> 24) & 0xFF, 0);
        sb(s, "; vec3 F = "); ps_in(s, (k->ps_f1 >> 16) & 0xFF, 0);
        sb(s, "; R[15] = vec4(E * F, 0.0); }\n  { vec3 A = "); ps_in(s, (k->ps_f0 >> 24) & 0xFF, 0);
        sb(s, "; vec3 B = "); ps_in(s, (k->ps_f0 >> 16) & 0xFF, 0);
        sb(s, "; vec3 C = "); ps_in(s, (k->ps_f0 >> 8) & 0xFF, 0);
        sb(s, "; vec3 D = "); ps_in(s, k->ps_f0 & 0xFF, 0);
        sb(s, ";\n    cur.rgb = D + C * (1.0 - A) + B * A; cur.a = "); ps_in(s, (k->ps_f1 >> 8) & 0xFF, 1);
        sb(s, "; }\n");
    } else {
        /* No final combiner: D3D supplies one, which fogs r0 when FOGENABLE is set. */
        sb(s, k->fog ? "  cur = vec4(mix(R[3].rgb, R[12].rgb, R[3].a), R[12].a);\n" : "  cur = R[12];\n");
    }
    sb(s, "  cur = clamp(cur, 0.0, 1.0);\n");
}

static void gen_fs(const Key *k, Sb *s)
{
    int st;
    const char *fl = k->flat ? "flat " : "";

    sb(s, "%s", k_common);
    sb(s, "%sin vec4 v_d0;\n%sin vec4 v_d1;\nin vec4 v_t0, v_t1, v_t2, v_t3;\nin float v_fog, v_eyez;\n", fl, fl);
    sb(s, "uniform vec4 u_tf, u_fog, u_fogcolor, u_tscale[4];\nuniform float u_aref;\n");
    for (st = 0; st < 4; st++)
        if (k->tex[st])
            sb(s, "uniform %s u_tex%d;\n", k->tex[st] == 2 ? "samplerCube" : "sampler2D", st);
    if (k->ps) {
        sb(s, "uniform vec4 u_psc[16], u_psf[2];\nout vec4 o_color;\nvoid main() {\n  vec4 cur;\n");
        gen_wbuf(k, s);
        gen_ps(k, s);
        goto alpha_test;
    }
    sb(s, "out vec4 o_color;\n"
          "void main() {\n"
          "  vec4 dif = v_d0, spe = v_d1, tf = u_tf, cur = dif, tmp = vec4(0.0), a0, a1, a2;\n");
    gen_wbuf(k, s);
    for (st = 0; st < 4; st++) {
        if (k->tex[st] == 2)
            sb(s, "  vec4 tx%d = texture(u_tex%d, v_t%d.xyz);\n", st, st, st);
        else if (k->tex[st] && k->tc_proj[st])
            sb(s, "  vec4 tx%d = texture(u_tex%d, v_t%d.xy / v_t%d.%c * u_tscale[%d].xy);\n", st, st, st, st,
               k->tc_count[st] == 3 ? 'z' : k->tc_count[st] == 2 ? 'y' : 'w', st);
        else if (k->tex[st])
            sb(s, "  vec4 tx%d = texture(u_tex%d, v_t%d.xy * u_tscale[%d].xy);\n", st, st, st, st);
        else
            sb(s, "  vec4 tx%d = vec4(1.0);\n", st);
    }
    for (st = 0; st < 4; st++) {
        if (k->colorop[st] == XOP_DISABLE || !k->colorop[st])
            break;
        sb(s, "  a0 = "); ff_arg(s, k->colorarg[st][0], st);
        sb(s, "; a1 = "); ff_arg(s, k->colorarg[st][1], st);
        sb(s, "; a2 = "); ff_arg(s, k->colorarg[st][2], st);
        sb(s, ";\n  vec3 c%d = clamp((", st); ff_op(s, k->colorop[st], st); sb(s, ").rgb, 0.0, 1.0);\n");
        if (k->alphaop[st] == XOP_DISABLE || !k->alphaop[st]) {
            sb(s, "  float x%d = cur.a;\n", st);
        } else {
            sb(s, "  a0 = "); ff_arg(s, k->alphaarg[st][0], st);
            sb(s, "; a1 = "); ff_arg(s, k->alphaarg[st][1], st);
            sb(s, "; a2 = "); ff_arg(s, k->alphaarg[st][2], st);
            sb(s, ";\n  float x%d = clamp((", st); ff_op(s, k->alphaop[st], st); sb(s, ").a, 0.0, 1.0);\n");
        }
        sb(s, "  %s = vec4(c%d, x%d);\n", k->result_temp[st] ? "tmp" : "cur", st, st);
    }
    if (k->spec_add)
        sb(s, "  cur.rgb = clamp(cur.rgb + spe.rgb, 0.0, 1.0);\n");
    if (k->fog) {
        if (k->fog_table)
            sb(s, "  float f = u_fog.x == 3.0 ? (u_fog.z - v_eyez) / (u_fog.z - u_fog.y)\n"
                  "          : u_fog.x == 1.0 ? exp(-v_eyez * u_fog.w) : exp(-(v_eyez * u_fog.w) * (v_eyez * u_fog.w));\n");
        else
            sb(s, "  float f = v_fog;\n");
        sb(s, "  cur.rgb = mix(u_fogcolor.rgb, cur.rgb, clamp(f, 0.0, 1.0));\n");
    }
alpha_test:
    switch (k->view) {
    case VIEW_NORMAL: break;
    case VIEW_COVERAGE: sb(s, "  o_color = vec4(1.0, 0.0, 1.0, 1.0);\n}\n"); return;
    case VIEW_TEXTURE0:
        sb(s, k->tex[0] == 1 ? "  o_color = vec4(texture(u_tex0, v_t0.xy * u_tscale[0].xy).rgb, 1.0);\n}\n"
                             : "  o_color = vec4(1.0, 1.0, 0.0, 1.0);\n}\n");
        return;
    case VIEW_DIFFUSE: sb(s, "  o_color = vec4(v_d0.rgb, 1.0);\n}\n"); return;
    case VIEW_COLOUR: sb(s, "  o_color = vec4(cur.rgb, 1.0);\n}\n"); return;
    default: sb(s, "  o_color = vec4(vec3(cur.a), 1.0);\n}\n"); return;
    }
    if (k->alpha_test) {
        /* GL_NEVER 0x200 .. GL_ALWAYS 0x207, as the Xbox stores them. */
        static const char *const cmp[8] = { "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        int f = k->alpha_func & 7;
        if (f == 0) sb(s, "  discard;\n");
        else if (f != 7) sb(s, "  if (!(floor(cur.a * 255.0 + 0.5) %s u_aref)) discard;\n", cmp[f]);
    }
    sb(s, "  o_color = cur;\n}\n");
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "[GL] %s shader does not compile:\n%s\n--- source ---\n%s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log, src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static char s_src[256 * 1024], s_src_fs[256 * 1024];

/* -- Programs cached on disk ---------------------------------------------- */

/* Compiling a program costs tens of milliseconds on ANGLE (GLSL, then HLSL,
 * then D3D bytecode), and a stage meets twenty at once when it starts. Each
 * linked program is saved (glGetProgramBinary) under the hash of its two
 * sources, and loaded from there the next time the same sources come up. A
 * binary the driver no longer takes (another ANGLE, another GPU driver) fails
 * to load and the program is compiled and saved again.
 *
 * RECOMP_GL_CACHE=<dir> sets the folder (default: shader_cache next to the
 * exe); RECOMP_GL_CACHE=0 turns the cache off. */

#define PROG_CACHE_MAGIC 0x31475450u           /* "PTG1" */

static const char *prog_cache_dir(void)
{
    static char dir[MAX_PATH];
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_GL_CACHE");
        GLint formats = 0;
        init = 1;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
        if (formats <= 0)
            fprintf(stderr, "[GL] no program binary format: programs are not cached\n");
        else if (e && strcmp(e, "0") != 0)
            snprintf(dir, sizeof dir, "%s", e);
        else if (!e && GetModuleFileNameA(NULL, dir, sizeof dir)) {
            char *slash = strrchr(dir, '\\');
            if (slash && (size_t)(slash + 1 - dir) + sizeof "shader_cache" <= sizeof dir)
                strcpy(slash + 1, "shader_cache");
            else
                dir[0] = 0;
        }
        if (dir[0]) {
            CreateDirectoryA(dir, NULL);
            fprintf(stderr, "[GL] program cache in %s\n", dir);
        }
    }
    return dir[0] ? dir : NULL;
}

static uint64_t fnv64(uint64_t h, const char *s)
{
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ull;
    }
    return h;
}

static void prog_cache_path(char *path, size_t n, const char *dir, uint64_t h, const char *ext)
{
    snprintf(path, n, "%s\\%016llx.%s", dir, (unsigned long long)h, ext);
}

/* Links prog from the cached binary; 0 if there is none or the driver
 * refuses it. */
static int prog_cache_load(GLuint prog, uint64_t h)
{
    const char *dir = prog_cache_dir();
    char path[MAX_PATH + 32];
    uint32_t hdr[3];
    void *buf;
    FILE *f;
    GLint ok = 0;

    if (!dir)
        return 0;
    prog_cache_path(path, sizeof path, dir, h, "bin");
    if ((f = fopen(path, "rb")) == NULL)
        return 0;
    if (fread(hdr, 4, 3, f) != 3 || hdr[0] != PROG_CACHE_MAGIC || !hdr[2] || hdr[2] > (64u << 20)
        || (buf = malloc(hdr[2])) == NULL) {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, hdr[2], f) == hdr[2]) {
        glProgramBinary(prog, (GLenum)hdr[1], buf, (GLsizei)hdr[2]);
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    }
    free(buf);
    fclose(f);
    return ok;
}

static void prog_cache_save(GLuint prog, uint64_t h)
{
    const char *dir = prog_cache_dir();
    char path[MAX_PATH + 32], tmp[MAX_PATH + 32];
    GLint len = 0;
    GLenum format = 0;
    uint32_t hdr[3];
    void *buf;
    FILE *f;

    if (!dir)
        return;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || (buf = malloc((size_t)len)) == NULL)
        return;
    glGetProgramBinary(prog, len, &len, &format, buf);
    prog_cache_path(path, sizeof path, dir, h, "bin");
    prog_cache_path(tmp, sizeof tmp, dir, h, "tmp");
    if (len > 0 && (f = fopen(tmp, "wb")) != NULL) {
        int good;
        hdr[0] = PROG_CACHE_MAGIC;
        hdr[1] = (uint32_t)format;
        hdr[2] = (uint32_t)len;
        good = fwrite(hdr, 4, 3, f) == 3 && fwrite(buf, 1, (size_t)len, f) == (size_t)len;
        good = fclose(f) == 0 && good;
        /* Written whole, then renamed: a crash never leaves half a binary. */
        if (!good || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
            DeleteFileA(tmp);
    }
    free(buf);
}

static Prog *prog_get(const Key *k)
{
    static const char *const attr[] = { "a_pos", "a_normal", "a_diffuse", "a_specular", "a_psize",
                                        "a_tc0", "a_tc1", "a_tc2", "a_tc3", "a_tc4", "a_tc5", "a_tc6", "a_tc7" };
    Prog *p;
    GLuint vs, fs;
    GLint ok = 0;
    Sb s = { s_src, sizeof s_src, 0 }, sf = { s_src_fs, sizeof s_src_fs, 0 };
    uint64_t h;
    int i;
    char name[32];

    for (i = 0; i < s_nprog; i++)
        if (!memcmp(&s_prog[i].key, k, sizeof *k))
            return s_prog[i].prog ? &s_prog[i] : NULL;
    if (s_nprog == MAX_PROG)
        return NULL;
    p = &s_prog[s_nprog++];
    memset(p, 0, sizeof *p);
    p->key = *k;
    s_src[0] = s_src_fs[0] = 0;
    gen_vs(k, &s);
    gen_fs(k, &sf);
    {
        const char *dump = getenv("RECOMP_GL_DUMP_SHADERS");
        if (dump) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s/prog_%04d.vert", dump, s_nprog - 1);
            if ((f = fopen(path, "w")) != NULL) { fputs(s_src, f); fclose(f); }
            snprintf(path, sizeof path, "%s/prog_%04d.frag", dump, s_nprog - 1);
            if ((f = fopen(path, "w")) != NULL) { fputs(s_src_fs, f); fclose(f); }
        }
    }
    /* The sources and the attribute bindings name the program. */
    h = fnv64(fnv64(fnv64(14695981039346656037ull, s_src), "\n--\n"), s_src_fs);
    h = fnv64(h, k->vs ? "\n--v" : "\n--ff");
    p->prog = glCreateProgram();
    if (prog_cache_load(p->prog, h)) {
        s_st.prog_loads++;
    } else {
        s_st.vs_compiles++;
        vs = compile(GL_VERTEX_SHADER, s_src);
        fs = compile(GL_FRAGMENT_SHADER, s_src_fs);
        if (!vs || !fs) {
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            glDeleteProgram(p->prog);
            p->prog = 0;
            return NULL;
        }
        glAttachShader(p->prog, vs);
        glAttachShader(p->prog, fs);
        if (k->vs) {
            for (i = 0; i < 16; i++) {
                snprintf(name, sizeof name, "v%d", i);
                glBindAttribLocation(p->prog, (GLuint)i, name);
            }
        } else {
            for (i = 0; i < (int)(sizeof attr / sizeof attr[0]); i++)
                glBindAttribLocation(p->prog, (GLuint)i, attr[i]);
        }
        glProgramParameteri(p->prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glLinkProgram(p->prog);
        glDetachShader(p->prog, vs);
        glDetachShader(p->prog, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
            fprintf(stderr, "[GL] program does not link: %s\n", log);
            glDeleteProgram(p->prog);
            p->prog = 0;
            return NULL;
        }
        prog_cache_save(p->prog, h);
    }
#define U(f, n) p->f = glGetUniformLocation(p->prog, n)
    U(u_wv, "u_wv"); U(u_proj, "u_proj"); U(u_texm, "u_texm"); U(u_mat, "u_mat"); U(u_amb, "u_amb");
    U(u_ld, "u_ld"); U(u_ls, "u_ls"); U(u_la, "u_la"); U(u_lp, "u_lp"); U(u_ldir, "u_ldir");
    U(u_latt, "u_latt"); U(u_lspot, "u_lspot"); U(u_rhw, "u_rhw"); U(u_fog, "u_fog");
    U(u_fogcolor, "u_fogcolor"); U(u_tf, "u_tf"); U(u_aref, "u_aref"); U(u_tscale, "u_tscale");
    U(u_c, "u_c"); U(u_psize, "u_psize"); U(u_psc, "u_psc"); U(u_psf, "u_psf");
#undef U
    glUseProgram(p->prog);
    for (i = 0; i < 4; i++) {
        snprintf(name, sizeof name, "u_tex%d", i);
        p->u_tex[i] = glGetUniformLocation(p->prog, name);
        if (p->u_tex[i] >= 0)
            glUniform1i(p->u_tex[i], i);
    }
    return p;
}

/* -- Per-draw state ------------------------------------------------------ */

static void set_cap(GLenum cap, int on) { if (on) glEnable(cap); else glDisable(cap); }

static void apply_render_state(void)
{
    uint32_t cull = xb_rs(XRS_CULLMODE), cw = xb_rs(XRS_COLORWRITEENABLE);
    int depth = s_cur_ds != NULL;

    set_cap(GL_DEPTH_TEST, depth && xb_rs(XRS_ZENABLE));
    glDepthFunc(xb_rs(XRS_ZFUNC) >= 0x200 && xb_rs(XRS_ZFUNC) <= 0x207 ? xb_rs(XRS_ZFUNC) : GL_ALWAYS);
    glDepthMask(xb_rs(XRS_ZWRITEENABLE) ? GL_TRUE : GL_FALSE);
    set_cap(GL_BLEND, xb_rs(XRS_ALPHABLENDENABLE) != 0);
    glBlendFunc(xb_rs(XRS_SRCBLEND), xb_rs(XRS_DESTBLEND));
    glBlendEquation(xb_rs(XRS_BLENDOP) ? xb_rs(XRS_BLENDOP) : GL_FUNC_ADD);
    {
        float bc[4];
        argb_vec(xb_rs(XRS_BLENDCOLOR), bc);
        glBlendColor(bc[0], bc[1], bc[2], bc[3]);
    }
    glColorMask((cw >> 16) & 1, (cw >> 8) & 1, cw & 1, (cw >> 24) & 1);
    set_cap(GL_STENCIL_TEST, depth && xb_rs(XRS_STENCILENABLE));
    glStencilFunc(xb_rs(XRS_STENCILFUNC) >= 0x200 ? xb_rs(XRS_STENCILFUNC) : GL_ALWAYS,
                  (GLint)xb_rs(XRS_STENCILREF), xb_rs(XRS_STENCILMASK));
    glStencilMask(xb_rs(XRS_STENCILWRITEMASK));
    glStencilOp(xb_rs(XRS_STENCILFAIL) ? xb_rs(XRS_STENCILFAIL) : GL_ZERO,
                xb_rs(XRS_STENCILZFAIL) ? xb_rs(XRS_STENCILZFAIL) : GL_ZERO,
                xb_rs(XRS_STENCILPASS) ? xb_rs(XRS_STENCILPASS) : GL_ZERO);
    /* A D3D clockwise triangle is counter-clockwise in GL's (unflipped)
     * window space: see the top of the file. */
    set_cap(GL_CULL_FACE, cull == 0x900 || cull == 0x901);
    glCullFace(GL_BACK);
    glFrontFace(cull == 0x900 ? GL_CW : GL_CCW);
    {
        int solid = xb_rs(XRS_SOLIDOFFSETENABLE) != 0;
        float slope = solid ? f32(xb_rs(XRS_POLYGONOFFSETZSLOPESCALE)) : 0.0f;
        float units = solid ? f32(xb_rs(XRS_POLYGONOFFSETZOFFSET)) : 0.0f;
        /* ZBIAS (0..16, larger is nearer) on top, in 2^-24 steps.
         * ponytail: 256 steps per ZBIAS unit is a guess; tune against xemu. */
        units -= (float)xb_rs(XRS_ZBIAS) * 256.0f;
        set_cap(GL_POLYGON_OFFSET_FILL, slope != 0.0f || units != 0.0f);
        glPolygonOffset(slope, units);
    }
    glDisable(GL_SCISSOR_TEST);
}

static GLint gl_wrap(uint32_t v)
{
    return v == 1 ? GL_REPEAT : v == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
}

/* A copy of the bound colour target, for a draw that samples the target it
 * draws into: the NV2A reads the texture as it was, GL leaves a feedback loop
 * undefined (black on ANGLE). */
static GLuint s_fb_copy;
static uint32_t s_fb_copy_w, s_fb_copy_h;

static GLuint rt_feedback_copy(HostRt *r)
{
    GLint w = (GLint)(r->w * s_scale), h = (GLint)(r->h * s_scale);
    uint32_t cw = xb_rs(XRS_COLORWRITEENABLE);

    if (!s_fb_copy || s_fb_copy_w != r->w || s_fb_copy_h != r->h) {
        if (!s_fb_copy) glGenTextures(1, &s_fb_copy);
        glBindTexture(GL_TEXTURE_2D, s_fb_copy);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        s_fb_copy_w = r->w;
        s_fb_copy_h = r->h;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo_read);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fb_copy, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glColorMask((cw >> 16) & 1, (cw >> 8) & 1, cw & 1, (cw >> 24) & 1);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    return s_fb_copy;
}

static void apply_stages(Key *k)
{
    int st, copied = 0;
    for (st = 0; st < 4; st++) {
        GLuint t = 0;
        float lw = 0, lh = 0;
        int cube = 0;
        uint32_t mag = xb_tss(st, XTSS_MAGFILTER), min = xb_tss(st, XTSS_MINFILTER), mip = xb_tss(st, XTSS_MIPFILTER);

        if (s_stage_tex[st])
            t = tex_get(s_stage_tex[st], s_stage_pal[st], &lw, &lh, &cube);
        if (t && s_cur_rt && t == s_cur_rt->tex) {
            t = copied ? s_fb_copy : rt_feedback_copy(s_cur_rt);
            copied = 1;
        }
        s_lin[st][0] = lw ? 1.0f / lw : 0;
        s_lin[st][1] = lh ? 1.0f / lh : 0;
        s_tex_cube[st] = cube;
        k->tex[st] = t ? (cube ? 2 : 1) : 0;
        glActiveTexture(GL_TEXTURE0 + (GLenum)st);
        glBindTexture(cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, t);
        glBindSampler((GLuint)st, s_samp[st]);
        glSamplerParameteri(s_samp[st], GL_TEXTURE_WRAP_S, gl_wrap(xb_tss(st, XTSS_ADDRESSU)));
        glSamplerParameteri(s_samp[st], GL_TEXTURE_WRAP_T, gl_wrap(xb_tss(st, XTSS_ADDRESSV)));
        glSamplerParameteri(s_samp[st], GL_TEXTURE_WRAP_R, gl_wrap(xb_tss(st, XTSS_ADDRESSW)));
        glSamplerParameteri(s_samp[st], GL_TEXTURE_MAG_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
        glSamplerParameteri(s_samp[st], GL_TEXTURE_MIN_FILTER,
                            mip == 0 ? (min == 1 ? GL_NEAREST : GL_LINEAR)
                            : mip == 1 ? (min == 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST)
                            : (min == 1 ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR));
        glSamplerParameterf(s_samp[st], GL_TEXTURE_MIN_LOD, (float)xb_tss(st, XTSS_MAXMIPLEVEL));
        k->colorop[st] = (uint8_t)xb_tss(st, XTSS_COLOROP);
        k->alphaop[st] = (uint8_t)xb_tss(st, XTSS_ALPHAOP);
        k->colorarg[st][0] = (uint8_t)xb_tss(st, XTSS_COLORARG0);
        k->colorarg[st][1] = (uint8_t)xb_tss(st, XTSS_COLORARG1);
        k->colorarg[st][2] = (uint8_t)xb_tss(st, XTSS_COLORARG2);
        k->alphaarg[st][0] = (uint8_t)xb_tss(st, XTSS_ALPHAARG0);
        k->alphaarg[st][1] = (uint8_t)xb_tss(st, XTSS_ALPHAARG1);
        k->alphaarg[st][2] = (uint8_t)xb_tss(st, XTSS_ALPHAARG2);
        k->result_temp[st] = xb_tss(st, XTSS_RESULTARG) == 5;
    }
    glActiveTexture(GL_TEXTURE0);
}

static void key_vertex(Key *k)
{
    uint32_t fvf = s_vshader, ntc = (fvf >> 8) & 0xF, i;

    if (k->vs)
        return;
    k->rhw = (fvf & 0x00E) == 0x004;
    k->normal = (fvf & 0x010) != 0;
    k->diffuse = (fvf & 0x040) != 0;
    k->specular = (fvf & 0x080) != 0;
    k->ntc = (uint8_t)(ntc > 8 ? 8 : ntc);
    for (i = 0; i < k->ntc; i++) {
        uint32_t sz = (fvf >> (16 + 2 * i)) & 3;
        k->tc_size[i] = (uint8_t)(sz == 0 ? 2 : sz == 1 ? 3 : sz == 2 ? 4 : 1);
    }
    k->lighting = xb_rs(XRS_LIGHTING) != 0 && !k->rhw;
    if (k->lighting) {
        k->local_viewer = xb_rs(XRS_LOCALVIEWER) != 0;
        k->normalize = xb_rs(XRS_NORMALIZENORMALS) != 0;
        if (xb_rs(XRS_COLORVERTEX)) {
            k->mat_src[0] = (uint8_t)xb_rs(XRS_DIFFUSEMATERIALSOURCE);
            k->mat_src[1] = (uint8_t)xb_rs(XRS_AMBIENTMATERIALSOURCE);
            k->mat_src[2] = (uint8_t)xb_rs(XRS_SPECULARMATERIALSOURCE);
            k->mat_src[3] = (uint8_t)xb_rs(XRS_EMISSIVEMATERIALSOURCE);
            for (i = 0; i < 4; i++)               /* a colour the FVF lacks reads the material */
                if ((k->mat_src[i] == 1 && !k->diffuse) || (k->mat_src[i] == 2 && !k->specular) || k->mat_src[i] > 2)
                    k->mat_src[i] = 0;
        }
        for (i = 0; i < 8; i++)
            if ((s_light_on >> i) & 1)
                k->light_type[i] = (uint8_t)(s_light[i].type >= 1 && s_light[i].type <= 3 ? s_light[i].type : 0);
    }
    for (i = 0; i < 4; i++) {
        uint32_t tci = xb_tss(i, XTSS_TEXCOORDINDEX), ttf = xb_tss(i, XTSS_TEXTURETRANSFORMFLAGS);
        k->tc_set[i] = (uint8_t)(tci & 7);
        k->tc_gen[i] = (uint8_t)((tci >> 16) & 3);
        k->tc_count[i] = (uint8_t)(ttf & 7);
        k->tc_proj[i] = (ttf & 0x100) != 0;
    }
}

static void upload_uniforms(const Prog *p)
{
    float v[16 * 4], wv[16], t[4];
    int i;

    if (p->u_wv >= 0) {
        mat_mul(s_xf[6].m, s_xf[0].m, wv);                  /* world0 * view */
        glUniformMatrix4fv(p->u_wv, 1, GL_FALSE, wv);
    }
    if (p->u_proj >= 0) {
        /* The title draws its movie quad exactly on the far plane with
         * ZFUNC LESS against a depth buffer cleared to 1.0. The NV2A's
         * integer depth rounds that below 1; the host compares (and clips)
         * in float. Scaling projected z by a hair under 1 keeps the order of
         * everything else. */
        Mat pr = s_xf[1];
        const float kz = 1.0f - 1.0f / 4194304.0f;
        pr.m[2] *= kz; pr.m[6] *= kz; pr.m[10] *= kz; pr.m[14] *= kz;
        glUniformMatrix4fv(p->u_proj, 1, GL_FALSE, pr.m);
    }
    if (p->u_texm >= 0) {
        for (i = 0; i < 4; i++)
            memcpy(v + 16 * i, s_xf[2 + i].m, 64);
        glUniformMatrix4fv(p->u_texm, 4, GL_FALSE, v);
    }
    if (p->u_mat >= 0) {
        memcpy(v, s_material.diffuse, 16); memcpy(v + 4, s_material.ambient, 16);
        memcpy(v + 8, s_material.specular, 16); memcpy(v + 12, s_material.emissive, 16);
        v[16] = s_material.power; v[17] = v[18] = v[19] = 0;
        glUniform4fv(p->u_mat, 5, v);
    }
    if (p->u_amb >= 0) {
        argb_vec(xb_rs(XRS_AMBIENT), t);
        glUniform4fv(p->u_amb, 1, t);
    }
    if (p->u_ld >= 0) {
        /* Lights go to eye space here, where the shader lights. */
        float ld[32], ls[32], la[32], lp[32], ldir[32], latt[32], lspot[32];
        const float *vw = s_xf[0].m;
        for (i = 0; i < 8; i++) {
            const Light *L = &s_light[i];
            const float *P = L->position, *D = L->direction;
            memcpy(ld + 4 * i, L->diffuse, 16); memcpy(ls + 4 * i, L->specular, 16); memcpy(la + 4 * i, L->ambient, 16);
            lp[4 * i] = P[0] * vw[0] + P[1] * vw[4] + P[2] * vw[8] + vw[12];
            lp[4 * i + 1] = P[0] * vw[1] + P[1] * vw[5] + P[2] * vw[9] + vw[13];
            lp[4 * i + 2] = P[0] * vw[2] + P[1] * vw[6] + P[2] * vw[10] + vw[14];
            lp[4 * i + 3] = L->range > 0 ? L->range : 1e30f;
            ldir[4 * i] = D[0] * vw[0] + D[1] * vw[4] + D[2] * vw[8];
            ldir[4 * i + 1] = D[0] * vw[1] + D[1] * vw[5] + D[2] * vw[9];
            ldir[4 * i + 2] = D[0] * vw[2] + D[1] * vw[6] + D[2] * vw[10];
            {
                float n = (float)sqrt(ldir[4 * i] * ldir[4 * i] + ldir[4 * i + 1] * ldir[4 * i + 1] + ldir[4 * i + 2] * ldir[4 * i + 2]);
                if (n > 0) { ldir[4 * i] /= n; ldir[4 * i + 1] /= n; ldir[4 * i + 2] /= n; }
            }
            ldir[4 * i + 3] = L->falloff;
            latt[4 * i] = L->att0; latt[4 * i + 1] = L->att1; latt[4 * i + 2] = L->att2; latt[4 * i + 3] = 0;
            lspot[4 * i] = (float)cos(L->theta * 0.5f); lspot[4 * i + 1] = (float)cos(L->phi * 0.5f);
            lspot[4 * i + 2] = lspot[4 * i + 3] = 0;
        }
        glUniform4fv(p->u_ld, 8, ld); glUniform4fv(p->u_ls, 8, ls); glUniform4fv(p->u_la, 8, la);
        glUniform4fv(p->u_lp, 8, lp); glUniform4fv(p->u_ldir, 8, ldir); glUniform4fv(p->u_latt, 8, latt);
        glUniform4fv(p->u_lspot, 8, lspot);
    }
    if (p->u_rhw >= 0) {
        t[0] = (float)s_vp.x; t[1] = (float)s_vp.y; t[2] = (float)s_vp_cw; t[3] = (float)s_vp_ch;
        glUniform4fv(p->u_rhw, 1, t);
    }
    if (p->u_fog >= 0) {
        t[0] = (float)xb_rs(XRS_FOGTABLEMODE); t[1] = f32(xb_rs(XRS_FOGSTART));
        t[2] = f32(xb_rs(XRS_FOGEND)); t[3] = f32(xb_rs(XRS_FOGDENSITY));
        glUniform4fv(p->u_fog, 1, t);
    }
    if (p->u_fogcolor >= 0) { argb_vec(xb_rs(XRS_FOGCOLOR), t); glUniform4fv(p->u_fogcolor, 1, t); }
    if (p->u_tf >= 0) { argb_vec(xb_rs(XRS_TEXTUREFACTOR), t); glUniform4fv(p->u_tf, 1, t); }
    if (p->u_aref >= 0) glUniform1f(p->u_aref, (float)(xb_rs(XRS_ALPHAREF) & 0xFF));
    if (p->u_tscale >= 0) {
        for (i = 0; i < 4; i++) {
            v[4 * i] = s_lin[i][0] ? s_lin[i][0] : 1.0f;
            v[4 * i + 1] = s_lin[i][1] ? s_lin[i][1] : 1.0f;
            v[4 * i + 2] = v[4 * i + 3] = 1.0f;
        }
        glUniform4fv(p->u_tscale, 4, v);
    }
    if (p->u_c >= 0 && p->key.vs) {
        /* c[58]/c[59] identity: the translated program undoes D3D's divide. */
        s_vsc[58][0] = s_vsc[58][1] = s_vsc[58][2] = s_vsc[58][3] = 1.0f;
        s_vsc[59][0] = s_vsc[59][1] = s_vsc[59][2] = s_vsc[59][3] = 0.0f;
        glUniform4fv(p->u_c, NV2A_VS_MAX_CONSTANTS, &s_vsc[0][0]);
    }
    if (p->u_psize >= 0) glUniform1f(p->u_psize, f32(xb_rs(XRS_POINTSIZE)) * (float)s_scale);
    if (p->u_psc >= 0) {
        float c[64];
        for (i = 0; i < 8; i++) {
            argb_vec(xb_rs(XRS_PSCONSTANT0_0 + (int)i), &c[4 * i]);
            argb_vec(xb_rs(XRS_PSCONSTANT1_0 + (int)i), &c[32 + 4 * i]);
        }
        glUniform4fv(p->u_psc, 16, c);
    }
    if (p->u_psf >= 0) {
        float c[8];
        argb_vec(xb_rs(XRS_PSFINALCOMBINERCONSTANT0), &c[0]);
        argb_vec(xb_rs(XRS_PSFINALCOMBINERCONSTANT1), &c[4]);
        glUniform4fv(p->u_psf, 2, c);
    }
}

/* -- Frame capture ------------------------------------------------------------ */

/* RECOMP_GL_CAPTURE=<file>: each time the file is written, the next frame is
 * logged draw by draw ([CAP] lines on stderr), with the draws that were
 * dropped and the NV2A methods decoded from the pushbuffer, and every colour
 * target drawn into is written next to the file as <file>_<n>_<address>.bmp.
 * A target is also written each time the draws move to another one, as
 * <file>_<n>_pass<k>_<address>.bmp, so passes that later draws overwrite stay
 * visible. <file>.view selects a diagnostic view (VIEW_*) for every frame. */

static const char *s_cap_path;
static FILETIME s_cap_mtime;
static int s_cap;                            /* this frame is being captured */
static int s_pb_draw;                        /* the draw was decoded from the pushbuffer */
static const char *s_drop;                   /* why the last prepare_draw returned NULL */
static uint32_t s_cap_seq, s_cap_pass, s_caller;
static HostRt *s_cap_rt;                     /* the target the capture's last draw went to */
static struct { uint32_t method, count; } s_cap_methods[256];
static int s_cap_nmethods;

/* A colour target as a 24-bit BMP at 1x, averaging the render scale away. */
static void write_bmp(HostRt *r, const char *path)
{
    uint32_t W = r->w, H = r->h, x, y, sx, sy, S = s_scale;
    uint8_t *px = (uint8_t *)malloc((size_t)W * S * H * S * 4);
    FILE *f;

    if (!px)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo_read);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
    glReadPixels(0, 0, (GLsizei)(W * S), (GLsizei)(H * S), GL_RGBA, GL_UNSIGNED_BYTE, px);
    if ((f = fopen(path, "wb")) != NULL) {
        uint32_t row = W * 3, pad = (4 - (row & 3)) & 3, filesz = 54 + (row + pad) * H;
        uint8_t hdr[54] = { 'B', 'M' };
        memcpy(hdr + 2, &filesz, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &W, 4);
        memcpy(hdr + 22, &H, 4);
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, sizeof hdr, f);
        /* Rows are top-down in the target; a BMP is bottom-up. */
        for (y = H; y-- > 0; ) {
            for (x = 0; x < W; x++) {
                uint32_t acc[3] = { 0, 0, 0 }, n = S * S;
                uint8_t bgr[3];
                for (sy = 0; sy < S; sy++)
                    for (sx = 0; sx < S; sx++) {
                        const uint8_t *p = px + ((size_t)(y * S + sy) * W * S + x * S + sx) * 4;
                        acc[0] += p[2]; acc[1] += p[1]; acc[2] += p[0];
                    }
                bgr[0] = (uint8_t)(acc[0] / n); bgr[1] = (uint8_t)(acc[1] / n); bgr[2] = (uint8_t)(acc[2] / n);
                fwrite(bgr, 1, 3, f);
            }
            fwrite("\0\0\0", 1, pad, f);
        }
        fclose(f);
    }
    free(px);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
}

static void cap_method(uint32_t key)
{
    int i;
    for (i = 0; i < s_cap_nmethods; i++)
        if (s_cap_methods[i].method == key) {
            s_cap_methods[i].count++;
            return;
        }
    if (s_cap_nmethods < (int)(sizeof s_cap_methods / sizeof s_cap_methods[0])) {
        s_cap_methods[s_cap_nmethods].method = key;
        s_cap_methods[s_cap_nmethods++].count = 1;
    }
}

/* At each present: end the frame being captured, then start one if the
 * trigger file was written since the last look. */
static void cap_frame(void)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    int i;

    if (s_cap) {
        char path[600];
        for (i = 0; i < s_nrt; i++) {
            if (!s_rt[i].captured)
                continue;
            s_rt[i].captured = 0;
            snprintf(path, sizeof path, "%s_%03u_%08X.bmp", s_cap_path, s_cap_seq, s_rt[i].data);
            write_bmp(&s_rt[i], path);
            fprintf(stderr, "[CAP] target %08X %ux%u -> %s\n", s_rt[i].data, s_rt[i].w, s_rt[i].h, path);
        }
        for (i = 0; i < s_cap_nmethods; i++)
            fprintf(stderr, "[CAP] pushbuffer method %04X subchannel %u: %u\n", s_cap_methods[i].method & 0x1FFC,
                    s_cap_methods[i].method >> 13, s_cap_methods[i].count);
        fprintf(stderr, "[CAP] frame %u captured\n", s_frame);
        s_cap = 0;
    }
    if (!s_cap_path) {
        s_cap_path = getenv("RECOMP_GL_CAPTURE");
        if (!s_cap_path)
            s_cap_path = "";
        else if (GetFileAttributesExA(s_cap_path, GetFileExInfoStandard, &a))
            s_cap_mtime = a.ftLastWriteTime;
    }
    if (*s_cap_path) {
        /* <file>.view holds the diagnostic view as one digit (VIEW_*, 0 normal). */
        char vp[600];
        FILE *vf;
        snprintf(vp, sizeof vp, "%s.view", s_cap_path);
        if ((vf = fopen(vp, "r")) != NULL) {
            int c = fgetc(vf);
            fclose(vf);
            if (c >= '0' && c <= '0' + VIEW_ALPHA && c - '0' != s_view) {
                s_view = c - '0';
                fprintf(stderr, "[CAP] diagnostic view %d\n", s_view);
            }
        }
    }
    if (!*s_cap_path || !GetFileAttributesExA(s_cap_path, GetFileExInfoStandard, &a)
        || !CompareFileTime(&a.ftLastWriteTime, &s_cap_mtime))
        return;
    s_cap_mtime = a.ftLastWriteTime;
    s_cap = 1;
    s_cap_seq++;
    s_cap_nmethods = 0;
    s_cap_pass = 0;
    s_cap_rt = NULL;
    fprintf(stderr, "[CAP] capture %u: frame %u\n", s_cap_seq, s_frame + 1);
}

static void cap_draw(uint32_t xprim, uint32_t n, const char *src, const Prog *prog)
{
    int st;
    if (!prog) {
        fprintf(stderr, "[CAP] dropped from %08X: %s; prim %u n %u %s vs %08X ps %08X\n",
                s_caller, s_drop, xprim, n, src, s_vshader, s_pshader);
        return;
    }
    s_cur_rt->captured = 1;
    if (s_cur_rt != s_cap_rt) {
        /* A pass ends: the target it drew, before anything later reads or overwrites it. */
        if (s_cap_rt) {
            char path[600];
            snprintf(path, sizeof path, "%s_%03u_pass%02u_%08X.bmp", s_cap_path, s_cap_seq, s_cap_pass++, s_cap_rt->data);
            write_bmp(s_cap_rt, path);
            fprintf(stderr, "[CAP] pass ends: %s\n", path);
        }
        s_cap_rt = s_cur_rt;
    }
    if (prog->key.vs) {
        /* The first draw of each vertex program: its inputs, the pixel
         * shader's combiner state and the non-zero constants. */
        static uint8_t seen[MAX_PROG];
        static uint32_t seen_seq;
        int c;
        if (seen_seq != s_cap_seq) { memset(seen, 0, sizeof seen); seen_seq = s_cap_seq; }
        if (!seen[prog - s_prog]++) {
            VsInput in[16];
            int ni = vs_inputs(prog->key.vs->decl, in), q;
            fprintf(stderr, "[CAP] prog %d inputs:", (int)(prog - s_prog));
            for (q = 0; q < ni; q++)
                fprintf(stderr, " v%d=s%d+%u:%02X(stride %u vb %08X)", in[q].reg, in[q].stream, in[q].offset, in[q].type,
                        s_stream_stride[in[q].stream], s_stream_vb[in[q].stream]);
            fprintf(stderr, " | ps ctl %08X modes %05X alphatest %u func %X ref %u fog %u",
                    xb_rs(XRS_PSCOMBINERCOUNT), xb_rs(XRS_PSTEXTUREMODES), xb_rs(XRS_ALPHATESTENABLE),
                    xb_rs(XRS_ALPHAFUNC), xb_rs(XRS_ALPHAREF), xb_rs(XRS_FOGENABLE));
            for (q = 0; q < 8; q++)
                fprintf(stderr, " [%08X %08X %08X %08X]", xb_rs(XRS_PSRGBINPUTS0 + q), xb_rs(XRS_PSRGBOUTPUTS0 + q),
                        xb_rs(XRS_PSALPHAINPUTS0 + q), xb_rs(XRS_PSALPHAOUTPUTS0 + q));
            fprintf(stderr, " final %08X %08X c0 %08X\n", xb_rs(XRS_PSFINALCOMBINERINPUTSABCD),
                    xb_rs(XRS_PSFINALCOMBINERINPUTSEFG), xb_rs(XRS_PSCONSTANT0_0));
            fprintf(stderr, "[CAP] prog %d constants:", (int)(prog - s_prog));
            for (c = 0; c < NV2A_VS_MAX_CONSTANTS; c++)
                if (s_vsc[c][0] != 0 || s_vsc[c][1] != 0 || s_vsc[c][2] != 0 || s_vsc[c][3] != 0)
                    fprintf(stderr, " c%d(%g %g %g %g)", c, s_vsc[c][0], s_vsc[c][1], s_vsc[c][2], s_vsc[c][3]);
            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "[CAP] draw from %08X prog %d prim %u n %u %s vs %08X ps %08X modes %05X rt %08X %ux%u%s"
            " vp %u,%u %ux%u z %u/%u blend %u %X>%X cull %X",
            s_caller, (int)(prog - s_prog), xprim, n, src, s_vshader, s_pshader,
            s_pshader ? xb_rs(XRS_PSTEXTUREMODES) : 0, s_cur_rt->data, s_cur_rt->w, s_cur_rt->h,
            s_cur_ds ? " +depth" : "", s_vp.x, s_vp.y, s_vp.w, s_vp.h, xb_rs(XRS_ZENABLE), xb_rs(XRS_ZWRITEENABLE),
            xb_rs(XRS_ALPHABLENDENABLE), xb_rs(XRS_SRCBLEND), xb_rs(XRS_DESTBLEND), xb_rs(XRS_CULLMODE));
    fprintf(stderr, " | %s surface format %08X pitch %08X colour %08X zeta %08X clip %08X %08X",
            s_surf_pb ? "pushbuffer" : "device", NV(0x0208), NV(0x020C), NV(0x0210), NV(0x0214), NV(0x0200),
            NV(0x0204));
    if (s_cur_ds)
        fprintf(stderr, " | depth %08X fmt %02X control0 %08X clip %g..%g vpscale %g %g %g %g vpoffset %g %g %g %g"
                " proj33 %g proj43 %g |", s_cur_ds->data, s_cur_ds->xfmt, NV(0x0290), f32(NV(0x0394)), f32(NV(0x0398)),
                f32(NV(0x0AF0)), f32(NV(0x0AF4)), f32(NV(0x0AF8)), f32(NV(0x0AFC)), f32(NV(0x0A20)),
                f32(NV(0x0A24)), f32(NV(0x0A28)), f32(NV(0x0A2C)), s_xf[1].m[10], s_xf[1].m[14]);
    for (st = 0; st < 4; st++) {
        uint32_t res = s_stage_tex[st], w, h, pitch;
        if (!res)
            continue;
        surf_dims(res, &w, &h, &pitch);
        fprintf(stderr, " t%d %08X fmt %02X %ux%u%s", st, res_data(res), (MEM32(res + 0xC) >> 8) & 0xFF, w, h,
                rt_find(res_data(res)) ? " (target)" : "");
    }
    fprintf(stderr, "\n");
}

/* -- Draws ------------------------------------------------------------------- */

static uint32_t fvf_stride(uint32_t fvf)
{
    uint32_t n = 0, i, tc = (fvf >> 8) & 0xF;
    switch (fvf & 0x00E) {
    case 0x002: n = 12; break; case 0x004: n = 16; break; case 0x006: n = 16; break;
    case 0x008: n = 20; break; case 0x00A: n = 24; break; case 0x00C: n = 28; break;
    }
    if (fvf & 0x010) n += 12;
    if (fvf & 0x020) n += 4;
    if (fvf & 0x040) n += 4;
    if (fvf & 0x080) n += 4;
    for (i = 0; i < tc; i++) {
        uint32_t sz = (fvf >> (16 + 2 * i)) & 3;
        n += sz == 0 ? 8 : sz == 1 ? 12 : sz == 2 ? 16 : 4;
    }
    return n;
}

/* Attribute pointers for an FVF vertex in the bound array buffer. */
static void fvf_attribs(uint32_t fvf, uint32_t stride)
{
    uint32_t off = 0, i, ntc = (fvf >> 8) & 0xF, pos = fvf & 0x00E;
    for (i = 0; i < 13; i++)
        glDisableVertexAttribArray(i);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, pos == 0x004 ? 4 : 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)(uintptr_t)off);
    off += pos == 0x004 ? 16 : 12;
    if (pos >= 0x006 && pos <= 0x00C) {           /* ponytail: blend weights are skipped, world0 only */
        static int said;
        if (!said++)
            fprintf(stderr, "[GL] FVF blend weights skipped (fvf 0x%x)\n", fvf);
        off += 4 * ((pos - 0x004) / 2);
    }
    if (fvf & 0x010) {
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)(uintptr_t)off);
        off += 12;
    }
    if (fvf & 0x020) off += 4;
    if (fvf & 0x040) {
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (const void *)(uintptr_t)off);
        off += 4;
    }
    if (fvf & 0x080) {
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (const void *)(uintptr_t)off);
        off += 4;
    }
    for (i = 0; i < ntc && i < 8; i++) {
        uint32_t sz = (fvf >> (16 + 2 * i)) & 3, n = sz == 0 ? 2 : sz == 1 ? 3 : sz == 2 ? 4 : 1;
        glEnableVertexAttribArray(5 + i);
        glVertexAttribPointer(5 + i, (GLint)n, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)(uintptr_t)off);
        off += n * 4;
    }
}

static uint8_t *s_vsbuf;
static size_t s_vsbuf_cap;

/* Vertices base+[lo, hi] for a program, as float4 slots. Stream 0 is `up`
 * when the draw supplies it. Returns the buffer (index 0 = vertex lo) and its
 * stride, and in *regs_out the registers it fills. */
static const void *vs_vertices(HostVs *h, uint32_t base, uint32_t lo, uint32_t hi,
                               const uint8_t *up, uint32_t up_stride, uint32_t *stride_out, uint16_t *regs_out)
{
    VsInput in[16];
    int n = vs_inputs(h->decl, in), slot[16], ns = 0, k;
    uint16_t regs = h->inputs;
    uint32_t i, stride;
    const uint8_t *src[16];
    uint32_t sstride[16];

    for (k = 0; k < n; k++)
        regs |= 1u << in[k].reg;
    for (k = 0; k < 16; k++)
        slot[k] = (regs & (1u << k)) ? ns++ : -1;
    if (!ns) {
        regs = 1;
        slot[0] = ns++;
    }
    stride = 16 * (uint32_t)ns;
    if ((size_t)(hi - lo + 1) * stride > s_vsbuf_cap) {
        free(s_vsbuf);
        s_vsbuf_cap = (size_t)(hi - lo + 1) * stride * 2;
        s_vsbuf = (uint8_t *)malloc(s_vsbuf_cap);
    }
    for (k = 0; k < 16; k++) {
        if (k == 0 && up) {
            src[k] = up; sstride[k] = up_stride;
        } else {
            src[k] = s_stream_vb[k] ? (const uint8_t *)gp(res_data(s_stream_vb[k])) : NULL;
            sstride[k] = s_stream_stride[k];
        }
    }
    for (i = lo; i <= hi; i++) {
        float *o = (float *)(s_vsbuf + (size_t)(i - lo) * stride);
        for (k = 0; k < ns; k++) {
            o[k * 4] = o[k * 4 + 1] = o[k * 4 + 2] = 0.0f; o[k * 4 + 3] = 1.0f;
        }
        for (k = 0; k < n; k++) {
            const uint8_t *s = src[in[k].stream];
            if (s)
                vsdt_read(in[k].type, s + (size_t)(base + i) * sstride[in[k].stream] + in[k].offset,
                          o + slot[in[k].reg] * 4);
        }
    }
    *stride_out = stride;
    *regs_out = regs;
    return s_vsbuf;
}

static char s_vsh_body[128 * 1024];

static Prog *prepare_draw(HostVs **vs_out)
{
    Key k;
    HostVs *vs = NULL;

    s_drop = "no GL context";
    if (!dev_ready())
        return NULL;
    if (s_vshader & 1) {
        vs = vs_find(s_vshader);
        s_drop = "vertex shader handle not created through CreateVertexShader";
        if (!vs)
            return NULL;
        if (!vs->glsl) {
            Sb s = { s_vsh_body, sizeof s_vsh_body, 0 };
            s_vsh_body[0] = 0;
            vsh_body(vs->mc, vs->nmc, &s, &vs->inputs, &vs->uses_vp);
            vs->glsl = (char *)malloc(s.len + 1);
            memcpy(vs->glsl, s_vsh_body, s.len + 1);
        }
    }
    {
        /* Depth buffers this draw reads as textures, before bind_targets:
         * the conversion changes state that bind_targets and
         * apply_render_state set again. */
        int st;
        for (st = 0; st < 4; st++) {
            HostRt *z = s_stage_tex[st] ? rt_find_kind(res_data(s_stage_tex[st]), 1) : NULL;
            HostRt *c = z ? rt_find(z->data) : NULL;
            if (z && (!c || z->written > c->written))
                depth_to_colour(z);
        }
    }
    bind_targets();
    s_drop = "no colour target";
    if (!s_cur_rt)
        return NULL;
    memset(&k, 0, sizeof k);
    k.vs = vs;
    k.vs_gen = vs ? vs->gen : 0;
    k.wbuf = s_cur_ds && xb_rs(XRS_ZENABLE) == 2 && depth_is_d24(s_cur_ds);
    key_vertex(&k);
    apply_render_state();
    apply_stages(&k);
    k.spec_add = xb_rs(XRS_SPECULARENABLE) != 0;
    k.fog = xb_rs(XRS_FOGENABLE) != 0;
    k.fog_table = k.fog && xb_rs(XRS_FOGTABLEMODE) != 0 && !vs;
    if (s_pshader) {
        int i;
        k.ps = 1;
        for (i = 0; i < 8; i++) {
            k.ps_ai[i] = xb_rs(XRS_PSALPHAINPUTS0 + i);
            k.ps_ao[i] = xb_rs(XRS_PSALPHAOUTPUTS0 + i);
            k.ps_ri[i] = xb_rs(XRS_PSRGBINPUTS0 + i);
            k.ps_ro[i] = xb_rs(XRS_PSRGBOUTPUTS0 + i);
        }
        k.ps_f0 = xb_rs(XRS_PSFINALCOMBINERINPUTSABCD);
        k.ps_f1 = xb_rs(XRS_PSFINALCOMBINERINPUTSEFG);
        k.ps_ctl = xb_rs(XRS_PSCOMBINERCOUNT);
        k.ps_modes = xb_rs(XRS_PSTEXTUREMODES);
        /* The texture-stage ops do not apply: keep them out of the key. */
        memset(k.colorop, 0, sizeof k.colorop);
        memset(k.colorarg, 0, sizeof k.colorarg);
        memset(k.alphaop, 0, sizeof k.alphaop);
        memset(k.alphaarg, 0, sizeof k.alphaarg);
        memset(k.result_temp, 0, sizeof k.result_temp);
    }
    k.alpha_test = xb_rs(XRS_ALPHATESTENABLE) != 0;
    k.alpha_func = k.alpha_test ? (uint8_t)(xb_rs(XRS_ALPHAFUNC) - 0x200) : 0;
    k.flat = xb_rs(XRS_SHADEMODE) == 0x1D00;
    if (vs && s_view) {
        k.view = (uint8_t)s_view;
        glDisable(GL_BLEND);
        if (s_view == VIEW_COVERAGE) {
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
        }
    }
    *vs_out = vs;
    {
        Prog *p = prog_get(&k);
        s_drop = "program does not compile";
        if (!p)
            return NULL;
        glUseProgram(p->prog);
        upload_uniforms(p);
        return p;
    }
}

static GLenum prim_gl(uint32_t xprim, uint32_t n, uint32_t *count)
{
    switch (xprim) {
    case 1: *count = n; return GL_POINTS;
    case 2: *count = n; return GL_LINES;
    case 3: *count = n; return GL_LINE_LOOP;
    case 4: *count = n; return GL_LINE_STRIP;
    case 5: *count = n; return GL_TRIANGLES;
    case 6: case 9: *count = n; return GL_TRIANGLE_STRIP;   /* quad strip: same order */
    case 7: case 10: *count = n; return GL_TRIANGLE_FAN;    /* polygon: convex */
    }
    *count = 0;
    return GL_TRIANGLES;
}

static uint16_t *s_qidx;
static size_t s_qidx_cap;

static uint16_t *qidx(size_t n)
{
    if (n > s_qidx_cap) {
        free(s_qidx);
        s_qidx_cap = n * 2;
        s_qidx = (uint16_t *)malloc(s_qidx_cap * sizeof *s_qidx);
    }
    return s_qidx;
}

/* One draw. Vertices come from `up` (stream 0 given with the call) or the
 * bound streams, starting at vertex `base`; idx == NULL draws [0, n). */
static void draw(uint32_t xprim, uint32_t n, const uint16_t *idx,
                 const uint8_t *up, uint32_t up_stride, uint32_t base)
{
    uint32_t lo = 0, hi = n ? n - 1 : 0, i, stride, count;
    const uint8_t *verts;
    HostVs *vs = NULL;
    uint16_t *ix = NULL;
    GLenum mode;

    Prog *prog;

    if (!n)
        return;
    prog = prepare_draw(&vs);
    if (s_cap)
        cap_draw(xprim, n, idx ? "indexed" : s_pb_draw ? "pushbuffer" : up ? "up" : "stream", prog);
    if (!prog)
        return;
    if (xb_rs(XRS_COLORWRITEENABLE))
        s_cur_rt->written = ++s_serial;
    if (s_cur_ds && xb_rs(XRS_ZENABLE) && xb_rs(XRS_ZWRITEENABLE))
        s_cur_ds->written = ++s_serial;
    {
        /* RECOMP_GL_DRAWLOG=<n>: the first n draws, for bring-up. */
        static long left = -1;
        if (left < 0) {
            const char *e = getenv("RECOMP_GL_DRAWLOG");
            left = e ? atol(e) : 0;
        }
        if (left > 0) {
            const float *v = (const float *)(up ? up : NULL);
            left--;
            fprintf(stderr, "[GL] draw prog %d ps %08X tex %08X prim %u n %u %s vs %08X stride %u rt %p vp %ux%u+%u+%u z %.2f-%.2f",
                    (int)(prog - s_prog), s_pshader, s_stage_tex[0], xprim, n, idx ? "indexed" : up ? "up" : "stream", s_vshader,
                    up ? up_stride : s_stream_stride[0], (void *)s_cur_rt, s_vp.w, s_vp.h, s_vp.x, s_vp.y,
                    s_vp.minz, s_vp.maxz);
            if (v)
                fprintf(stderr, " v0 %.1f %.1f %.3f %.3f", v[0], v[1], v[2], v[3]);
            fprintf(stderr, "\n");
        }
    }
    if (idx) {
        lo = 0xFFFF; hi = 0;
        for (i = 0; i < n; i++) {
            if (idx[i] < lo) lo = idx[i];
            if (idx[i] > hi) hi = idx[i];
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    if (vs) {
        uint16_t regs;
        int r;
        verts = (const uint8_t *)vs_vertices(vs, base, lo, hi, up, up_stride, &stride, &regs);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)(hi - lo + 1) * stride), verts, GL_STREAM_DRAW);
        for (r = 0, i = 0; r < 16; r++) {
            if (regs & (1u << r)) {
                glEnableVertexAttribArray((GLuint)r);
                glVertexAttribPointer((GLuint)r, 4, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)(uintptr_t)(16 * i));
                i++;
            } else {
                glDisableVertexAttribArray((GLuint)r);
                glVertexAttrib4f((GLuint)r, 0, 0, 0, 1);
            }
        }
    } else {
        stride = up ? up_stride : s_stream_stride[0];
        if (up)
            verts = up;
        else if (s_stream_vb[0])
            verts = (const uint8_t *)gp(res_data(s_stream_vb[0])) + (size_t)base * stride;
        else
            return;
        if (!stride)
            stride = fvf_stride(s_vshader);
        if (!stride)
            return;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)(hi - lo + 1) * stride), verts + (size_t)lo * stride, GL_STREAM_DRAW);
        fvf_attribs(s_vshader, stride);
    }
    /* Vertices went up from `lo`: indices are rebased onto that. Quads have
     * no GL primitive and are split into triangles. */
    if (xprim == 8) {
        uint32_t q;
        ix = qidx((size_t)n / 4 * 6);
        for (q = 0; q < n / 4; q++) {
            uint16_t v[4];
            for (i = 0; i < 4; i++)
                v[i] = (uint16_t)((idx ? idx[q * 4 + i] : q * 4 + i) - lo);
            ix[q * 6] = v[0]; ix[q * 6 + 1] = v[1]; ix[q * 6 + 2] = v[2];
            ix[q * 6 + 3] = v[0]; ix[q * 6 + 4] = v[2]; ix[q * 6 + 5] = v[3];
        }
        mode = GL_TRIANGLES;
        count = n / 4 * 6;
    } else {
        mode = prim_gl(xprim, n, &count);
        if (idx) {
            ix = qidx(n);
            for (i = 0; i < n; i++)
                ix[i] = (uint16_t)(idx[i] - lo);
        }
    }
    if (!count)
        return;
    if (ix) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(count * 2), ix, GL_STREAM_DRAW);
        glDrawElements(mode, (GLsizei)count, GL_UNSIGNED_SHORT, NULL);
    } else {
        glDrawArrays(mode, 0, (GLsizei)count);
    }
    s_st.draws++;
    s_st.prims += count;
}

/* -- Present ----------------------------------------------------------------- */

static void dump_bmp(HostRt *r)
{
    static const char *prefix = (const char *)-1;
    static int every, seq;
    char path[512];

    if (prefix == (const char *)-1) {
        const char *e = getenv("RECOMP_D3D9_DUMP_EVERY");
        prefix = getenv("RECOMP_D3D9_DUMP");
        every = e ? atoi(e) : 600;
    }
    if (!prefix || !every || s_frame % every != 0)
        return;
    snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
    write_bmp(r, path);
    fprintf(stderr, "[GL] frame dump %s\n", path);
}

/* -- Frame timing (RECOMP_PERF) ------------------------------------------------ */

/* Each present is timed against the previous one. Every second, with
 * RECOMP_PERF, a [PERF] line gives the frame rate, the mean, worst and 99th
 * percentile frame times, the slow frames (longer than the presentation
 * interval's frame, plus 0.8 ms of timer noise: the previous one shows again,
 * a stutter), the frames that missed a vblank (half a vblank over), those
 * over 50 and 100 ms, and the time spent in the renderer. Each frame that
 * missed a vblank gets a [LATE] line (over 50 ms, [HITCH]) with where its time
 * went. The host's monitor reads the same numbers. */

#define VBLANK_MS (1001.0 / 60.0)              /* NTSC, 59.94 Hz */
static uint32_t s_interval = 1;                /* FullScreen_PresentationInterval */
#define PERF_HITCH_MS 50.0
static double s_pace_ms;                      /* gl_hle_swap's wait before this present */

static int interval_vblanks(void)
{
    return s_interval & 8 ? 4 : s_interval & 4 ? 3 : s_interval & 2 ? 2 : 1;
}

/* The CPU time the presenting (game) thread used since the last present, from
 * its cycle count: a late frame that used far less CPU than it took was
 * waiting, not working. The cycle counter's rate is measured against the
 * performance counter over the first frames. */
static double thread_cpu_ms(void)
{
    static ULONG64 c_prev, c0;
    static double q0, per_ms;
    ULONG64 c = 0;
    double now = now_ms(), ms;

    QueryThreadCycleTime(GetCurrentThread(), &c);
    if (!q0) {
        q0 = now;
        c0 = __rdtsc();
    } else if (!per_ms && now - q0 > 500.0) {
        per_ms = (double)(__rdtsc() - c0) / (now - q0);
    }
    ms = per_ms && c_prev ? (double)(c - c_prev) / per_ms : -1.0;
    c_prev = c;
    return ms;
}
static double s_swap_ms;                      /* the last present's buffer swap */
static volatile double s_last_present;       /* now_ms() at the last present */
static volatile uint32_t s_presents;
static CRITICAL_SECTION s_perf_cs;           /* guards s_perf only, never held long */
static GlHlePerf s_perf;

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void perf_frame(double now)
{
    static int on = -1;
    static float ms[512];
    static double t0, in_sum;
    static unsigned n, slow, late, over50, over100, compiled, loaded;
    static uint32_t f0;
    double prev = s_last_present, dt = prev ? now - prev : 0.0, renderer = s_in_ms, cpu = thread_cpu_ms();
    const double target = interval_vblanks() * VBLANK_MS;
    const double slow_ms = target + 0.8, late_ms = target + VBLANK_MS / 2;

    if (on < 0) {
        const char *e = getenv("RECOMP_PERF");
        on = e && strcmp(e, "0") != 0;
        InitializeCriticalSection(&s_perf_cs);
        t0 = now;
    }
    s_last_present = now;
    s_presents++;
    s_in_ms = 0;
    if (prev) {
        if (n < sizeof ms / sizeof ms[0])
            ms[n++] = (float)dt;
        in_sum += renderer;
        compiled += s_st.vs_compiles;
        loaded += s_st.prog_loads;
        slow += dt > slow_ms;
        late += dt > late_ms;
        over50 += dt > PERF_HITCH_MS;
        over100 += dt > 100.0;
        /* Where the frame went: the renderer (its last buffer swap
         * included), the present pacing, and the game's own code. */
        if (on && dt > late_ms)
            fprintf(stderr, "[%s] frame %u: %.1f ms: renderer %.1f (swap %.1f), present pacing %.1f, game %.1f;"
                    " thread on the CPU %.1f; %u draws, %u textures created, %u programs compiled, %u loaded from the"
                    " cache, %u target uploads, %u depth conversions\n",
                    dt > PERF_HITCH_MS ? "HITCH" : "LATE", s_frame, dt, renderer, s_swap_ms, s_pace_ms,
                    dt - renderer - s_pace_ms, cpu, s_st.draws, s_st.tex_creates, s_st.vs_compiles, s_st.prog_loads,
                    s_st.rt_uploads, s_st.depth_convs);
    }
    s_pace_ms = 0;
    if (now - t0 < 1000.0 || !n)
        return;
    {
        GlHlePerf p;
        double sum = 0;
        unsigned i;
        for (i = 0; i < n; i++)
            sum += ms[i];
        qsort(ms, n, sizeof ms[0], cmp_float);
        p.fps = (float)((s_frame - f0) * 1000.0 / (now - t0));
        p.avg_ms = (float)(sum / n);
        p.worst_ms = ms[n - 1];
        p.p99_ms = ms[n - 1 - n / 100];
        p.renderer_ms = (float)(in_sum / n);
        p.target_ms = (float)target;
        p.slow = slow;
        p.late = late;
        p.over50 = over50;
        p.over100 = over100;
        EnterCriticalSection(&s_perf_cs);
        s_perf = p;
        LeaveCriticalSection(&s_perf_cs);
        if (on)
            fprintf(stderr, "[PERF] %.1f fps, frame %.1f ms mean, %.1f worst, %.1f 99th percentile;"
                    " %u slow, %u missed a vblank, %u over 50 ms, %u over 100 ms (target %.1f ms);"
                    " renderer %.1f ms a frame; programs %u compiled, %u from the cache\n", p.fps, p.avg_ms,
                    p.worst_ms, p.p99_ms, p.slow, p.late, p.over50, p.over100, p.target_ms, p.renderer_ms, compiled,
                    loaded);
    }
    t0 = now;
    f0 = s_frame;
    n = slow = late = over50 = over100 = compiled = loaded = 0;
    in_sum = 0;
}

double gl_hle_last_present_ms(void) { return s_last_present; }
double gl_hle_frame_renderer_ms(void)
{
    /* Read from another thread without the lock: a close estimate, the call
     * in progress included. */
    double t0 = s_lock_t0;
    return s_in_ms + (s_lock_depth ? now_ms() - t0 : 0.0);
}
uint32_t gl_hle_presents(void) { return s_presents; }

int gl_hle_perf(GlHlePerf *out)
{
    if (!s_presents)
        return 0;
    EnterCriticalSection(&s_perf_cs);
    *out = s_perf;
    LeaveCriticalSection(&s_perf_cs);
    return out->fps > 0;
}

static void present(uint32_t data)
{
    HostRt *r = data ? rt_find(data) : NULL;
    int ww, wh;

    if (!r)
        r = s_cur_rt;
    rt_sync(r, 1);
    if (r)
        r->captured |= s_cap;
    cap_frame();
    s_frame++;
    perf_frame(now_ms());
    if (!r || !r->tex)
        return;
    dump_bmp(r);
    win_size(&ww, &wh);
    {
        static int said;
        if (!said++)
            fprintf(stderr, "[GL] presenting into %dx%d\n", ww, wh);
    }
    if (ww > 0 && wh > 0) {
        int aw = r->w;
        int fw = ww, fh = (int)((int64_t)ww * r->h / aw), fx, fy;
        if (fh > wh) { fh = wh; fw = (int)((int64_t)wh * aw / r->h); }
        fx = (ww - fw) / 2;
        fy = (wh - fh) / 2;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, ww, wh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo_read);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
        /* Target row 0 is the top; the window's is the bottom: flip. */
        glBlitFramebuffer(0, 0, (GLint)(r->w * s_scale), (GLint)(r->h * s_scale),
                          fx, wh - fy, fx + fw, wh - fy - fh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        {
            double t = now_ms();
            swap_window();
            s_swap_ms = now_ms() - t;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    memset(&s_st, 0, sizeof s_st);
}

/* -- Inline pushbuffer draws ---------------------------------------------------- */

/* Titles built with D3D8LTCG write some primitives into the pushbuffer
 * themselves: SET_BEGIN_END(prim), INLINE_ARRAY vertices in the current vertex
 * format, SET_BEGIN_END(0). They also write vertex shader constants there
 * directly. Everything written since the last flush is decoded; other methods
 * are state the mirrored globals already carry. A flush happens before any
 * intercepted D3D call changes state. */

static uint32_t s_pb_last;
static uint32_t s_pb_prim, s_pb_const;
static uint8_t *s_pb_verts;
static uint32_t s_pb_len, s_pb_cap;

static void pb_append(const uint8_t *p, uint32_t n)
{
    if (s_pb_len + n > s_pb_cap) {
        uint32_t cap = s_pb_cap ? s_pb_cap * 2 : 4096;
        uint8_t *v;
        while (cap < s_pb_len + n) cap *= 2;
        v = (uint8_t *)realloc(s_pb_verts, cap);
        if (!v) return;
        s_pb_verts = v;
        s_pb_cap = cap;
    }
    memcpy(s_pb_verts + s_pb_len, p, n);
    s_pb_len += n;
}

static void pb_decode(uint32_t p, uint32_t end)
{
    while (p + 4 <= end) {
        uint32_t h = MEM32(p), count = (h >> 18) & 0x7FF, method = h & 0x1FFC;
        p += 4;
        if (h == 0)
            continue;
        if ((h & 0xE0030003) && !(h & 0x40000000)) {
            if (s_cap)                          /* old jump 001, new jump 01, call 10, return 0x20000 */
                fprintf(stderr, "[CAP] pushbuffer %s %08X at %08X; decoding stops\n",
                        (h & 0xE0000003) == 0x20000000 ? "jump to" : (h & 3) == 1 ? "jump to"
                        : (h & 3) == 2 ? "call to" : h == 0x00020000 ? "return" : "unknown header",
                        (h & 0xE0000003) == 0x20000000 ? h & 0x1FFFFFFC : (h & 3) ? h & 0xFFFFFFFC : h, p - 4);
            return;                             /* jump or call: the ring moved */
        }
        if (p + count * 4 > end)
            return;
        if (s_cap)
            cap_method(h & 0xFFFC);
        if (!(h & 0xE000)) {                    /* subchannel 0, the 3D class */
            uint32_t j, incr = !(h & 0x40000000);
            for (j = 0; j < count; j++) {
                uint32_t m = (method >> 2) + (incr ? j : 0);
                s_nv[m & 0x7FF] = MEM32(p + 4 * j);
                if (m == 0x0210 >> 2)           /* SET_SURFACE_COLOR_OFFSET */
                    s_surf_pb = 1;
            }
        }
        if (method == 0x17FC && count) {        /* SET_BEGIN_END */
            uint32_t prim = MEM32(p);
            if (prim == 0 && s_pb_prim && s_pb_len) {
                uint32_t stride = fvf_stride(s_vshader);
                if (stride && s_pb_len % stride == 0) {
                    s_pb_draw = 1;
                    draw(s_pb_prim, s_pb_len / stride, NULL, s_pb_verts, stride, 0);
                    s_pb_draw = 0;
                } else {
                    static int said;
                    if (said++ < 4)
                        fprintf(stderr, "[GL] inline draw skipped: %u bytes, vertex shader %08X stride %u\n",
                                s_pb_len, s_vshader, stride);
                }
            }
            s_pb_prim = prim;
            s_pb_len = 0;
        } else if (method == 0x1818 && s_pb_prim) { /* INLINE_ARRAY */
            pb_append((const uint8_t *)gp(p), count * 4);
        } else if (method == 0x1EA4 && count) {     /* SET_TRANSFORM_CONSTANT_LOAD: a slot */
            s_pb_const = MEM32(p);
        } else if (method >= 0x0B80 && method < 0x0C00) {
            /* SET_TRANSFORM_CONSTANT: four dwords per slot from the load
             * slot on. Most of the title's constants only come this way. */
            uint32_t j, comp = (method - 0x0B80) / 4, incr = !(h & 0x40000000);
            for (j = 0; j < count; j++, comp += incr) {
                if (s_pb_const < NV2A_VS_MAX_CONSTANTS)
                    memcpy(&s_vsc[s_pb_const][comp & 3], gp(p + 4 * j), 4);
                if ((comp & 3) == 3)
                    s_pb_const++;
            }
        }
        p += count * 4;
    }
}

void gl_hle_pushbuffer_flush(void)
{
    uint32_t dev = xb_device(), cur;
    if (!dev)
        return;
    cur = MEM32(dev + XB_DEV_PUSH_PUT);
    if (s_pb_last && cur > s_pb_last && cur - s_pb_last < 0x400000) {
        lock();
        if (dev_ready())
            pb_decode(s_pb_last, cur);
        unlock();
    } else if (s_cap && cur != s_pb_last) {
        fprintf(stderr, "[CAP] pushbuffer put moved %08X -> %08X: not decoded\n", s_pb_last, cur);
    }
    s_pb_last = cur;
}

void gl_hle_pushbuffer_restart(void)
{
    uint32_t dev = xb_device();
    s_pb_last = dev ? MEM32(dev + XB_DEV_PUSH_PUT) : 0;
    s_pb_prim = 0;
    s_pb_len = 0;
}

/* -- API (gl_hle.h) ------------------------------------------------------------ */

uint32_t gl_hle_back_buffer(void)
{
    uint32_t bb = MEM32(xb_device() + XB_DEV_BACK_BUFFER);
    return bb ? res_data(bb) : 0;
}

/* Presentation interval (gl_hle_set_presentation_interval). On the console a
 * flip waits for its vblank in the GPU, and the title blocks once the GPU is
 * a frame behind; here the GPU's fences always read done, so nothing would
 * hold the title back but this. VBLANK_MS and s_interval are with the frame
 * timing above. */
void gl_hle_set_presentation_interval(uint32_t interval)
{
    /* RECOMP_PRESENT_INTERVAL=<n> replaces the title's: 1 every vblank (60
     * a second), 2 every other one, with IMMEDIATE kept. */
    const char *e = getenv("RECOMP_PRESENT_INTERVAL");
    s_interval = interval ? interval : 1;      /* D3DPRESENT_INTERVAL_DEFAULT is ONE */
    if (e && atoi(e) >= 1 && atoi(e) <= 4) {
        static const uint32_t bits[5] = { 0, 1, 2, 4, 8 };
        s_interval = (s_interval & 0x80000000u) | bits[atoi(e)];
    }
    fprintf(stderr, "[GL] presentation interval 0x%08X: every %d vblank(s)%s%s\n", interval, interval_vblanks(),
            s_interval & 0x80000000u ? ", a late frame at once" : "",
            e && atoi(e) >= 1 && atoi(e) <= 4 ? " (RECOMP_PRESENT_INTERVAL)" : "");
}

void gl_hle_swap(uint32_t flags, uint32_t back_buffer)
{
    if (flags == 0 || (flags & 4)) {
        /* Presents keep a grid of `vblanks` vblanks from the first one. A
         * frame on time waits for its slot; a late one shows at once with
         * IMMEDIATE (and starts the grid again from there), or waits for the
         * next vblank without. */
        static double next;
        const int vblanks = interval_vblanks();
        double now = now_ms(), t0 = now, base;
        if (next && now < next) {
            while (now < next) {
                sleep_ms(next - now - 1);
                now = now_ms();
            }
            base = next;
        } else if (next && !(s_interval & 0x80000000u)) {
            double due = next + ceil((now - next) / VBLANK_MS) * VBLANK_MS;
            while (now < due) {
                sleep_ms(due - now - 1);
                now = now_ms();
            }
            base = due;
        } else {
            base = now;
        }
        next = base + vblanks * VBLANK_MS;
        s_pace_ms = now - t0;
        lock();
        if (dev_ready())
            present(back_buffer);
        unlock();
    }
}

void gl_hle_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
    GLbitfield f = 0;
    Rect r[16];
    uint32_t i;

    lock();
    if (dev_ready()) {
        bind_targets();
        if ((flags & 0xF0) && s_cur_rt) f |= GL_COLOR_BUFFER_BIT;
        if (s_cur_ds && (flags & 1)) f |= GL_DEPTH_BUFFER_BIT;
        if (s_cur_ds && (flags & 2)) f |= GL_STENCIL_BUFFER_BIT;
        if (count > 16) count = 16;
        for (i = 0; i < count && rects; i++) {
            r[i].x1 = SMEM32(rects + 16 * i); r[i].y1 = SMEM32(rects + 16 * i + 4);
            r[i].x2 = SMEM32(rects + 16 * i + 8); r[i].y2 = SMEM32(rects + 16 * i + 12);
        }
        if (f) {
            float c[4];
            argb_vec(color, c);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glStencilMask(0xFF);
            glClearColor(c[0], c[1], c[2], c[3]);
            glClearDepthf(z);
            glClearStencil((GLint)stencil);
            if (rects && count) {
                glEnable(GL_SCISSOR_TEST);
                for (i = 0; i < count; i++) {
                    glScissor(r[i].x1 * (GLint)s_scale, r[i].y1 * (GLint)s_scale,
                              (r[i].x2 - r[i].x1) * (GLsizei)s_scale, (r[i].y2 - r[i].y1) * (GLsizei)s_scale);
                    glClear(f);
                }
                glDisable(GL_SCISSOR_TEST);
            } else {
                glDisable(GL_SCISSOR_TEST);
                glClear(f);
            }
        }
        if ((f & GL_COLOR_BUFFER_BIT) && s_cur_rt)
            rt_fill(s_cur_rt, r, rects ? count : 0, color);
        if (f & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))
            s_cur_ds->written = ++s_serial;
    }
    unlock();
}

/* The guest's copy has already run on guest memory, which holds nothing the
 * host drew; the copy is redone on the host targets and the destination's
 * guest hash recorded, so rt_sync does not replace it with the guest's bytes.
 * Without rects, the whole source goes to the whole destination, scaled. */
void gl_hle_copy_rects(uint32_t src, uint32_t rects, uint32_t count, uint32_t dst, uint32_t points)
{
    HostRt *s, *d;
    GLint S = (GLint)s_scale;
    uint32_t i;

    lock();
    if (dev_ready() && src && dst && (s = rt_find(res_data(src))) != NULL) {
        rt_sync(s, 0);
        d = rt_get(dst, 0);
        if (d != s) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo_read);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->tex, 0);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            if (!rects || !count) {
                glBlitFramebuffer(0, 0, (GLint)s->w * S, (GLint)s->h * S, 0, 0, (GLint)d->w * S, (GLint)d->h * S,
                                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
            } else {
                for (i = 0; i < count; i++) {
                    int32_t x1 = SMEM32(rects + 16 * i), y1 = SMEM32(rects + 16 * i + 4);
                    int32_t x2 = SMEM32(rects + 16 * i + 8), y2 = SMEM32(rects + 16 * i + 12);
                    int32_t dx = points ? SMEM32(points + 8 * i) : x1, dy = points ? SMEM32(points + 8 * i + 4) : y1;
                    glBlitFramebuffer(x1 * S, y1 * S, x2 * S, y2 * S, dx * S, dy * S, (dx + x2 - x1) * S,
                                      (dy + y2 - y1) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                }
            }
            s_cur_rt = NULL;                 /* the draw FBO's attachments changed */
            glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
            d->ghash = hash_bytes((const uint8_t *)gp(d->data), rt_guest_bytes(d));
            d->written = ++s_serial;
        }
    }
    unlock();
}

void gl_hle_set_viewport(uint32_t p)
{
    if (p) {
        s_vp.x = MEM32(p); s_vp.y = MEM32(p + 4);
        s_vp.w = MEM32(p + 8); s_vp.h = MEM32(p + 12);
        s_vp.minz = f32(MEM32(p + 16)); s_vp.maxz = f32(MEM32(p + 20));
    }
}

void gl_hle_set_transform(uint32_t state, uint32_t matrix)
{
    if (state < 10 && matrix)
        memcpy(&s_xf[state], gp(matrix), sizeof(Mat));
}

void gl_hle_set_light(uint32_t index, uint32_t light)
{
    if (index < 8 && light)
        memcpy(&s_light[index], gp(light), sizeof(Light));
}

void gl_hle_light_enable(uint32_t index, uint32_t enable)
{
    if (index < 8)
        s_light_on = (s_light_on & ~(1u << index)) | ((uint32_t)(enable != 0) << index);
}

void gl_hle_set_material(uint32_t material)
{
    if (material)
        memcpy(&s_material, gp(material), sizeof s_material);
}

void gl_hle_set_texture(uint32_t stage, uint32_t texture)
{
    if (stage < 4)
        s_stage_tex[stage] = texture;
}

void gl_hle_set_palette(uint32_t stage, uint32_t palette)
{
    if (stage < 4)
        s_stage_pal[stage] = palette;
}

void gl_hle_set_stream_source(uint32_t stream, uint32_t vb, uint32_t stride)
{
    if (stream < 16) {
        s_stream_vb[stream] = vb;
        s_stream_stride[stream] = stride;
    }
}

void gl_hle_set_indices(uint32_t ib, uint32_t base_vertex)
{
    s_ib = ib;
    s_base_vertex = base_vertex;
}

void gl_hle_set_vertex_shader(uint32_t handle) { s_vshader = handle; }

void gl_hle_caller(uint32_t return_address) { s_caller = return_address; }

void gl_hle_render_target_set(void) { s_surf_pb = 0; }

void gl_hle_set_pixel_shader(uint32_t handle) { s_pshader = handle; }

void gl_hle_draw_vertices_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride)
{
    lock();
    if (data)
        draw(prim, count, NULL, (const uint8_t *)gp(data), stride, 0);
    unlock();
}

void gl_hle_draw_indexed_vertices_up(uint32_t prim, uint32_t count, uint32_t indices, uint32_t data, uint32_t stride)
{
    lock();
    if (indices && data)
        draw(prim, count, (const uint16_t *)gp(indices), (const uint8_t *)gp(data), stride, 0);
    unlock();
}

void gl_hle_draw_vertices(uint32_t prim, uint32_t start, uint32_t count)
{
    lock();
    draw(prim, count, NULL, NULL, 0, start);
    unlock();
}

void gl_hle_draw_indexed_vertices(uint32_t prim, uint32_t count, uint32_t indices)
{
    lock();
    if (indices)
        draw(prim, count, (const uint16_t *)gp(indices), NULL, 0, s_base_vertex);
    unlock();
}

void gl_hle_create_vertex_shader(uint32_t decl, uint32_t func, uint32_t handle_ptr)
{
    HostVs *h;
    int i;

    if (!handle_ptr || !MEM32(handle_ptr) || !func)
        return;
    lock();
    h = vs_find(MEM32(handle_ptr));
    if (!h && s_nvs < MAX_VS)
        h = &s_vs[s_nvs++];
    if (!h) {
        static int said;
        if (!said++)
            fprintf(stderr, "[GL] more than %d vertex shaders: %08X and later ones draw nothing\n", MAX_VS,
                    MEM32(handle_ptr));
    } else {
        free(h->glsl);
        memset(h, 0, sizeof *h);
        h->handle = MEM32(handle_ptr);
        h->gen = ++s_vs_gen;
        for (i = 0; i < 96; i++) {
            h->decl[i] = decl ? MEM32(decl + 4 * i) : 0xFFFFFFFFu;
            if (h->decl[i] == 0xFFFFFFFFu)
                break;
        }
        if (i == 96) h->decl[95] = 0xFFFFFFFFu;
        h->nmc = (int)(MEM32(func) >> 16);
        if (h->nmc > NV2A_VS_MAX_INSTRUCTIONS) h->nmc = NV2A_VS_MAX_INSTRUCTIONS;
        memcpy(h->mc, gp(func + 4), (size_t)h->nmc * 16);
    }
    unlock();
}

void gl_hle_set_vertex_shader_constants(int32_t reg, uint32_t data, uint32_t count)
{
    uint32_t k;
    for (k = 0; k < count && reg + (int32_t)k < NV2A_VS_MAX_CONSTANTS && data; k++)
        if (reg + (int32_t)k >= 0)
            memcpy(s_vsc[reg + k], gp(data + 16 * k), 16);
}

/* -- Init -------------------------------------------------------------------- */

void gl_hle_init(void)
{
    const char *e = getenv("RECOMP_SCALE");
    int i;
    lock_init();
    for (i = 0; i < 10; i++) {          /* D3D's defaults: identity */
        memset(&s_xf[i], 0, sizeof s_xf[i]);
        s_xf[i].m[0] = s_xf[i].m[5] = s_xf[i].m[10] = s_xf[i].m[15] = 1.0f;
    }
    if (e && atoi(e) >= 1 && atoi(e) <= 8)
        s_scale = (uint32_t)atoi(e);
}
