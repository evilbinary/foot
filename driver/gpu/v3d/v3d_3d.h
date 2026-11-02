/*
 * v3d_3d.h - VideoCore VII (V3D 7.1) 3D-pipeline back end for the EwokOS
 * raspberry-pi5 videocore library.
 *
 * This module drives the V3D *render* pipeline (CLE binning thread CT0 +
 * render thread CT1, PTB tile allocator, GL shader-state records and QPU
 * coordinate/vertex/fragment shaders), ported from the bare-metal bring-up in
 * videocore7-exps/app/src/v3d.c.  It is the 3D counterpart of v3d_g2d.c's
 * CSD compute path and deliberately REUSES that path's bring-up:
 *
 *   - the V3D register window, the V3D MMU (identity over sys_dma +
 *     shm_contig), the SMS power-up, the L2C enable and the clock are all
 *     established once by v3d_g2d_init(); this module never remaps the block
 *     and never issues a runtime power-domain write (PM GRAFX_V3D / SMS),
 *     which is proven to wedge the g2dd daemon;
 *   - every buffer it hands the GPU (control lists, binning pool, tile-state
 *     array, shader-state record, staged QPU code, uniforms, generic tile
 *     list) is dma_alloc'ed, so it already sits inside the V3D page table and
 *     its GPU IOVA is the low 32 bits of its physical address;
 *   - dispatches are serialized against the CSD path through
 *     v3d_g2d_op_begin()/op_end() and share its L2 maintenance helpers.
 *
 * Attribute wiring note (finished here, left incomplete upstream): for the
 * non-indexed VERTEX_ARRAY_PRIMS path the 16-byte vertex-attribute records
 * must lie CONTIGUOUSLY immediately after the 32-byte GL shader-state record,
 * because the CLE walks `number_of_attribute_arrays` records from
 * shader_record_address + 32.  The upstream build_attr_records() filled a
 * separate, never-referenced buffer; here the record and its attribute arrays
 * are one 32-byte-aligned allocation.
 *
 * HARDWARE STATUS: the V3D 7.1 binning/render bring-up is experimental.  It
 * cannot be exercised under QEMU (no V3D 3D model) and requires iterative
 * validation on a physical Pi 5.  Every entry point returns a tri-state so a
 * failure or an unsupported request degrades to the caller's CPU path instead
 * of hanging the compositor; a submitted job is never replayed.
 */

#ifndef V3D_3D_H
#define V3D_3D_H

#include <stdint.h>
#include <stddef.h>

/* V3D output image formats (subset, STORE_TILE_BUFFER_GENERAL field). */
#define V3D_3D_FMT_BGR565 7u
#define V3D_3D_FMT_RGBA8  27u
#define V3D_3D_FMT_D32F   40u   /* depth_32f, for the persistent Z surface */

/* Primitive modes for VERTEX_ARRAY_PRIMS. */
#define V3D_3D_MODE_POINTS    0
#define V3D_3D_MODE_LINES     1
#define V3D_3D_MODE_TRIANGLES 4

/* Tri-state result of a 3D dispatch (mirrors the g2d GPU_DONE/FAILED/
 * UNSUPPORTED contract; a submitted job is never replayed on the CPU). */
#define V3D_3D_DONE        0
#define V3D_3D_FAILED      1
#define V3D_3D_UNSUPPORTED 2
/* compiled-draw refinements of FAILED (the job was submitted): the compute
 * dispatch overran its wait budget / the GPU MMU faulted.  Mapped by the bsp
 * layer onto distinct negative client codes so the client log names the cause. */
#define V3D_3D_TIMEOUT     3
#define V3D_3D_MMU_FAULT   4

/* Depth-function codes for a draw (value-identical to the VC_PGL3D_DEPTH_*
 * set in vc_pgl3d.h and the G2D_PGL_DEPTH_* / PGL_G3D_DEPTH_* protocol sets).
 * NONE disables the depth test and leaves the tile-local Z buffer unallocated;
 * the rest select the V3D depth_test_function (Compare Function enum) and turn
 * on the Z buffer.  All eight GL compare functions are covered; depth_func_hw()
 * maps each code to the 3-bit Compare Function value (NEVER=0 .. ALWAYS=7). */
