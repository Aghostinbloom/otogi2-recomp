#ifndef GL_HLE_H
#define GL_HLE_H

/* The D3D8 renderer's entry points. A game's glue intercepts its D3D
 * functions, reads their arguments as its D3D build passes them, and calls
 * these. Addresses are guest virtual addresses. State setters only mirror
 * state: the guest's own function still has to run. Draws replace the guest's
 * draw entirely. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_hle_init(void);

/* Provided by the host: the window to draw into (an HWND), or NULL while it
 * does not exist yet, in which case the renderer tries again next frame. */
void *gl_hle_host_window(void);

/* Read the back buffer before the guest's Swap rotates it, then present it. */
uint32_t gl_hle_back_buffer(void);
void gl_hle_swap(uint32_t flags, uint32_t back_buffer);

void gl_hle_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
/* After the guest's CopyRects: redo the copy between host targets. */
void gl_hle_copy_rects(uint32_t source, uint32_t rects, uint32_t count, uint32_t destination, uint32_t points);
void gl_hle_set_viewport(uint32_t viewport);
void gl_hle_set_transform(uint32_t state, uint32_t matrix);
void gl_hle_set_light(uint32_t index, uint32_t light);
void gl_hle_light_enable(uint32_t index, uint32_t enable);
void gl_hle_set_material(uint32_t material);
void gl_hle_set_texture(uint32_t stage, uint32_t texture);
void gl_hle_set_palette(uint32_t stage, uint32_t palette);
void gl_hle_set_stream_source(uint32_t stream, uint32_t vertex_buffer, uint32_t stride);
void gl_hle_set_indices(uint32_t index_buffer, uint32_t base_vertex);
void gl_hle_set_vertex_shader(uint32_t handle);
void gl_hle_set_pixel_shader(uint32_t handle);
/* After the guest's CreateVertexShader has filled *handle_ptr. */
void gl_hle_create_vertex_shader(uint32_t declaration, uint32_t function, uint32_t handle_ptr);
void gl_hle_set_vertex_shader_constants(int32_t first_register, uint32_t data, uint32_t count);

/* Draw what the title wrote into the pushbuffer itself since the last flush
 * (SET_BEGIN_END and INLINE_ARRAY). Call before anything changes state, and
 * restart after the guest's pushbuffer wraps. */
void gl_hle_pushbuffer_flush(void);
void gl_hle_pushbuffer_restart(void);

/* The guest return address of the draw being handled, for the frame capture
 * (RECOMP_GL_CAPTURE). */
void gl_hle_caller(uint32_t return_address);

void gl_hle_draw_vertices_up(uint32_t primitive, uint32_t vertex_count, uint32_t data, uint32_t stride);
void gl_hle_draw_indexed_vertices_up(uint32_t primitive, uint32_t vertex_count, uint32_t indices,
                                     uint32_t data, uint32_t stride);
void gl_hle_draw_vertices(uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count);
void gl_hle_draw_indexed_vertices(uint32_t primitive, uint32_t vertex_count, uint32_t indices);

#ifdef __cplusplus
}
#endif

#endif
