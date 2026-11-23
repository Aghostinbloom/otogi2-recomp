#ifndef OTOGI2_D3D_GAME_H
#define OTOGI2_D3D_GAME_H

/* Otogi 2's D3D (XDK 5849, D3D8LTCG) globals, from XbSymbolDatabase. */
#define XB_RENDER_STATE      0x001EE208u   /* D3D_g_RenderState */
#define XB_TEXTURE_STATE     0x001EBB40u   /* D3D_g_DeferredTextureState[4][32] */
#define XB_STREAMS           0x001EF1C8u   /* D3D_g_Stream[16] */
#define XB_DEVICE_PTR        0x001EE1F8u   /* D3D_g_pDevice */

/* Device fields, as Clear, Swap and BeginPush read them. */
#define XB_DEV_PUSH_PUT      0x0000u       /* where the CPU writes the next method */
#define XB_DEV_RENDER_TARGET 0x1A04u
#define XB_DEV_DEPTH_STENCIL 0x1A08u
#define XB_DEV_BACK_BUFFER   0x1A14u

#endif