#define V3D_3D_DEPTH_NONE     0u
#define V3D_3D_DEPTH_LESS     1u
#define V3D_3D_DEPTH_LEQUAL   2u
#define V3D_3D_DEPTH_GREATER  3u
#define V3D_3D_DEPTH_GEQUAL   4u
#define V3D_3D_DEPTH_ALWAYS   5u
#define V3D_3D_DEPTH_NEVER    6u
#define V3D_3D_DEPTH_EQUAL    7u
#define V3D_3D_DEPTH_NOTEQUAL 8u

/* Back-face-cull configuration codes for a draw (carried in the cull_en
 * protocol field, value-identical across the layers).  0 draws both facings;
 * the rest select which facing(s) survive plus the front-face winding, decoded
 * into CFG_BITS bits 0-2 (forward facing / reverse facing / clockwise) in
 * build_draw_bcl.  FRONT_FACE_CCW is the GL default winding. */
#define V3D_3D_CULL_NONE       0u
#define V3D_3D_CULL_BACK_CCW   1u
#define V3D_3D_CULL_BACK_CW    2u
#define V3D_3D_CULL_FRONT_CCW  3u
#define V3D_3D_CULL_FRONT_CW   4u
#define V3D_3D_CULL_BOTH       5u

/* Maximum vertices per single draw dispatch (a pure bound check - the vertex
 * data itself lives in the caller's GPU-addressed staging, and the control-list
 * buffer is separately sized by V3D_3D_CL_MAX with its own length guard; larger
 * draws are split by the caller).  Raised to 20480 to match G2D_PGL_MAX_VERTS /
 * VC_PGL3D_MAX_VERTS so a whole rigid mesh can ride one dispatch.  The geometry
 * compute route (teapot) does not consult this cap - it is gated by its own
 * ntris < 2^16 workgroup limit. */
#define V3D_3D_MAX_VERTS 20480u

/* Built-in fixed-function fragment-shader ids.  Value-identical to the
 * VC_PGL3D_FS_* set exposed by vc_pgl3d.h and the G2D_PGL_FS_* protocol set.
 * A draw with tex == NULL uses SOLID (0) or GOURAUD (1); a draw with a
 * non-NULL v3d_3d_tex_t uses TEX_MOD (2) and ignores col_iova in favour of
 * uv_iova (see v3d_3d_tex_t). */
#define V3D_3D_FS_SOLID   0u
#define V3D_3D_FS_GOURAUD 1u
#define V3D_3D_FS_TEX_MOD 2u

/* Optional per-draw extended render state (Phase B2c): the scissor clip
 * window, polygon offset and the colour write mask.  A NULL ext (or an
 * all-zero ext) reproduces the historical full-frame / no-offset / write-all-
 * channels path, so every existing caller is unaffected.  Field semantics
 * mirror the g2d_pgl_ext_t protocol struct; the scissor rect arrives already
 * in the render target's y-down orientation (scissor_y = distance from the top
 * edge to the clip window's bottom row).  These drive CLIP_WINDOW (0x6B),
 * CFG_BITS bit3 + Depth Offset (0x6A) and COLOR_WRITE_MASKS (0x57); the exact
 * V3D 7.1 encodings are best-effort and confirmed on the Pi5. */
typedef struct {
    int32_t  scissor_x;     /* clip window left, pixels */
    int32_t  scissor_y;     /* clip window bottom, y down from the top edge */
    int32_t  scissor_w;     /* scissor_w/h <= 0 -> scissor off (full frame) */
    int32_t  scissor_h;
    uint8_t  poly_enable;   /* polygon offset applies to this primitive */
    float    poly_factor;   /* Depth Offset Factor (packed as f187) */
    float    poly_units;    /* Depth Offset Units (packed as f187) */
    uint32_t color_mask;    /* RGBA write-enable, GL order bit0=R..bit3=A;
                             * 0xF (or 0) = write all four channels */
} v3d_3d_ext_t;

/* Optional per-draw texture state (Phase D): a non-NULL tex selects the
 * point-sampled CLAMP-to-edge RGBA8 texture path.  The embedded tex vertex
 * shader emits (u,v) as two varyings (w=1.0 -> affine screen-space
 * interpolation, matching the validated gouraud path; perspective-correct
 * texturing is deferred to Pi5 hardware tuning since V3D 7.1 has no
 * reciprocal in the QPU).  The tex fragment shader reads a linear RGBA8
 * texel via the TMU (mov tmua / ldtmu), unpacks the byte channels, scales
 * them by 1/255 and vfpacks the result into the TLB.
 *
 * tex_iova is the GPU IOVA of the linear RGBA8 texture image (row pitch
 * tex_stride = tex_w * 4); uv_iova is the GPU IOVA of the per-vertex uv
 * float3 (stride 12, third component unused) which replaces col_iova in
 * attribute slot 1 for a TEX_MOD draw.  Both must be inside a memory range
 * v3d_g2d_phy_valid() accepts.  A NULL tex (or tex_w/tex_h == 0) keeps the
 * historical colour path unchanged. */
typedef struct {
    uint32_t tex_iova;      /* GPU IOVA of the linear RGBA8 texture image */
    uint32_t tex_stride;    /* texture row pitch in bytes (tex_w * 4)     */
    uint32_t tex_w, tex_h;  /* texture size in texels                     */
    uint32_t uv_iova;       /* GPU IOVA of per-vertex uv float3, stride 12 */
} v3d_3d_tex_t;

/* Maximum sampler texture units a compiled FS may bind (mirrors
 * VC_PGL3D_MAX_TEX_UNITS / G2D_PGL_MAX_TEX_UNITS). */
#define V3D_3D_MAX_TEX_UNITS 4u

/* Optional per-draw compiled-shader sampler bindings (WS4).  A compiled FS
 * references up to V3D_3D_MAX_TEX_UNITS texture units; each unit's
 * QUNIFORM_TMU_CONFIG_P0/P1 pair carries the unit index in the upper 8 bits
 * of its data word, so expand_uniforms extracts the slot and ORs the packed
 * template's low bits into the IOVA of a V3D71 TEXTURE_SHADER_STATE (P0) /
 * SAMPLER_STATE (P1) record that build_tex_state_records() packed from
 * ctex_iova[slot] / ctex_w[slot] / ctex_h[slot].
 * A slot with ctex_iova[i] == 0 is unbound: the FS is declined (it must not
 * sample an unbound unit).  Images are RGBA8 in the V3D texture memory
 * layout named by ctex_tiling[i] (V3D_3D_TEX_TILING_*; the TMU cannot sample
 * a raster 2D image, so the client tiles them), 64-byte aligned (the
 * texture_base_pointer field overlays bits 0-5 with flag bits; UIF layouts
 * need 4 KiB), with ctex_stride[i] >= ctex_w[i] * 4 (ctex_stride[i] == 0
 * lets the driver derive ctex_w[i] * 4) and inside a memory range
 * v3d_g2d_phy_valid() accepts.  A NULL ctex from the caller keeps every slot
 * unbound. */
#define V3D_3D_TEX_TILING_RASTER            0u
#define V3D_3D_TEX_TILING_LINEARTILE        1u
#define V3D_3D_TEX_TILING_UBLINEAR_1_COLUMN 2u
#define V3D_3D_TEX_TILING_UBLINEAR_2_COLUMN 3u
#define V3D_3D_TEX_TILING_UIF_NO_XOR        4u
#define V3D_3D_TEX_TILING_UIF_XOR           5u
typedef struct {
    uint32_t ctex_iova[V3D_3D_MAX_TEX_UNITS];
    uint32_t ctex_stride[V3D_3D_MAX_TEX_UNITS];
    uint32_t ctex_w[V3D_3D_MAX_TEX_UNITS];
    uint32_t ctex_h[V3D_3D_MAX_TEX_UNITS];
    uint8_t  ctex_tiling[V3D_3D_MAX_TEX_UNITS];
    uint8_t  ctex_ub_pad[V3D_3D_MAX_TEX_UNITS];
    /* Cubemap / 2D-array: ctex_depth[i] is the layer count (0/1 = plain 2D,
     * 6 = cubemap-as-2D-array); ctex_arrstride64[i] the per-layer tiled size
     * in 64-byte units (the record's array_stride_64_byte_aligned).  Both 0
     * for a plain 2D image, leaving its texture-state record byte-identical. */
    uint8_t  ctex_depth[V3D_3D_MAX_TEX_UNITS];
    uint32_t ctex_arrstride64[V3D_3D_MAX_TEX_UNITS];
    /* Complete mip count and independent mag/min/mip filters.  levels == 0
     * is normalized to one by the request decoder. */
    uint8_t  ctex_levels[V3D_3D_MAX_TEX_UNITS];
    uint8_t  ctex_filter[V3D_3D_MAX_TEX_UNITS];
    uint8_t  ctex_min_filter[V3D_3D_MAX_TEX_UNITS];
    uint8_t  ctex_mip_filter[V3D_3D_MAX_TEX_UNITS];
} v3d_3d_ctex_t;
#define V3D_3D_TEX_FILTER_NEAREST 0u
#define V3D_3D_TEX_FILTER_LINEAR  1u

/* Allocate dma staging and validate the GPU is usable for the 3D path.
 * Idempotent; requires v3d_g2d_ready().  Returns V3D_3D_DONE on success,
 * V3D_3D_UNSUPPORTED when the CSD bring-up is not ready or staging cannot be
 * allocated (the caller then keeps the CPU rasterizer). */
int v3d_3d_init(void);

/* Non-zero once v3d_3d_init() succeeded. */
int v3d_3d_ready(void);

/* Full-frame clear of the w x h render target at fb_iova (row pitch
 * stride_bytes, format fmt) to the packed rgba8 colour, then resolve every
 * tile to DRAM.  rb_swap swaps R/B on store (BGR565 onto an RGB565 panel).
 * Returns a V3D_3D_* tri-state. */
int v3d_3d_clear(uint32_t fb_iova, uint32_t stride_bytes, uint32_t fmt,
                 uint32_t w, uint32_t h, uint32_t rgba8, int rb_swap);

/* Draw `nverts` primitives of `mode` into the w x h render target at fb_iova.
 * Vertex positions (NDC -1..1, float3, stride 12) live at pos_iova and the
 * per-vertex RGBA8 colour (stride 4) at col_iova; both are GPU IOVAs of
 * dma/contig-shm memory.  The current shaders emit a solid `rgba` colour
 * (0xAABBGGRR); the coordinate+vertex shaders perform the viewport transform.
 * depth_func (V3D_3D_DEPTH_*), depth_mask (Z-write enable) and cull_en
 * (back-face cull) configure the depth test and facing; depth_func ==
 * V3D_3D_DEPTH_NONE reproduces the historical colour-only, no-cull path.
 *
 * Persistent depth / multi-dispatch frames: z_iova is the IOVA of a linear
 * Z32F depth surface (row pitch z_stride, size >= z_stride*h) that survives
 * across dispatches, or 0 for tile-local Z only.  first_frame != 0 marks the
 * first dispatch of a frame: it clears the tile buffer (colour to clear_rgba,
 * Z to clear_z) before rendering; first_frame == 0 LOADs the persisted colour
 * (+Z when z_iova != 0 and depth is on) so this dispatch accumulates onto the
 * earlier ones.  Every dispatch STOREs colour (+Z when persistent).
 *
 * ext carries the optional Phase B2c scissor / polygon-offset / colour-mask
 * state (NULL or all-zero = the historical full-frame, no-offset, write-all
 * path).
 *
 * WS4 compiled draw: prog_handle >= 0 selects a program uploaded via
 * v3d_3d_program (a mesa-compiled VS/FS QPU pair) instead of the built-in
 * fixed-function shaders; the V3D shader-state record is built from that
 * program's cached prog_data and its per-slot uniform stream is expanded by
 * the QUNIFORM interpreter from `unif` (num_uniforms u32 per-draw GL uniform
 * values indexed for QUNIFORM_UNIFORM slots, NULL when num_uniforms == 0).
 * The compiled path is scoped to the full-screen-quad case (pos-only vertex
 * attribute, the fixed coordinate shader reused for binning); prog_handle < 0
 * keeps the historical fixed-function fs_id path.  A program that carries a
 * COMPUTE stage (cs_qpu_words != 0) is instead dispatched through the CSD as a
 * full-frame compute grid writing fb directly (RGBA8 target, stride a multiple
 * of 4; pos/nverts/mode are ignored), bypassing the binner.
 * Returns a V3D_3D_* tri-state. */
int v3d_3d_draw(uint32_t fb_iova, uint32_t stride_bytes, uint32_t fmt,
                uint32_t w, uint32_t h, int mode, uint32_t nverts,
                uint32_t pos_iova, uint32_t col_iova,
                uint32_t rgba, int rb_swap,
                uint32_t depth_func, uint32_t depth_mask, uint32_t cull_en,
                uint32_t z_iova, uint32_t z_stride, int first_frame,
                uint32_t clear_rgba, float clear_z,
                const v3d_3d_ext_t *ext,
                const v3d_3d_tex_t *tex,
                int32_t prog_handle, const uint32_t *unif,
                uint32_t num_uniforms,
                const v3d_3d_ctex_t *ctex);

/* ------------------------------------------------------------------ */
/* Compiled-shader program upload (WS4).                                 */
/*                                                                       */
/* A compiled program (VS/FS QPU words + uniform streams + prog_data)     */
/* arrives from bsp_g3d via vc_pgl3d_program.  v3d_3d_program copies the   */
/* GPU-resident parts (QPU code + the static uniform value stream) into    */
/* dma through alloc_stage and caches the IOVAs + the CPU-side prog_data   */
/* (uniform layout descriptors, VPM sizes, attribute/input maps) in a      */
/* handle-indexed table, so a later draw can build the shader record and   */
/* per-draw uniform buffer from the handle alone.                          */
/* ------------------------------------------------------------------ */

/* Program-cache bounds.  The QPU words and uniform streams are deep-copied
 * into GPU dma / the inline cache; a program exceeding any cap is declined
 * (never truncated) so the caller falls back to its CPU path.  MAX_QPU is in
 * 32-bit words (2 words per V3D 7.1 QPU instruction). */
#define V3D_3D_MAX_PROGS       16u    /* simultaneously cached programs       */
#define V3D_3D_PROG_MAX_QPU    131072u /* QPU 32-bit words per stage (wave's
                                        * faithful ES310 ocean compiles to
                                        * ~22930 cs words; 4096 declined it).
                                        * Costless to raise: QPU text is dma-
                                        * allocated by ACTUAL word count.      */
#define V3D_3D_PROG_MAX_UNIF   16384u  /* uniform-stream words per stage (wave
                                        * cs embeds 1428; the faithful ED-209
                                        * shadertoy raymarcher folds its many
                                        * constants into fs 4278 / cs 4341, so
                                        * 2048 declined it -> CPU fallback;
                                        * 8192 keeps ~1.9x headroom.  Grows
                                        * _progs[16] BSS to ~5 MB (10 inline
                                        * MAX_UNIF arrays/slot) + 32 KB/stage
                                        * dma; V3D has no hw cap near this).   */
/* Solar's unchanged shader needs 74016 QPU words and 10521 uniform entries.
 * These bounds supersede the historical sizing notes above: the 16-slot
 * uniform cache now occupies 10 MiB plus metadata, with 64 KiB DMA per stage.
 * QPU storage remains allocated by actual word count; over-cap uploads decline. */
#define V3D_3D_PROG_MAX_ATTRS  64u    /* VS attribute / driver-location slots */
#define V3D_3D_PROG_MAX_INPUTS 64u    /* FS input (varying) slots             */

/* Program-upload result codes.  A non-negative return is the program handle
 * (an index into the cache); the two negatives are value-identical to the
 * G2D_ERR_FAILED / G2D_ERR_NOT_SUPPORTED codes so bsp_g3d forwards them
 * verbatim through the g2d dev-cntl reply. */
#define V3D_3D_PROG_FAILED      (-1)
#define V3D_3D_PROG_UNSUPPORTED (-2)

/* Compiled-program descriptor handed to v3d_3d_program by vc_pgl3d (filled by
 * bsp_g3d from the parsed g2d upload blob).  Every pointer is CPU-side and
 * need only stay valid for the duration of the call; the QPU words and the
 * uniform value streams are deep-copied into GPU dma, and the uniform layout
 * descriptors + attribute/input maps are copied into the inline cache.  The
 * shape mirrors the public vc_pgl3d_program_req_t field-for-field. */
typedef struct {
    /* vertex stage */
    const uint32_t *vs_qpu;             uint32_t vs_qpu_words;
    const uint32_t *vs_unif_contents;   /* per-slot layout descriptor (CPU)  */
    const uint32_t *vs_unif_data;       uint32_t vs_num_uniforms;
    uint8_t  vs_threads;
    uint8_t  vs_single_seg;
    uint32_t vs_vpm_input_size;
    uint32_t vs_vpm_output_size;
    uint32_t vs_vcm_cache_size;
    uint8_t  vs_separate_segments;
    uint8_t  vs_vattr_sizes[V3D_3D_PROG_MAX_ATTRS / 4];
    int32_t  vs_driver_location_map[V3D_3D_PROG_MAX_ATTRS];
    /* fragment stage */
    const uint32_t *fs_qpu;             uint32_t fs_qpu_words;
    const uint32_t *fs_unif_contents;
    const uint32_t *fs_unif_data;       uint32_t fs_num_uniforms;
    uint8_t  fs_threads;
    uint8_t  fs_single_seg;
    uint8_t  fs_needs_coords;
    uint8_t  fs_uses_center_w;
    uint8_t  fs_writes_z;
    uint8_t  fs_disable_ez;
    uint32_t fs_num_inputs;
    uint32_t fs_input_slots[V3D_3D_PROG_MAX_INPUTS];
    /* coordinate (binning) stage: the same VS NIR compiled with is_coord=true.
     * Carries its own QPU + uniform stream and VPM layout (gl_Position only, no
     * varyings, so its VPM output size differs from the render VS's).  Shares
     * the VS's uniform driver_locations, so one per-draw blob feeds both. */
    const uint32_t *coord_qpu;          uint32_t coord_qpu_words;
    const uint32_t *coord_unif_contents;
    const uint32_t *coord_unif_data;    uint32_t coord_num_uniforms;
    uint8_t  coord_threads;
    uint8_t  coord_single_seg;
    uint32_t coord_vpm_input_size;
    uint32_t coord_vpm_output_size;
    uint32_t coord_vcm_cache_size;
    uint8_t  coord_separate_segments;
    /* optional COMPUTE stage (the shadertoy compute route): the fragment
     * program recompiled as a compute kernel that writes ARGB8888 straight into
     * the frame buffer through an SSBO (binding 0 = the render target).  A
     * program with cs_qpu_words == 0 has no compute stage and draws through the
     * binner.  cs_local_size is the kernel's workgroup shape; the dispatch grid
     * is ceil(w / x) x ceil(h / y) x 1 (z must be 1). */
    const uint32_t *cs_qpu;             uint32_t cs_qpu_words;
    const uint32_t *cs_unif_contents;
    const uint32_t *cs_unif_data;       uint32_t cs_num_uniforms;
    uint8_t  cs_threads;
    uint8_t  cs_single_seg;
    uint32_t cs_local_size[3];
    uint32_t cs_shared_size;
    uint32_t cs_spill_size;   /* bytes/thread of register-spill scratch (0 = none);
                               * the daemon allocates qpu_count*4*this as a TMU-general
                               * scratch DMA buffer and resolves QUNIFORM_SPILL_OFFSET /
                               * QUNIFORM_SPILL_SIZE_PER_THREAD from it. */
    /* Non-zero when the compute stage is one of the fixed two-pass GEOMETRY
     * rasterizer kernels (geom_raster_cs.h) rather than a shadertoy full-frame
     * effect: 1 = gouraud/colour (SSBO 0=fb 1=pos 2=nrm 3=depth), 2 = textured
     * (1 = interleaved pos+uv, 3 = texture), 3 = instanced (gears), 4 = colour-
     * modulated textured (imgui: 1 = interleaved pos+uv+rgba stride-10, 3 = the
     * linear font atlas; the raster pass alpha-blends and clips via a uScissor
     * uniform with no depth write, and vout grows to a 12-float record).  Geom
     * programs dispatch grid (ceil(ntris/cs_geom_tpw),1,1) and take the draw's
     * pos/col/z iovas as SSBOs. */
    uint32_t cs_is_geom;
    /* Raster-pass triangles-per-workgroup for geom programs: 0 = the legacy
     * one-workgroup-per-triangle grid; otherwise draw_compute_geom dispatches
     * ceil(ntris/cs_geom_tpw) workgroups.  Must be 0 or equal to the cs
     * workgroup lane count (one-lane-per-triangle kernels cover exactly one
     * triangle per lane; validated at program upload).  The wide grid is the
     * performance fix: the V3D 7.1 CSD charges ~3us fixed overhead per
     * workgroup, so 6400 one-triangle workgroups cost ~19-20ms while 25
     * 256-triangle workgroups cost well under 1ms. */
    uint32_t cs_geom_tpw;
    /* second COMPUTE stage (VERSION 4): the VERTEX-TRANSFORM pass (pass 1) of
     * the two-pass geometry rasterizer.  cs2_qpu_words == 0 means absent (a
     * single-pass or non-geom program).  cs2 runs FIRST (one invocation per
     * vertex), writing a packed per-vertex record into the daemon-allocated
     * `vout` SSBO; cs then rasterises from vout (one workgroup per triangle).
     * cs2_uniform_blob_dwords is the split of the per-draw uniform blob: the
     * leading dwords belong to cs2, the remainder to cs. */
    const uint32_t *cs2_qpu;            uint32_t cs2_qpu_words;
    const uint32_t *cs2_unif_contents;
    const uint32_t *cs2_unif_data;      uint32_t cs2_num_uniforms;
    uint8_t  cs2_threads;
    uint8_t  cs2_single_seg;
    uint32_t cs2_local_size[3];
    uint32_t cs2_shared_size;
    uint32_t cs2_uniform_blob_dwords;
    /* VERSION 6 geom_kind == 5 (Vulkan user shader) per-vertex stream strides
     * in FLOATS: cs2_vout_stride is the pass-1 vout record stride (the daemon
     * sizes the pass-1 dst_span = nverts * cs2_vout_stride * 4), vk_attr_stride
     * the attribute stream stride (the attr canvas is validated as
     * nverts * vk_attr_stride * 4).  Zero / ignored for every other kind. */
    uint32_t cs2_vout_stride;
    uint32_t vk_attr_stride;
} v3d_3d_prog_desc_t;

/* Upload a compiled program: copy the VS/FS QPU words and the static uniform
 * value streams into GPU dma (alloc_stage), cache the resulting IOVAs plus the
 * prog_data (uniform layout, VPM sizes, attribute/input maps, FS flags) in the
 * handle table, and return the non-negative program handle.  Returns
 * V3D_3D_PROG_UNSUPPORTED when the 3D path is not ready, a payload is out of
 * range, or the cache is full; V3D_3D_PROG_FAILED on a dma allocation failure.
 * A submitted upload is idempotent per call (each call consumes a new slot). */
int v3d_3d_program(const v3d_3d_prog_desc_t *desc);

/* Drop a cached program (free its slot and dma QPU blocks).  Returns 0, or
 * V3D_3D_PROG_UNSUPPORTED for an unknown / already-free handle. */
int v3d_3d_program_free(int prog_handle);

/* ------------------------------------------------------------------ */
/* OpenCL compute dispatch (the vc_cl route).                            */
/*                                                                       */
/* A CL kernel is uploaded through v3d_3d_program as a pure-compute (cs    */
/* only) program, exactly like the shadertoy compute route, and dispatched */
/* here.  This generalises draw_compute: instead of one frame-buffer SSBO  */
/* and a frame-derived grid, the caller supplies the workgroup grid        */
/* (wg_count / wg_size) and the table of __global buffer SSBO IOVAs the    */
/* kernel's QUNIFORM_SSBO_OFFSET slots index by binding.  The private      */
/* QUNIFORM interpreter (expand_uniforms), the shared expanded-uniform /   */
/* job-uniform dma staging and the CSD submission + cache maintenance are  */
/* all reused verbatim, so a CL launch is byte-for-byte the proven compute */
/* path with a caller-owned SSBO table.  `job_unif` (num_job_unif u32) is  */
/* the packed scalar / __constant argument blob the QUNIFORM_UNIFORM and   */
/* QUNIFORM_UBO_ADDR slots read; NULL when num_job_unif == 0.  dst_phy /   */
/* dst_span bound the POST L2 writeback over the hull of every argument    */
/* buffer the launch may write (0 = the full-range fallback walk).         */
/* Returns a V3D_3D_* tri-state; a submitted launch is never replayed.     */
/* ------------------------------------------------------------------ */
int v3d_3d_cl_dispatch(int32_t prog_handle,
                       const uint32_t wg_count[3], uint32_t wg_size,
                       const uint32_t *ssbo_iova, uint32_t num_ssbo,
                       const uint32_t *job_unif, uint32_t num_job_unif,
                       uint32_t dst_phy, uint32_t dst_span);

#endif /* V3D_3D_H */
