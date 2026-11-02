/*
 * v3d_3d_min.c - V3D 7.1 3D 管线的最小实现（只做「画三角形」）。
 *
 * 参照 EwokOS 的 v3d_3d.c（3186 行）重写，砍掉了这些不需要的部分：
 *   纹理 / cubemap、compiled shader（GLSL→QPU）、instancing、
 *   scissor / polygon offset / color mask、逐 tag 统计、多程序槽。
 *
 * 保留的部分（都是逆向成果，无法重写）：
 *   - 3 套手写 QPU shader 的机器码（frag / vtx / coord）
 *   - CLE 命令流的编码序列（bin 阶段 + render 阶段）
 *   - shader-state record / vertex-attribute record 的布局
 *
 * 整体流程：
 *   init  : 分配 6 块 DMA（CL / tile 池 / tile-state / shader record /
 *           QPU 代码 / uniform），装载 shader
 *   draw  : 写 uniform → 建 shader record + attr record → 建 bin CL 并提交
 *           → 建 render CL 并提交
 *
 * 顶点格式：pos = float3（stride 12）、col = RGBA byte4。
 * 坐标变换由 vtx shader 做：NDC [-1,1] → 屏幕 [0,w]×[0,h]（y 向下）。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "v3d_3d.h"
#include "v3d_3d_compat.h"

/* ---- 常量（照 EwokOS，寄存器偏移都是逆向出来的） ---- */
#define CLE_CT0QMA   0x170u   /* bin queue message (tile-alloc pool) address */
#define CLE_CT0QMS   0x174u   /* bin queue message size                      */
#define CLE_CT0QTS   0x15cu   /* bin queue tile-state address | enable       */
#define CLE_CT0QBA   0x160u   /* bin queue base address                      */
#define CLE_CT0QEA   0x168u   /* bin queue end address (write starts the job)*/
#define CLE_CT1QBA   0x164u   /* render queue base address                   */
#define CLE_CT1QEA   0x16cu   /* render queue end address                    */
#define CLE_CT1QCFG  0x178u   /* render queue config（勿写：Linux 保留默认） */
#define CLE_CT1CS    0x104u   /* control thread 1 (render) status            */
#define CLE_BFC      0x134u   /* binning flush count                         */
#define CLE_RFC      0x138u   /* render flush count                          */
#define PTB_BPOS     0x30cu   /* binning pool overspill size (kernel zeroes) */
#define PTB_BPCA     0x300u   /* binning pool current address (write cursor) */
#define PTB_BPCS     0x304u   /* binning pool current size                   */
#define CLE_CT0CS    0x100u   /* control thread 0 (binning) status           */
#define CLE_CT0CA    0x110u   /* control thread 0 current CL address         */
#define CLE_CT0RA    0x118u   /* control thread 0 read address               */
#define CLE_CT1CA    0x114u   /* control thread 1 current CL address         */
#define CLE_CT1RA    0x11cu   /* control thread 1 read address               */
#define CLE_CT0LC    0x120u   /* control thread 0 list counter               */
#define CLE_CT1LC    0x124u   /* control thread 1 list counter               */
#define CLE_CT0PC    0x128u   /* control thread 0 prim counter               */
#define V3D_ERR_STAT 0xf20u   /* core error status                           */
#define V3D_INT_STS  0x50u    /* interrupt status (flush-complete/error bits)*/
#define V3D_3D_CL_MAX      (64u * 1024u)   /* control-list buffer           */
#define V3D_3D_POOL_SIZE   (256u * 1024u)  /* binning tile-alloc pool       */
#define V3D_3D_TSDA_SIZE   (64u * 1024u)   /* tile-state data array         */
#define V3D_3D_SSR_SIZE    64u             /* 32 shader-rec + 2*16 attr-rec */
#define V3D_3D_SHADER_WORDS 160u           /* staged frag+vtx+coord + tex frag/vtx QPU (127 used) */
#define V3D_3D_UNIF_WORDS  8u              /* vtx/frag uniforms             */
#define V3D_3D_GEN_SIZE    256u            /* generic tile list             */
#define V3D_3D_COMPUTE_BUDGET_MS 2000u     /* compiled compute draw wait    */
#define V3D_3D_WAIT_MS     1000u
#define SH_FLAG_4WAY       (1u << 0)
#define SH_FLAG_FINAL_SEG  (1u << 1)
#define SH_FLAG_PROP_NANS  (1u << 2)


/* ============================ 手写 QPU shader ============================
 * 这三段是 EwokOS 在真机上验证过的机器码，原样保留。
 * 改动其中任何一个字都会让 3D 管线失效。
 */

/* Vertex shader: viewport transform + pass-through of the rgba attribute.
 * VPM input in[0..2]=pos.xyz, in[3..6]=rgba.  Uniforms [0]=w [1]=xscale
 * [2]=yscale [3]=zscale [4]=zoff.  VPM output out[0]=int(X*xscale)
 * out[1]=int(Y*yscale) out[2]=Z*zscale+zoff out[3]=w out[4..7]=rgba. */
static const uint64_t _frag_shader[] = {
    0x39003186BB03F000ull, /* nop            ; ldvary rf0   (varying 0 = R) */
    0x39007186BB03F000ull, /* nop            ; ldvary rf1   (varying 1 = G) */
    0x3900B186BB03F000ull, /* nop            ; ldvary rf2   (varying 2 = B) */
    0x3900F186BB03F000ull, /* nop            ; ldvary rf3   (varying 3 = A) */
    0x380031873903F001ull, /* vfpack tlb,rf0,rf1 ; nop      (R,G -> TLB)    */
    0x380031873903F083ull, /* vfpack tlb,rf2,rf3 ; nop      (B,A -> TLB)    */
    0x38203186BB03F000ull, /* nop            ; thrsw                        */
    0x38000000bb03f000ull, /* nop            ; nop   (thrend delay slot 1)  */
    0x38000000bb03f000ull, /* nop            ; nop   (thrend delay slot 2)  */
};

static const uint64_t _vtx_shader[] = {
    0x39C02180BC03F000ull, /* ldvpmv_in rf0,0        ; nop */
    0x39C02181BC03F040ull, /* ldvpmv_in rf1,1        ; nop */
    0x39C02182BC03F080ull, /* ldvpmv_in rf2,2        ; nop */
    0x39C02183BC03F0C0ull, /* ldvpmv_in rf3,3        ; nop */
    0x3981F186BB03F000ull, /* nop                    ; ldunifrf rf7 (w) */
    0x39C02184BC03F100ull, /* ldvpmv_in rf4,4        ; nop */
    0x39823186BB03F000ull, /* nop                    ; ldunifrf rf8 (xscale) */
    0x39C02185BC03F140ull, /* ldvpmv_in rf5,5        ; nop */
    0x39827186BB03F000ull, /* nop                    ; ldunifrf rf9 (yscale) */
    0x39C02186BC03F180ull, /* ldvpmv_in rf6,6        ; nop */
    0x3982B186BB03F000ull, /* nop                    ; ldunifrf rf10 (zscale) */
    0x40001306BB008000ull, /* nop                    ; fmul rf12,rf0,rf8 */
    0x3982F186BB03F000ull, /* nop                    ; ldunifrf rf11 (zoff) */
    0x40001346BB049000ull, /* nop                    ; fmul rf13,rf1,rf9 */
    0x3800218EF503F307ull, /* ftoin rf14,rf12        ; nop (Xs) */
    0x400013C6BB08A000ull, /* nop                    ; fmul rf15,rf2,rf10 */
    0x38002190F503F347ull, /* ftoin rf16,rf13        ; nop (Ys) */
    0x380021910503F3CBull, /* fadd rf17,rf15,rf11    ; nop (Zs) */
    0x39C02180BE03F00Eull, /* stvpmv 0,rf14          ; nop (Xs) */
    0x39C02180BE03F050ull, /* stvpmv 1,rf16          ; nop (Ys) */
    0x39C02180BE03F091ull, /* stvpmv 2,rf17          ; nop (Zs) */
    0x39C02180BE03F0C7ull, /* stvpmv 3,rf7           ; nop (w) */
    0x39C02180BE03F103ull, /* stvpmv 4,rf3           ; nop (R) */
    0x39C02180BE03F144ull, /* stvpmv 5,rf4           ; nop (G) */
    0x39C02180BE03F185ull, /* stvpmv 6,rf5           ; nop (B) */
    0x39C02180BE03F1C6ull, /* stvpmv 7,rf6           ; nop (A) */
    0x38203186BB03F000ull, /* nop                    ; thrsw */
    0x38000000bb03f000ull, /* nop                    ; nop (thrend delay slot 1) */
    0x38000000bb03f000ull, /* nop                    ; nop (thrend delay slot 2) */
};

/* Coordinate shader: the binner's per-vertex position shader.  VPM input
 * in[0..2]=pos.xyz.  Uniforms [0]=W(1.0) [1]=xscale [2]=yscale.  VPM output
 * out[0]=X out[1]=Y out[2]=Z out[3]=W out[4]=int(X*xscale) out[5]=int(Y*yscale). */
static const uint64_t _coord_shader[] = {
    0x39C02180BC03F000ull, /* ldvpmv_in rf0,0        ; nop (X) */
    0x39C02181BC03F040ull, /* ldvpmv_in rf1,1        ; nop (Y) */
    0x39C02182BC03F080ull, /* ldvpmv_in rf2,2        ; nop (Z) */
    0x3980F186BB03F000ull, /* nop                    ; ldunifrf rf3 (W) */
    0x39813186BB03F000ull, /* nop                    ; ldunifrf rf4 (xscale) */
    0x39C02180BE03F000ull, /* stvpmv 0,rf0           ; nop (X) */
    0x39C02180BE03F041ull, /* stvpmv 1,rf1           ; nop (Y) */
    0x39817186BB03F000ull, /* nop                    ; ldunifrf rf5 (yscale) */
    0x39C02180BE03F082ull, /* stvpmv 2,rf2           ; nop (Z) */
    0x39C02180BE03F0C3ull, /* stvpmv 3,rf3           ; nop (W) */
    0x40001186BB004000ull, /* nop                    ; fmul rf6,rf0,rf4 */
    0x38002187F503F187ull, /* ftoin rf7,rf6          ; nop (Xs) */
    0x40001206BB045000ull, /* nop                    ; fmul rf8,rf1,rf5 */
    0x39C02180BE03F107ull, /* stvpmv 4,rf7           ; nop (Xs) */
    0x38002189F503F207ull, /* ftoin rf9,rf8          ; nop (Ys) */
    0x39C02180BE03F149ull, /* stvpmv 5,rf9           ; nop (Ys) */
    0x38203186BB03F000ull, /* nop                    ; thrsw */
    0x38000000bb03f000ull, /* nop                    ; nop (thrend delay slot 1) */
    0x38000000bb03f000ull, /* nop                    ; nop (thrend delay slot 2) */
};

/* ---------------------------- 运行状态 ---------------------------- */
static int _ready;

static uint8_t* _cl_va;
static uint32_t _cl_p;
static uint8_t* _pool_va;
static uint32_t _pool_p;
static uint8_t* _tsda_va;
static uint32_t _tsda_p;
static uint8_t* _ssr_va; /* shader record(32) + 属性 record(2×16) */
static uint32_t _ssr_p;
static uint64_t* _shaders_va;
static uint32_t _shaders_p;
static uint32_t* _unif_va; /* vtx / frag 共用 */
static uint32_t _unif_p;
static uint32_t* _cs_unif_va; /* coord（binning）shader 的 uniform */
static uint32_t _cs_unif_p;

static uint32_t _frag_iova;
static uint32_t _vtx_iova;
static uint32_t _coord_iova;

/* generic tile list：render CL 里的 SUB_LIST 指向它 */
static uint8_t* _gen_va;
static uint32_t _gen_p;

/* 当前作业描述（超时日志里打出来，便于定位是哪次 draw） */
static char _job_desc[128];

/* 前向声明：wait_flush 里会调用，定义在后面 */
static void stalled(const char* why);
static void gen_emit_store_load(uint8_t* gp, uint32_t* g, uint8_t opcode,
                                uint32_t buffer, uint32_t imgfmt, int rb_swap,
                                uint32_t stride, uint32_t addr);

/* -------------------- DMA 分配（identity 映射） -------------------- */

/* 【identity arena 的 cache 一致性】EwokOS 的 dma_alloc 是 uncached，
 * CPU 写直接落 DRAM；我们的 arena 是 cached，必须显式 clean。
 * 参数是 GPU VA，内部换算成 CPU 侧地址再做 cache 操作。 */
static void dma_clean(uint32_t iova, uint32_t len) {
  void* va = v3d_dma_cpu(iova);
  foot_cache_clean(va, (void*)((char*)va + len));
  __asm__ __volatile__("dsb sy" ::: "memory");
}

static int alloc_stage(uint32_t size, void** va, uint32_t* iova) {
  uint32_t gva = v3d_dma_alloc(4096u, size);
  if (gva == 0) {
    return -1;
  }
  *iova = gva;              /* GPU VA，给 shader record / CL 引用用 */
  *va = v3d_dma_cpu(gva);   /* CPU 侧地址，读写走这里 */
  memset(*va, 0, size);
  dma_clean(gva, size);
  return 0;
}
static uint32_t cl_u8(uint32_t off, uint8_t v)
{
    if (off < V3D_3D_CL_MAX)
        _cl_va[off] = v;
    return off + 1;
}

static uint32_t cl_u16le(uint32_t off, uint32_t v)
{
    if (off + 1 < V3D_3D_CL_MAX) {
        _cl_va[off]     = (uint8_t)(v & 0xFF);
        _cl_va[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    }
    return off + 2;
}

static uint32_t cl_u32le(uint32_t off, uint32_t v)
{
    if (off + 3 < V3D_3D_CL_MAX) {
        _cl_va[off]     = (uint8_t)(v & 0xFF);
        _cl_va[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        _cl_va[off + 2] = (uint8_t)((v >> 16) & 0xFF);
        _cl_va[off + 3] = (uint8_t)((v >> 24) & 0xFF);
    }
    return off + 4;
}

static uint32_t cl_f32le(uint32_t off, float v)
{
    uint32_t u;
    __builtin_memcpy(&u, &v, 4);
    return cl_u32le(off, u);
}

static uint16_t cl_f187(float v)
{
    uint32_t u;
    __builtin_memcpy(&u, &v, 4);
    return (uint16_t)((((u >> 31) & 1u) << 15) |
                      (((u >> 23) & 0xFFu) << 7) |
                      ((u >> 16) & 0x7Fu));
}

static uint32_t sh_flags(uint32_t threads, uint32_t single_seg)
{
    return (threads == 4u ? SH_FLAG_4WAY : 0u) |
           (single_seg ? SH_FLAG_FINAL_SEG : 0u) |
           SH_FLAG_PROP_NANS;
}

static void build_shader_record(uint32_t frag_addr, uint32_t frag_unif,
                                uint32_t frag_flags,
                                uint32_t vtx_addr, uint32_t vtx_unif,
                                uint32_t vtx_flags,
                                uint32_t coord_addr, uint32_t coord_unif,
                                uint32_t coord_flags,
                                uint32_t varyings)
{
    uint8_t *r = _ssr_va;                 /* bytes [0..31] */

    memset(r, 0, 32);
    r[0] = (1u << 1);                     /* enable_clipping */
    r[1] = (1u << 4);                     /* real_pixel_centre_w */
    r[2] = (1u << 2);                     /* disable_implicit_point_line_varyings */
    r[3] = (uint8_t)(varyings & 0x1Fu);   /* varyings into fragment shader (4 gouraud, 2 tex) */
    /* VPM segment sizes (7.1): coord/vtx input sizes 0 (shared with output),
     * high nibbles = min segments in play (As=1, Ve=0). */
    r[4] = 0x01;                          /* coord out segs=1, Ve=0 */
    r[5] = 0x10;                          /* coord in segs=0, As=1  */
    r[6] = 0x01;                          /* vtx  out segs=1, Ve=0  */
    r[7] = 0x10;                          /* vtx  in segs=0, As=1   */
    r[8]  = (uint8_t)((frag_addr & 0xF8) | (frag_flags & 0x07u));
    r[9]  = (uint8_t)((frag_addr >> 8) & 0xFF);
    r[10] = (uint8_t)((frag_addr >> 16) & 0xFF);
    r[11] = (uint8_t)((frag_addr >> 24) & 0xFF);
    r[12] = (uint8_t)(frag_unif & 0xFF);
    r[13] = (uint8_t)((frag_unif >> 8) & 0xFF);
    r[14] = (uint8_t)((frag_unif >> 16) & 0xFF);
    r[15] = (uint8_t)((frag_unif >> 24) & 0xFF);
    r[16] = (uint8_t)((vtx_addr & 0xF8) | (vtx_flags & 0x07u));
    r[17] = (uint8_t)((vtx_addr >> 8) & 0xFF);
    r[18] = (uint8_t)((vtx_addr >> 16) & 0xFF);
    r[19] = (uint8_t)((vtx_addr >> 24) & 0xFF);
    r[20] = (uint8_t)(vtx_unif & 0xFF);
    r[21] = (uint8_t)((vtx_unif >> 8) & 0xFF);
    r[22] = (uint8_t)((vtx_unif >> 16) & 0xFF);
    r[23] = (uint8_t)((vtx_unif >> 24) & 0xFF);
    r[24] = (uint8_t)((coord_addr & 0xF8) | (coord_flags & 0x07u));
    r[25] = (uint8_t)((coord_addr >> 8) & 0xFF);
    r[26] = (uint8_t)((coord_addr >> 16) & 0xFF);
    r[27] = (uint8_t)((coord_addr >> 24) & 0xFF);
    r[28] = (uint8_t)(coord_unif & 0xFF);
    r[29] = (uint8_t)((coord_unif >> 8) & 0xFF);
    r[30] = (uint8_t)((coord_unif >> 16) & 0xFF);
    r[31] = (uint8_t)((coord_unif >> 24) & 0xFF);
}

static void build_attr_records(uint32_t pos_iova, uint32_t col_iova)
{
    uint8_t *a = _ssr_va + 32;            /* bytes [32..63] */

    memset(a, 0, 32);
    a[0]  = (uint8_t)(pos_iova & 0xFF);
    a[1]  = (uint8_t)((pos_iova >> 8) & 0xFF);
    a[2]  = (uint8_t)((pos_iova >> 16) & 0xFF);
    a[3]  = (uint8_t)((pos_iova >> 24) & 0xFF);
    a[4]  = (3u << 0) | (2u << 2);        /* vec_size=3, type=FLOAT */
    a[5]  = (3u << 4) | (3u << 0);        /* nvals vs=3, cs=3 */
    a[8]  = 12;                           /* stride = 12 bytes */
    a[12] = 0xFF; a[13] = 0xFF; a[14] = 0xFF; a[15] = 0xFF;  /* max index */
    a[16] = (uint8_t)(col_iova & 0xFF);
    a[17] = (uint8_t)((col_iova >> 8) & 0xFF);
    a[18] = (uint8_t)((col_iova >> 16) & 0xFF);
    a[19] = (uint8_t)((col_iova >> 24) & 0xFF);
    a[20] = (0u << 0) | (4u << 2) | (1u << 6);  /* vec_size=0, BYTE, normalized */
    a[21] = (0u << 0) | (4u << 4);              /* nvals vs=4, cs=0 */
    a[24] = 4;                            /* stride = 4 bytes */
    a[28] = 0xFF; a[29] = 0xFF; a[30] = 0xFF; a[31] = 0xFF;
}

static int wait_flush(volatile uint32_t *core, uint32_t reg, uint32_t before)
{
    /* 【不用时钟】用户态读 cntpct_el0 会触发异常（实测整个 gpud 卡死、
     * 连 stalled() 的日志都出不来）。改成纯计数自旋：
     *   每次 yield 约 1~2us（单核被让出再回来），500 万次 ≈ 5~10s，
     *   比原 1s 预算宽松，保证 bin/render 有足够时间跑完。 */
    for (uint32_t i = 0; i < 5000000u; i++) {
        if (core[reg / 4] != before)
            return 0;
        __asm__ volatile("yield");
    }
    return -1;
}

static int submit_bin(volatile uint32_t *core, uint32_t start, uint32_t end)
{
    uint32_t before = core[CLE_BFC / 4];
    /* ERR_STAT is a sticky latch the port never clears.  Writing ~0 to it (the
     * VC4/pi3 reference idiom) is NOT safe on VC7: the VC7 reference only ever
     * READS 0xf20, and adding a ~0 write here made even the empty-clear bin
     * time out (pgl_g3d probe-clear -> "init FAIL probe-clear").  Detect a
     * FRESH error by read-only difference instead -- snapshot before the ring,
     * and after a timeout report only the bits that appeared during this bin. */
    uint32_t err_before = core[V3D_ERR_STAT / 4];
    int rc;

    /* 清 bin 线程的 boot-time halt —— 与 render 侧的 CT1CS=0 同理
     * （裸金属 bring-up 的 v3d_submit_bin 也写 CT0CS=0）。不写的话
     * CT0 从上电起就 parked，第一个命令都取不出来。 */
    core[CLE_CT0CS / 4] = 0;
    core[CLE_CT0QMA / 4] = _pool_p;
    core[CLE_CT0QMS / 4] = V3D_3D_POOL_SIZE;
    core[CLE_CT0QTS / 4] = (1u << 1) | _tsda_p;   /* enable | page-aligned tsda */
    core[PTB_BPOS / 4]   = 0;                     /* kernel zeroes overspill */
    __asm__ __volatile__("dsb sy" ::: "memory");
    v3d_g2d_l2_invalidate_all();
    core[CLE_CT0QBA / 4] = start;
    core[CLE_CT0QEA / 4] = end;                   /* write starts the job */
    rc = wait_flush(core, CLE_BFC, before);
    if (rc != 0) {
        uint32_t mctl = 0, mvio = 0, mid = 0, mint = 0;
        int mf = v3d_g2d_mmu_fault_peek(&mctl, &mvio, &mid, &mint);
        slog("v3d_3d: BIN timeout bfc %u->%u intsts 0x%x ct0cs 0x%x ct1cs 0x%x bpos 0x%x cl [%u,%u)\r\n",
             before, core[CLE_BFC / 4], core[V3D_INT_STS / 4],
             core[CLE_CT0CS / 4], core[CLE_CT1CS / 4], core[PTB_BPOS / 4],
             start, end);
        /* bin control-thread position: CT0CA/CT0RA = where in the CL it is
         * stuck, CT0PC = primitives processed, ERR_STAT = core error latch. */
        slog("v3d_3d: BIN ct0ca 0x%x ct0ra 0x%x ct0lc 0x%x ct0pc 0x%x err 0x%x\r\n",
             core[CLE_CT0CA / 4], core[CLE_CT0RA / 4], core[CLE_CT0LC / 4],
             core[CLE_CT0PC / 4], core[V3D_ERR_STAT / 4]);
        /* MMU fault state: fault=1 + vio_id client 0x30-0x37=CLE / 0x38=PTB
         * proves the binner hit an unmapped IOVA (vio_addr); fault=0 means the
         * coord-shader QPU stalled with the mapping intact. */
        slog("v3d_3d: BIN mmu fault=%d ctl=0x%x vio_addr=0x%x vio_id=0x%x hub_int=0x%x\r\n",
             mf, mctl, mvio, mid, mint);
        /* Binner pool write cursor: bpca advanced past the pool base (_pool_p)
         * means the binner emitted tile lists (coord shader produced binnable
         * vertices); bpca == base means it wrote nothing.  err is shown
         * before->after with new = bits that appeared during THIS bin (sticky
         * bits already set before the ring are masked out), so a new bit12
         * (VCDI) proves this bin faulted rather than inheriting a stale latch. */
        slog("v3d_3d: BIN pool base 0x%x bpca 0x%x bpcs 0x%x err %x->%x new %x\r\n",
             _pool_p, core[PTB_BPCA / 4], core[PTB_BPCS / 4],
             err_before, core[V3D_ERR_STAT / 4],
             core[V3D_ERR_STAT / 4] & ~err_before);
        /* CT0 is stuck mid-job: reset the block so later dispatches (2D/CSD
         * included) do not wait out their budget behind it. */
        stalled("bin timeout");
    }
    return rc;
}

static int submit_render(volatile uint32_t *core, uint32_t start, uint32_t end)
{
    uint32_t before = core[CLE_RFC / 4];
    int rc;

    v3d_g2d_l2_invalidate_all();
    /* 【不写 CT1QCFG】原 EwokOS 把 0x178 标成 CT1CFG 并写 0，实际 0x178 是
     * V3D_CLE_CT1QCFG（队列配置）——写 0 清掉了硬件上电默认值，CT1 从此
     * 一个字节都取不出来（probe consumed=0、ct1cs CTERR|CTSUBS）。
     * Linux 对这个寄存器的处理是 "XXX: Set the QCFG"（保留默认）。
     * 真正的 CT1CFG 在 0x144，本路径无需写它。 */
    /* Clear a boot-time halt on the render CLE thread before ringing CT1.
     * The proven bare-metal bring-up (videocore7-exps/app/src/v3d.c,
     * v3d_submit_render) writes CT1CS = 0 at exactly this point; the EwokOS
     * port dropped it.  Without the write the render control thread stays
     * parked from boot, the CT1 job never advances and CLE_RFC never
     * increments, so wait_flush burns its whole deadline and every dispatch
     * (even the shader-less probe clear) degrades to V3D_3D_FAILED -- which
     * latches the client's GPU path off and drops shadertoy to the CPU. */
    core[CLE_CT1CS / 4] = 0;
    __asm__ __volatile__("dsb sy" ::: "memory");
    core[CLE_CT1QBA / 4] = start;
    core[CLE_CT1QEA / 4] = end;
    rc = wait_flush(core, CLE_RFC, before);
    if (rc != 0) {
        slog("v3d_3d: RENDER timeout rfc %u->%u intsts 0x%x ct0cs 0x%x ct1cs 0x%x cl [%u,%u)\r\n",
             before, core[CLE_RFC / 4], core[V3D_INT_STS / 4],
             core[CLE_CT0CS / 4], core[CLE_CT1CS / 4], start, end);
        /* CT1CA/CT1RA 是定位关键：CA=消费到 CL 的哪个字节、
         * RA=子列表返回地址。CT1CA==start 表示一个命令都没消费；
         * ==start+1 表示第一条 opcode 之后停住。 */
        slog("v3d_3d: RENDER ct1ca 0x%x ct1ra 0x%x ct1lc 0x%x err 0x%x\r\n",
             core[CLE_CT1CA / 4], core[CLE_CT1RA / 4],
             core[CLE_CT1LC / 4], core[V3D_ERR_STAT / 4]);
        stalled("render timeout");
    }
    return rc;
}

static uint32_t build_bcl_empty(uint32_t w, uint32_t h)
{
    uint32_t off = 0;

    off = cl_u8(off, 0x77);                     /* NUMBER_OF_LAYERS */
    off = cl_u8(off, 0x00);                     /* layers - 1 = 0 */
    off = cl_u8(off, 0x78);                     /* TILE_BINNING_MODE_CFG */
    off = cl_u8(off, (1u << 4) | (0u << 2));    /* block 128b, initial 64b (matches RCL 0x7E) */
    /* 64px tiles (log2t=3), matching build_draw_bcl/build_draw_rcl.  The clear
     * bin previously used 16px tiles (log2t=1): at 1280x800 that is 80x50=4000
     * tiles, and the binner's per-tile allocation overran the 256 KiB pool
     * (bpca pinned at base+0x40000, bpcs 0) so the bin flush never landed and
     * v3d_3d_clear timed out -> pgl_g3d probe-clear failed -> GPU latched off
     * for the whole session.  64px tiles cut it to 20x13=260 tiles, the same
     * geometry the draw path already completes with. */
    off = cl_u8(off, (3u << 3) | (3u << 0));    /* log2 tile h/w = 3 (64px) */
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x00);
    off = cl_u16le(off, w - 1u);
    off = cl_u16le(off, h - 1u);
    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */
    off = cl_u8(off, 0x06);                     /* START_TILE_BINNING */
    off = cl_u8(off, 0x04);                     /* FLUSH */
    return off;
}

static uint32_t build_clear_rcl(uint32_t fb_iova, uint32_t stride_bytes,
                                uint32_t w, uint32_t h, uint32_t tile,
                                uint32_t fmt, uint32_t rgba8, int rb_swap)
{
    uint32_t off = 0;
    uint32_t tx = (w + tile - 1) / tile;
    uint32_t ty = (h + tile - 1) / tile;
    uint32_t log2t = 3;             /* 64 px tiles (tile arg must be 8<<log2t) */
    uint32_t x, y, i;
    /* clear color 按 RT 的实际格式（RGB565）打包；rgba8 约定 0xAARRGGBB */
    uint32_t r8 = (rgba8 >> 16) & 0xFFu;
    uint32_t g8 = (rgba8 >> 8) & 0xFFu;
    uint32_t b8 = rgba8 & 0xFFu;
    uint32_t c565 = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3);

    /* TILE_RENDERING_MODE_CFG_COMMON (0x79, sub_id 0) */
    off = cl_u8(off, 0x79);
    off = cl_u8(off, 0x00);                     /* nrt-1 = 0, sub_id = 0 */
    off = cl_u16le(off, w);
    off = cl_u16le(off, h);
    off = cl_u8(off, (1u << 6) | (1u << 4));    /* early_z_disable, depth_buf_disable */
    /* v71: Log2 Tile Width 在参数 bit52-54（byte7 bit4-6），Log2 Tile Height
     * 在 bit55-57 —— 跨 byte7 bit7 + byte8 bit0-1。EwokOS 把 byte8 写成 0，
     * height 的高 2 位丢失，硬件按 16px 高的 tile 解析：640x480 要 300 个
     * tile 而 CL 只有 80 个 tile 的命令 → CT1 找不到后续 tile 的命令
     * 挂死（实测 ct1cs 卡 0x18、rfc 0->0）。 */
    off = cl_u8(off, (uint8_t)((1u << 7) | (log2t << 4)));
    off = cl_u8(off, (uint8_t)((log2t >> 1) & 0x3u));

    /* TILE_RENDERING_MODE_CFG_RENDER_TARGET_PART1 (sub_id 2)。
     * v71 字段：sub_id(3) rt(3) base(11,单位512B,start7) stride(7,单位64B,
     * minus_one,start18) bpp(2,start25) type(5,start27) clear_low(32,start32)。
     * base 11 位 @512B 只能覆盖 1MB —— 它只喂 LOAD；clear-only 不 load，
     * 填 0（写屏幕由 STORE 的 32 位地址负责）。
     * stride 按实际行距算；type=16ui(5)：RGB565 是 16 位无符号。
     * EwokOS 原来stride用 tile/2-1=31（=2048B，行距 1280B 对不上）、
     * type=8（8 位 unorm），clear 色也是 RGBA8 直接塞给 565 的 RT。 */
    {
        /* 权威参数（mesa v3d_util.c / v3dx_format_table.c）：
         *   stride = tile_width_px * internal_bpp_words / 2
         *            （单位 128-bit，覆盖 2 行，内部 tile 布局的行距，
         *             与图像行距无关——写屏幕靠 STORE 的 32 位地址）
         *   BGR565: internal_type = V3D_INTERNAL_TYPE_8（枚举 8），
         *           internal_bpp = 32（bpp_words = 1）
         *   base_address 是内部 tile 缓冲的相对布局，不是 FB 地址 */
        uint32_t rt_stride = (64u * 1u) / 2u;   /* = 32 */
        off = cl_u8(off, 0x79);
        off = cl_u8(off, 0x02);                 /* sub_id = 2, rt = 0, base[0:2]=0 */
        off = cl_u8(off, 0x00);                 /* base bits1-8 = 0 */
        off = cl_u8(off, (uint8_t)((rt_stride & 0x3Fu) << 2));
        off = cl_u8(off, (uint8_t)(((rt_stride >> 6) & 0x01u) | (8u << 3)));
        off = cl_u32le(off, c565);              /* clear color low 32 */
    }

    /* TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES (sub_id 1): z=1.0, stencil=0 */
    off = cl_u8(off, 0x79);
    off = cl_u8(off, 0x01);
    off = cl_u8(off, 0x00);
    off = cl_u32le(off, 0x3F800000u);           /* z = 1.0f */
    off = cl_u16le(off, 0x0000);

    /* initial TLB clear: global, 2 iterations (GFXH-1742 workaround) */
    for (i = 0; i < 2; i++) {
        off = cl_u8(off, 0x7C);                 /* TILE_COORDINATES (0,0) */
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x1A);                 /* END_OF_LOADS */
        off = cl_u8(off, 0x1D);                 /* STORE dummy (NONE) */
        off = cl_u8(off, 0x08);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u16le(off, 0x00);
        off = cl_u32le(off, 0x00);
        off = cl_u8(off, 0x19);                 /* CLEAR_RENDER_TARGETS */
        off = cl_u8(off, 0x1B);                 /* END_OF_TILE_MARKER */
    }

    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */

    /* per-tile store of the (cleared) tile buffer to the framebuffer */
    for (y = 0; y < ty; y++) {
        for (x = 0; x < tx; x++) {
            off = cl_u8(off, 0x7C);             /* TILE_COORDINATES */
            off = cl_u8(off, (uint8_t)(x & 0xFF));
            off = cl_u8(off, (uint8_t)(((y & 0x0F) << 4) | ((x >> 8) & 0x0F)));
            off = cl_u8(off, (uint8_t)((y >> 4) & 0xFF));
            off = cl_u8(off, 0x1A);             /* END_OF_LOADS */
            off = cl_u8(off, 0x1D);             /* STORE_TILE_BUFFER_GENERAL */
            off = cl_u8(off, 0x00);             /* RASTER, RT0 */
            off = cl_u8(off, (uint8_t)((fmt & 0x0F) << 4));
            off = cl_u8(off, (uint8_t)((rb_swap ? 0x10u : 0x00u) |
                                       ((fmt >> 4) & 0x03u)));
            off = cl_u8(off, (uint8_t)((stride_bytes & 0x0Fu) << 4));
            off = cl_u8(off, (uint8_t)((stride_bytes >> 4) & 0xFF));
            off = cl_u8(off, (uint8_t)((stride_bytes >> 12) & 0xFF));
            off = cl_u16le(off, 0);             /* height */
            off = cl_u32le(off, fb_iova);       /* absolute address */
            off = cl_u8(off, 0x1B);             /* END_OF_TILE_MARKER */
        }
    }

    off = cl_u8(off, 0x0D);                     /* END_OF_RENDERING */
    /* 同 bin 侧：RFC 只在 FLUSH 命令后递增，没有它 wait_flush 永远超时 */
    off = cl_u8(off, 0x04);                     /* FLUSH: rfc++ */
    return off;
}

static uint32_t depth_func_hw(uint32_t depth_func)
{
    switch (depth_func) {
    case V3D_3D_DEPTH_NEVER:    return 0u;
    case V3D_3D_DEPTH_LESS:     return 1u;
    case V3D_3D_DEPTH_EQUAL:    return 2u;
    case V3D_3D_DEPTH_LEQUAL:   return 3u;
    case V3D_3D_DEPTH_GREATER:  return 4u;
    case V3D_3D_DEPTH_NOTEQUAL: return 5u;
    case V3D_3D_DEPTH_GEQUAL:   return 6u;
    case V3D_3D_DEPTH_ALWAYS:   return 7u;
    default:                    return 0u;   /* NONE (test disabled) */
    }
}

static uint8_t prim_list_type(int mode)
{
    switch (mode) {
    case V3D_3D_MODE_POINTS: return 0u;
    case V3D_3D_MODE_LINES:  return 1u;
    default:                 return 2u;   /* triangles */
    }
}

static uint32_t build_draw_bcl(int mode, uint32_t nverts, uint32_t sr_iova,
                               uint32_t nattrs, uint32_t w, uint32_t h,
                               uint32_t depth_func, uint32_t depth_mask,
                               uint32_t cull_en, const v3d_3d_ext_t *ext)
{
    uint32_t off = 0;
    uint32_t log2t = 3;
    uint32_t half_x = w / 2u, half_y = h / 2u;

    off = cl_u8(off, 0x77);                     /* NUMBER_OF_LAYERS */
    off = cl_u8(off, 0x00);

    off = cl_u8(off, 0x78);                     /* TILE_BINNING_MODE_CFG */
    /* cl[1]: bits4-5 = tile_allocation_block_size, bits2-3 =
     * tile_allocation_initial_block_size (mesa v3d_packet.xml v71).  The
     * draw bin previously emitted (0<<4)|(1<<2) = block 64b / initial 128b,
     * copied from the bare-metal reference's UNFINISHED draw path, while the
     * reference's clear bin -- the only binning config ever proven to
     * complete on this silicon -- uses (1<<4)|(0<<2) = block 128b / initial
     * 64b.  With the swapped pairing the tile allocator ran away (bpca
     * advanced ~20 KB for 80 tiles / 2 triangles) and the bin flush never
     * landed (bfc unmoved, no fresh ERR_STAT, no MMU fault).  Use the
     * proven-completing pairing; cl[2] keeps the 64px tile size to match
     * build_draw_rcl. */
    off = cl_u8(off, (1u << 4) | (0u << 2));    /* block 128b, initial 64b */
    off = cl_u8(off, (uint8_t)((log2t << 3) | log2t));
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x00);
    off = cl_u16le(off, w - 1);
    off = cl_u16le(off, h - 1);
    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */
    off = cl_u8(off, 0x5C);                     /* OCCLUSION_QUERY_COUNTER(0) */
    off = cl_u32le(off, 0);
    off = cl_u8(off, 0x06);                     /* START_TILE_BINNING */
    if (ext && ext->scissor_w > 0 && ext->scissor_h > 0) {
        off = cl_u8(off, 0x6B);                 /* CLIP_WINDOW = scissor rect.
                                                 * Left/Bottom/Width/Height, the
                                                 * bottom already y-down from the
                                                 * top edge (client flipped it). */
        off = cl_u16le(off, (uint32_t)ext->scissor_x);
        off = cl_u16le(off, (uint32_t)ext->scissor_y);
        off = cl_u16le(off, (uint32_t)ext->scissor_w);
        off = cl_u16le(off, (uint32_t)ext->scissor_h);
    } else {
        off = cl_u8(off, 0x6B);                 /* CLIP_WINDOW 0,0,w,h */
        off = cl_u16le(off, 0);
        off = cl_u16le(off, 0);
        off = cl_u16le(off, w);
        off = cl_u16le(off, h);
    }
    {   /* CFG_BITS (0x60), V3D 7.1 field placement (mesa v3d_packet.xml):
         * byte0 bit0 = enable forward facing, bit1 = enable reverse facing,
         * bit2 = clockwise primitives; byte1 bits 4-6 (= packet bits 12-14) =
         * Depth-Test Function (Compare Function enum), bit7 (= packet bit 15)
         * = Z updates enable.  V3D 7.1 has NO separate depth-test-enable bit:
         * the test runs whenever a depth buffer is allocated (TRM_CFG_COMMON
         * depth_buf_disable cleared) and the function is not NEVER.  cull_en
         * carries a V3D_3D_CULL_* config code selecting which facing(s) survive
         * plus the front-face winding; CULL_NONE keeps both facings (draw
         * everything).  With depth_func == NONE and cull_en == CULL_NONE the
         * bytes equal the historical depth/cull-off base. */
        uint8_t b0;
        uint8_t b1 = 0x00;
        /* NOTE: the render target is y-down (the vtx shader scales y by -h/2),
         * so if the Pi5 shows the winding inverted, flip the clockwise bit. */
        switch (cull_en) {
        case V3D_3D_CULL_BACK_CCW:  b0 = 0x01; break;  /* forward only, ccw */
        case V3D_3D_CULL_BACK_CW:   b0 = 0x05; break;  /* forward only, cw  */
        case V3D_3D_CULL_FRONT_CCW: b0 = 0x02; break;  /* reverse only, ccw */
        case V3D_3D_CULL_FRONT_CW:  b0 = 0x06; break;  /* reverse only, cw  */
        case V3D_3D_CULL_BOTH:      b0 = 0x00; break;  /* cull everything   */
        default:                    b0 = 0x03; break;  /* no cull: both     */
        }
        if (ext && ext->poly_enable)
            b0 |= 0x08;                 /* enable depth offset, CFG_BITS bit3 */
        if (depth_func != V3D_3D_DEPTH_NONE) {
            b1 |= (uint8_t)(depth_func_hw(depth_func) << 4); /* func bits 12-14 */
            if (depth_mask)
                b1 |= 0x80;                 /* Z updates enable, bit 15 */
        }
        off = cl_u8(off, 0x60);
        off = cl_u8(off, b0);
        off = cl_u8(off, b1);
        off = cl_u8(off, 0x00);
    }
    if (ext && ext->poly_enable) {          /* DEPTH_OFFSET (0x6A): Factor (f187,
                                             * bytes0-1), Units (f187, bytes2-3),
                                             * Limit (float32, bytes4-7, unused). */
        off = cl_u8(off, 0x6A);
        off = cl_u16le(off, cl_f187(ext->poly_factor));
        off = cl_u16le(off, cl_f187(ext->poly_units));
        off = cl_f32le(off, 0.0f);
    }
    off = cl_u8(off, 0x68);                     /* POINT_SIZE */
    off = cl_f32le(off, 1.0f);
    off = cl_u8(off, 0x69);                     /* LINE_WIDTH */
    off = cl_f32le(off, 1.0f);
    off = cl_u8(off, 0x6E);                     /* CLIPPER_XY_SCALING (1/64 px) */
    off = cl_f32le(off, (float)half_x * 64.0f);
    off = cl_f32le(off, -(float)half_y * 64.0f);
    off = cl_u8(off, 0x6F);                     /* CLIPPER_Z_SCALE_AND_OFFSET */
    off = cl_f32le(off, 0.5f);
    off = cl_f32le(off, 0.5f);
    off = cl_u8(off, 0x6D);                     /* CLIPPER_Z_MIN_MAX 0..1 */
    off = cl_f32le(off, 0.0f);
    off = cl_f32le(off, 1.0f);
    {                                           /* VIEWPORT_OFFSET (14.8 fixed) */
        uint32_t fine_x = (uint32_t)((uint64_t)half_x * 256u);
        uint32_t fine_y = (uint32_t)((uint64_t)half_y * 256u);
        off = cl_u8(off, 0x6C);
        off = cl_u8(off, (uint8_t)(fine_x & 0xFF));
        off = cl_u8(off, (uint8_t)((fine_x >> 8) & 0xFF));
        off = cl_u8(off, (uint8_t)((fine_x >> 16) & 0x3F));
        off = cl_u8(off, 0x00);                 /* coarse x = 0 */
        off = cl_u8(off, (uint8_t)(fine_y & 0xFF));
        off = cl_u8(off, (uint8_t)((fine_y >> 8) & 0xFF));
        off = cl_u8(off, (uint8_t)((fine_y >> 16) & 0x3F));
        off = cl_u8(off, 0x00);                 /* coarse y = 0 */
    }
    off = cl_u8(off, 0x53);                     /* BLEND_ENABLES: none */
    off = cl_u8(off, 0x00);
    {   /* COLOR_WRITE_MASKS (0x57).  The reference Pi5 path emits 0 and still
         * renders full colour, so a CLEARED mask = write-all: a SET bit
         * DISABLES a channel.  ext->color_mask is the GL write-enable bitmask
         * (bit0=R..bit3=A); invert its low nibble so write-all (0xF) and an
         * absent/old ext (0) both yield the proven 0.  Channel order is the
         * natural GL R/G/B/A mapping, confirmed on the Pi5. */
        uint32_t wm = 0;
        if (ext && ext->color_mask != 0 && (ext->color_mask & 0xFu) != 0xFu)
            wm = (~ext->color_mask) & 0xFu;
        off = cl_u8(off, 0x57);
        off = cl_u32le(off, wm);
    }
    off = cl_u8(off, 0x56);                     /* BLEND_CONSTANT_COLOR f16 x4 */
    off = cl_u16le(off, 0);
    off = cl_u16le(off, 0);
    off = cl_u16le(off, 0);
    off = cl_u16le(off, 0);
    off = cl_u8(off, 0x61);                     /* ZERO_ALL_FLAT_SHADE */
    off = cl_u8(off, 0x63);                     /* ZERO_ALL_NON_PERSPECTIVE */
    off = cl_u8(off, 0x58);                     /* ZERO_ALL_CENTROID */
    off = cl_u8(off, 0x4A);                     /* TRANSFORM_FEEDBACK_SPECS */
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x5C);                     /* OCCLUSION_QUERY_COUNTER(0) */
    off = cl_u32le(off, 0);
    off = cl_u8(off, 0x5B);                     /* SAMPLE_STATE mask 0xF, cov 1.0 */
    off = cl_u8(off, 0x0F);
    off = cl_u8(off, 0x80);
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x47);                     /* VCM_CACHE_SIZE 4,4 */
    off = cl_u8(off, 0x44);
    off = cl_u8(off, 0x40);                     /* GL_SHADER_STATE */
    off = cl_u8(off, (uint8_t)(sr_iova & 0xFF) | (uint8_t)(nattrs & 0x1F));
    off = cl_u8(off, (uint8_t)((sr_iova >> 8) & 0xFF));
    off = cl_u8(off, (uint8_t)((sr_iova >> 16) & 0xFF));
    off = cl_u8(off, (uint8_t)((sr_iova >> 24) & 0xFF));
    off = cl_u8(off, 0x24);                     /* VERTEX_ARRAY_PRIMS */
    off = cl_u8(off, (uint8_t)mode);
    off = cl_u32le(off, nverts);
    off = cl_u32le(off, 0);                     /* first = 0 */
    off = cl_u8(off, 0x04);                     /* FLUSH */
    return off;
}

static uint32_t build_draw_rcl(uint32_t fb_iova, uint32_t stride_bytes,
                               uint32_t fmt, uint32_t rgba8, int rb_swap,
                               uint32_t w, uint32_t h, int depth_en,
                               uint32_t z_iova, uint32_t z_stride,
                               int first_frame, float clear_z, int mode)
{
    uint32_t off = 0;
    uint32_t log2t = 3;               /* 64 px tiles */
    uint32_t tx = (w + 63) / 64;
    uint32_t ty = (h + 63) / 64;
    uint32_t i;

    /* TRM_CFG_COMMON (0x79, sub 0): nrt=1, w, h, 64px tiles.  Bit 4 is
     * depth_buf_disable: it stays SET when the caller wants no depth (the
     * historical colour-only path) and is CLEARED when depth is enabled so
     * the tile-local Z buffer is allocated and used.  Internal Depth Type
     * (bits 47-50) stays 0 = depth_32f, matching the persistent Z32F surface
     * the STORE/LOAD packets below move in and out of DRAM.  early_z_disable
     * (bit 6) stays set: the fixed-function frag does not write depth, and
     * resolving Z after the fragment is the conservative choice. */
    off = cl_u8(off, 0x79);
    off = cl_u8(off, 0x00);
    off = cl_u16le(off, w);
    off = cl_u16le(off, h);
    off = cl_u8(off, depth_en ? (uint8_t)(1u << 6)
                              : (uint8_t)((1u << 6) | (1u << 4)));
    off = cl_u8(off, (uint8_t)((log2t << 7) | (log2t << 4)));
    off = cl_u8(off, (uint8_t)((log2t >> 1) & 0x03));
    /* RT_PART1 (0x79, sub 2): clear color, clamp-8, bpp-32, rt stride */
    {
        uint32_t rt_stride = (64u / 2u) - 1u;
        off = cl_u8(off, 0x79);
        off = cl_u8(off, 0x02);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, (uint8_t)((rt_stride & 0x7F) << 2));
        off = cl_u8(off, (uint8_t)((rt_stride >> 6) | (8u << 3) | (0u << 1)));
        off = cl_u32le(off, rgba8);
    }
    /* ZS_CLEAR_VALUES (0x79, sub 1): z = clear_z (float bits), stencil = 0 */
    {
        uint32_t zbits;
        __builtin_memcpy(&zbits, &clear_z, 4);
        off = cl_u8(off, 0x79);
        off = cl_u8(off, 0x01);
        off = cl_u8(off, 0x00);
        off = cl_u32le(off, zbits);
        off = cl_u16le(off, 0x0000);
    }
    /* TILE_LIST_INITIAL_BLOCK_SIZE (0x7E): auto-chain, first block 64b.
     * MUST match the binner's tile_allocation_initial_block_size in
     * TILE_BINNING_MODE_CFG (64b, bits 2-3 = 0): the render CLE walks each
     * tile's first block with this size, so 128b here made it run past the
     * 64b block the binner wrote into the neighbouring tile's list and fault
     * with CTERR (ct1cs 0x218) as soon as real geometry landed in a tile. */
    off = cl_u8(off, 0x7E);
    off = cl_u8(off, (1u << 2) | (0u << 0));
    /* MULTICORE_RENDERING_SUPERTILE_CFG (0x7A): 1 supertile = whole frame */
    off = cl_u8(off, 0x7A);
    off = cl_u8(off, (uint8_t)(tx - 1));        /* supertile_w - 1 (tiles) */
    off = cl_u8(off, (uint8_t)(ty - 1));
    off = cl_u8(off, 1);                        /* frame w in supertiles */
    off = cl_u8(off, 1);
    off = cl_u8(off, (uint8_t)(tx & 0xFF));
    off = cl_u8(off, (uint8_t)(((ty & 0x0F) << 4) | ((tx >> 8) & 0x0F)));
    off = cl_u8(off, (uint8_t)((ty >> 4) & 0xFF));
    off = cl_u8(off, 0x00);                     /* nblists-1 = 0 */
    /* MULTICORE_RENDERING_TILE_LIST_SET_BASE (0x7B) -> binning pool */
    off = cl_u8(off, 0x7B);
    off = cl_u32le(off, _pool_p);
    /* initial TLB clear (2x, global, GFXH-1742 workaround): only on the first
     * dispatch of a frame, which initialises colour (RT_PART1) + Z (ZS_CLEAR)
     * for the whole frame.  Later dispatches LOAD the persisted buffers, so
     * clearing here would erase the frame accumulated so far. */
    if (first_frame) {
        for (i = 0; i < 2; i++) {
            off = cl_u8(off, 0x7C);             /* TILE_COORDINATES (0,0) */
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x1A);             /* END_OF_LOADS */
            off = cl_u8(off, 0x1D);             /* STORE dummy (NONE) */
            off = cl_u8(off, 0x08);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u8(off, 0x00);
            off = cl_u16le(off, 0x00);
            off = cl_u32le(off, 0x00);
            off = cl_u8(off, 0x19);             /* CLEAR_RENDER_TARGETS */
            off = cl_u8(off, 0x1B);             /* END_OF_TILE_MARKER */
        }
    }
    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */
    /* generic tile list built directly into _gen (the cl_* helpers write
     * into _cl and would clobber the RCL being assembled there). */
    {
        uint8_t *gp = _gen_va;
        uint32_t g = 0;
        int zpersist = (depth_en && z_iova != 0);
        gp[g++] = 0x7D;                         /* TILE_COORDINATES_IMPLICIT */
        if (!first_frame) {
            /* later dispatch of the frame: reload the persisted colour (+Z)
             * from DRAM so this dispatch accumulates onto the previous ones
             * instead of erasing them. */
            gen_emit_store_load(gp, &g, 0x1E, 0, fmt, rb_swap,
                                stride_bytes, fb_iova);          /* LOAD RT0 */
            if (zpersist)
                gen_emit_store_load(gp, &g, 0x1E, 9, V3D_3D_FMT_D32F, 0,
                                    z_stride, z_iova);           /* LOAD Z */
        }
        gp[g++] = 0x1A;                         /* END_OF_LOADS */
        gp[g++] = 0x38;                         /* PRIM_LIST_FORMAT */
        gp[g++] = prim_list_type(mode);         /* points/lines/triangles */
        /* SET_INSTANCEID (V3D71 length 5: opcode + u32 instance id; PTB
         * expects 0).  Emitting only one id byte made the CLE swallow the
         * following 0x15 0x00 0x1D bytes as the id and then fetch the STORE
         * packet's buffer byte (0x00) as the next opcode = HALT: render thread
         * stopped inside the generic list (ct1cs 0x110 = CTSUBS, RTSD 1,
         * CTRUN 0, no CTERR) and RFC never advanced. */
        gp[g++] = 0x36;
        gp[g++] = 0x00;
        gp[g++] = 0x00;
        gp[g++] = 0x00;
        gp[g++] = 0x00;
        gp[g++] = 0x15;                         /* BRANCH_TO_IMPLICIT_TILE_LIST */
        gp[g++] = 0x00;                         /* set 0 */
        gen_emit_store_load(gp, &g, 0x1D, 0, fmt, rb_swap,
                            stride_bytes, fb_iova);              /* STORE RT0 */
        if (zpersist)
            gen_emit_store_load(gp, &g, 0x1D, 9, V3D_3D_FMT_D32F, 0,
                                z_stride, z_iova);               /* STORE Z */
        gp[g++] = 0x1B;                         /* END_OF_TILE_MARKER */
        gp[g++] = 0x12;                         /* RETURN_FROM_SUB_LIST */
        off = cl_u8(off, 0x14);                 /* START_ADDRESS_OF_GENERIC_TILE_LIST */
        off = cl_u32le(off, _gen_p);
        off = cl_u32le(off, _gen_p + g);
    }
    off = cl_u8(off, 0x17);                     /* SUPERTILE_COORDINATES (0,0) */
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x0D);                     /* END_OF_RENDERING */
    return off;
}


/* ============ 正式 CL 提交 API（Mesa 桥接用） ============
 * CL 由调用者（Mesa/应用）生成，[start,end) 是 GPU VA。
 * 这里只做「提交」：boot halt 清除、PTB/QMA/QMS/QTS、L2 维护、
 * 写 QBA/QEA 启动、等 BFC/RFC 递增。CL 内容的正确性由调用者保证。 */
uint32_t v3d_3d_cl_base(void) { return _cl_p; }

int v3d_3d_submit_cl(uint32_t bcl_start, uint32_t bcl_end,
                     uint32_t rcl_start, uint32_t rcl_end, uint32_t w,
                     uint32_t h) {
  volatile uint32_t* core;
  uint32_t bfc0, rfc0;
  int rcb, rcr;
  (void)w;
  (void)h;

  if (!_ready) {
    return V3D_3D_UNSUPPORTED;
  }
  core = v3d_g2d_core_regs();
  if (core == NULL) {
    return V3D_3D_FAILED;
  }

  v3d_g2d_op_begin();

  /* ---- bin ---- */
  bfc0 = core[CLE_BFC / 4];
  core[CLE_CT0CS / 4] = 0;
  core[CLE_CT0QMA / 4] = _pool_p;
  core[CLE_CT0QMS / 4] = V3D_3D_POOL_SIZE;
  core[CLE_CT0QTS / 4] = (1u << 1) | _tsda_p;
  core[PTB_BPOS / 4] = 0;
  __asm__ __volatile__("dsb sy" ::: "memory");
  v3d_g2d_l2_invalidate_all();
  core[CLE_CT0QBA / 4] = bcl_start;
  core[CLE_CT0QEA / 4] = bcl_end;
  rcb = wait_flush(core, CLE_BFC, bfc0);
  if (rcb != 0) {
    slog("v3d_3d: submit_cl BIN timeout bfc %u->%u ct0ca 0x%x ct0cs 0x%x\r\n",
         bfc0, core[CLE_BFC / 4], core[CLE_CT0CA / 4],
         core[CLE_CT0CS / 4]);
  }

  /* ---- render ---- */
  rfc0 = core[CLE_RFC / 4];
  core[CLE_CT1CS / 4] = 0;
  __asm__ __volatile__("dsb sy" ::: "memory");
  v3d_g2d_l2_invalidate_all();
  core[CLE_CT1QBA / 4] = rcl_start;
  core[CLE_CT1QEA / 4] = rcl_end;
  rcr = wait_flush(core, CLE_RFC, rfc0);
  if (rcr != 0) {
    slog("v3d_3d: submit_cl RENDER timeout rfc %u->%u ct1ca 0x%x ct1cs 0x%x\r\n",
         rfc0, core[CLE_RFC / 4], core[CLE_CT1CA / 4],
         core[CLE_CT1CS / 4]);
  }

  v3d_g2d_op_end();
  return (rcb == 0 && rcr == 0) ? V3D_3D_DONE : V3D_3D_FAILED;
}

static void stalled(const char *why)
{
    slog("v3d_3d: %s on job: %s\r\n", why, _job_desc);
    v3d_g2d_recover(why);
}

static void gen_emit_store_load(uint8_t *gp, uint32_t *g, uint8_t opcode,
                                uint32_t buffer, uint32_t imgfmt, int rb_swap,
                                uint32_t stride, uint32_t addr)
{
    gp[(*g)++] = opcode;
    gp[(*g)++] = (uint8_t)(buffer & 0x0F);              /* memfmt raster = 0 */
    gp[(*g)++] = (uint8_t)((imgfmt & 0x0F) << 4);       /* img fmt bits 12-15 */
    gp[(*g)++] = (uint8_t)((rb_swap ? 0x10u : 0x00u) | ((imgfmt >> 4) & 0x03u));
    gp[(*g)++] = (uint8_t)((stride & 0x0Fu) << 4);      /* stride bits 28-31 */
    gp[(*g)++] = (uint8_t)((stride >> 4) & 0xFF);       /* stride bits 32-39 */
    gp[(*g)++] = (uint8_t)((stride >> 12) & 0xFF);      /* stride bits 40-47 */
    gp[(*g)++] = 0x00;                                  /* height bits 48-55 */
    gp[(*g)++] = 0x00;                                  /* height bits 56-63 */
    gp[(*g)++] = (uint8_t)(addr & 0xFF);                /* address bits 64-95 */
    gp[(*g)++] = (uint8_t)((addr >> 8) & 0xFF);
    gp[(*g)++] = (uint8_t)((addr >> 16) & 0xFF);
    gp[(*g)++] = (uint8_t)((addr >> 24) & 0xFF);
}

/* ============================== 初始化 ============================== */
int v3d_3d_init(void) {
  void* va;
  uint32_t iova;

  if (_ready) {
    return 0;
  }
  if (!v3d_g2d_ready()) {
    return -1;
  }

  if (alloc_stage(V3D_3D_CL_MAX, &va, &iova)) return -1;
  _cl_va = (uint8_t*)va; _cl_p = iova;

  if (alloc_stage(V3D_3D_POOL_SIZE, &va, &iova)) return -1;
  _pool_va = (uint8_t*)va; _pool_p = iova;

  if (alloc_stage(V3D_3D_TSDA_SIZE, &va, &iova)) return -1;
  _tsda_va = (uint8_t*)va; _tsda_p = iova;

  if (alloc_stage(V3D_3D_SSR_SIZE, &va, &iova)) return -1;
  _ssr_va = (uint8_t*)va; _ssr_p = iova;

  if (alloc_stage(V3D_3D_SHADER_WORDS * 8u, &va, &iova)) return -1;
  _shaders_va = (uint64_t*)va; _shaders_p = iova;

  if (alloc_stage(V3D_3D_UNIF_WORDS * 4u, &va, &iova)) return -1;
  _unif_va = (uint32_t*)va; _unif_p = iova;

  if (alloc_stage(V3D_3D_UNIF_WORDS * 4u, &va, &iova)) return -1;
  _cs_unif_va = (uint32_t*)va; _cs_unif_p = iova;

  if (alloc_stage(V3D_3D_GEN_SIZE, &va, &iova)) return -1;
  _gen_va = (uint8_t*)va; _gen_p = iova;

  /* 装载 QPU 代码：frag @ [0]，vtx @ [nf]，coord @ [nf+nv] */
  {
    uint32_t nf = sizeof(_frag_shader) / 8u;
    uint32_t nv = sizeof(_vtx_shader) / 8u;
    uint32_t nc = sizeof(_coord_shader) / 8u;
    uint32_t i;

    if (nf + nv + nc > V3D_3D_SHADER_WORDS) {
      return -1;
    }
    for (i = 0; i < nf; i++) _shaders_va[i] = _frag_shader[i];
    for (i = 0; i < nv; i++) _shaders_va[nf + i] = _vtx_shader[i];
    for (i = 0; i < nc; i++) _shaders_va[nf + nv + i] = _coord_shader[i];

    _frag_iova = _shaders_p;
    _vtx_iova = _shaders_p + nf * 8u;
    _coord_iova = _shaders_p + (nf + nv) * 8u;
    /* QPU 取指走 DRAM：刚写的 shader 机器码必须落盘 */
    dma_clean(_shaders_p, (nf + nv + nc) * 8u);
  }
  __asm__ __volatile__("dsb sy" ::: "memory");

  _ready = 1;
  printf("v3d_3d: ready (cl=%u pool=%u tsda=%u shader=%u)\n",
         _cl_p, _pool_p, _tsda_p, _shaders_p);
  return 0;
}

int v3d_3d_ready(void) { return _ready; }

/* ============================== 绘制 ==============================
 * 只支持 fixed-function 路径（内置 shader），参数精简到茶壶够用的程度。
 *
 *   pos_iova : nverts × float3（stride 12）
 *   col_iova : nverts × RGBA byte4（stride 4）
 *   fb_iova  : 渲染目标
 *
 * 深度用 tile-local Z（不申请持久 Z 面），比较函数 LEQUAL，双面绘制。
 * first_frame != 0 时先清颜色和 Z；否则 LOAD 上一帧的结果累积。
 */
/* 【递进 render CL 探针】三个实验一轮跑完，读 CT1CA 判断消费量：
 *   A: 单字节 [END_OF_RENDERING]     —— CT 能否执行最简单的命令
 *   B: [0x79 Common 9B]              —— 0x79 的参数能否被读取
 *   C: B + [END]                     —— 完整收尾
 * consumed 读数：
 *   A=1  → 单字节命令 OK
 *   B=9  → 0x79 带参数 OK；B=1 → opcode 后停（参数读取失败）
 *   C=10 → 全通 */
/* 空的 bin CL（只配置、无图元），为 render 准备 tile 列表。
 * 【bin 必须先行】render CT 按 tile 执行，tile 列表由 bin 阶段写入
 * tile allocation pool；没有 bin，render CT 无 tile 可执行，
 * 一个命令都不消费（probe consumed=0 的根因）。 */
static int run_empty_bin(volatile uint32_t* core, uint32_t w, uint32_t h) {
  uint32_t blen = build_bcl_empty(w, h);
  dma_clean(_cl_p, blen);
  int rc = submit_bin(core, _cl_p, _cl_p + blen);
  printf("v3d_3d: empty bin rc=%d bfc=%u ct0ca=0x%x consumed=%u\r\n", rc,
         core[CLE_BFC / 4], core[CLE_CT0CA / 4],
         core[CLE_CT0CA / 4] - _cl_p);
  return rc;
}

int v3d_3d_probe_rcl(void) {
  volatile uint32_t* core;
  uint32_t off;
  uint32_t log2t = 3;
  int rc;

  if (!_ready) {
    return V3D_3D_UNSUPPORTED;
  }
  core = v3d_g2d_core_regs();
  if (core == NULL) {
    return V3D_3D_FAILED;
  }

  v3d_g2d_op_begin();

  if (run_empty_bin(core, 640, 480) != 0) {
    v3d_g2d_op_end();
    return V3D_3D_FAILED;
  }

  /* ---- A: 单字节 END_OF_RENDERING ---- */
  off = 0;
  off = cl_u8(off, 0x0D);
  dma_clean(_cl_p, off);
  rc = submit_render(core, _cl_p, _cl_p + off);
  printf("v3d_3d: probeA [END] rc=%d consumed=%u ct1cs=0x%x\r\n", rc,
         core[CLE_CT1CA / 4] - _cl_p, core[CLE_CT1CS / 4]);

  /* ---- B: 0x79 Common，9 字节 ---- */
  off = 0;
  off = cl_u8(off, 0x79);
  off = cl_u8(off, 0x00);
  off = cl_u16le(off, 640);
  off = cl_u16le(off, 480);
  off = cl_u8(off, (1u << 6) | (1u << 4));
  off = cl_u8(off, (uint8_t)((1u << 7) | (log2t << 4)));
  off = cl_u8(off, (uint8_t)((log2t >> 1) & 0x3u));
  dma_clean(_cl_p, off);
  rc = submit_render(core, _cl_p, _cl_p + off);
  printf("v3d_3d: probeB [Common] rc=%d consumed=%u ct1cs=0x%x\r\n", rc,
         core[CLE_CT1CA / 4] - _cl_p, core[CLE_CT1CS / 4]);

  /* ---- C: B + END_OF_RENDERING ---- */
  off = cl_u8(off, 0x0D);
  dma_clean(_cl_p, off);
  rc = submit_render(core, _cl_p, _cl_p + off);
  printf("v3d_3d: probeC [Common+END] rc=%d consumed=%u ct1cs=0x%x\r\n", rc,
         core[CLE_CT1CA / 4] - _cl_p, core[CLE_CT1CS / 4]);

  v3d_g2d_op_end();
  return rc;
}

/* 只跑 render 阶段的整屏清色，完全绕开 binner。
 * EwokOS 注明：clear 的 CL 是这块硅片上唯一被证明能完整跑通的路径，
 * 用它先把 render 引擎（CT1）验证通，binner（CT0）的问题再单独攻。 */
int v3d_3d_clear_min(uint32_t fb_iova, uint32_t stride_bytes, uint32_t fmt,
                     uint32_t w, uint32_t h, uint32_t rgba8) {
  volatile uint32_t* core;
  uint32_t len;
  int rc;

  if (!_ready || w == 0 || h == 0 || fb_iova == 0) {
    return V3D_3D_UNSUPPORTED;
  }
  if (!v3d_g2d_phy_valid(fb_iova, (size_t)stride_bytes * h)) {
    return V3D_3D_UNSUPPORTED;
  }
  core = v3d_g2d_core_regs();
  if (core == NULL) {
    return V3D_3D_FAILED;
  }
  v3d_g2d_op_begin();
  /* 【bin 必须先行】空 bin 生成 tile 列表，render 才有 tile 可执行。
   * bin 失败时不拦截——render+FLUSH 的组合还没测过，把两个变量分开。 */
  {
    int brc = run_empty_bin(core, w, h);
    printf("v3d_3d: clear: empty bin rc=%d（失败也继续测 render）\r\n", brc);
  }
  len = build_clear_rcl(fb_iova, stride_bytes, w, h, 64u, fmt, rgba8, 0);
  if (len == 0 || len > V3D_3D_CL_MAX) {
    v3d_g2d_op_end();
    return V3D_3D_FAILED;
  }
  dma_clean(_cl_p, len);
  rc = submit_render(core, _cl_p, _cl_p + len);
  v3d_g2d_invalidate_span(fb_iova, (size_t)stride_bytes * h);
  v3d_g2d_op_end();
  return rc == 0 ? V3D_3D_DONE : V3D_3D_FAILED;
}

int v3d_3d_draw_min(uint32_t fb_iova, uint32_t stride_bytes, uint32_t fmt,
                uint32_t w, uint32_t h, uint32_t mode, uint32_t nverts,
                uint32_t pos_iova, uint32_t col_iova, uint32_t clear_rgba,
                int first_frame) {
  volatile uint32_t* core;
  uint32_t len, ux, uy;
  float fx, fy;
  int rc;

  if (!_ready || w == 0 || h == 0 || fb_iova == 0 || nverts == 0) {
    return V3D_3D_UNSUPPORTED;
  }
  if (nverts > V3D_3D_MAX_VERTS) {
    return V3D_3D_UNSUPPORTED;
  }
  if (mode != V3D_3D_MODE_POINTS && mode != V3D_3D_MODE_LINES &&
      mode != V3D_3D_MODE_TRIANGLES) {
    return V3D_3D_UNSUPPORTED;
  }
  if (!v3d_g2d_phy_valid(fb_iova, (size_t)stride_bytes * h) ||
      !v3d_g2d_phy_valid(pos_iova, (size_t)nverts * 12u) ||
      !v3d_g2d_phy_valid(col_iova, (size_t)nverts * 4u)) {
    return V3D_3D_UNSUPPORTED;
  }

  /* 视口变换：x 缩放 w/2，y 缩放 -h/2（目标是 y 向下） */
  fx = (float)(w / 2u);
  fy = -(float)(h / 2u);
  memcpy(&ux, &fx, 4);
  memcpy(&uy, &fy, 4);

  /* coord（binning）shader 的 uniform：1.0 / xscale / yscale */
  _cs_unif_va[0] = 0x3F800000u;
  _cs_unif_va[1] = ux;
  _cs_unif_va[2] = uy;

  /* vtx/frag uniform：[0]=1.0 w [1]=xscale [2]=yscale [3]=0.5 zscale [4]=0.5 zoff */
  _unif_va[0] = 0x3F800000u;
  _unif_va[1] = ux;
  _unif_va[2] = uy;
  _unif_va[3] = 0x3F000000u;
  _unif_va[4] = 0x3F000000u;
  dma_clean(_cs_unif_p, 32u);
  dma_clean(_unif_p, 32u);

  /* shader record + 紧接着的属性 record（CLE 从 record+32 开始遍历属性，
   * 所以属性必须紧跟其后，不能放别的缓冲） */
  build_shader_record(_frag_iova, _unif_p, sh_flags(1u, 1u), _vtx_iova,
                      _unif_p, sh_flags(1u, 1u), _coord_iova, _cs_unif_p,
                      sh_flags(1u, 1u), 4u);
  build_attr_records(pos_iova, col_iova);
  /* shader record(32B) + 两个属性 record(2x16B) 一起落盘 */
  dma_clean(_ssr_p, V3D_3D_SSR_SIZE);

  core = v3d_g2d_core_regs();
  if (core == NULL) {
    return V3D_3D_FAILED;
  }
  v3d_g2d_op_begin();

  /* bin 阶段：生成图元列表 */
  len = build_draw_bcl((int)mode, nverts, _ssr_p, 2u, w, h,
                       V3D_3D_DEPTH_LEQUAL, 1u, V3D_3D_CULL_NONE, NULL);
  if (len == 0 || len > V3D_3D_CL_MAX) {
    goto out;
  }
  dma_clean(_cl_p, len); /* binner 从 DRAM 取 CL */
  rc = submit_bin(core, _cl_p, _cl_p + len);
  if (rc != 0) {
    goto out;
  }

  /* render 阶段：逐 tile 渲染 */
  len = build_draw_rcl(fb_iova, stride_bytes, fmt, clear_rgba, 0, w, h,
                       1 /* depth test on */, 0 /* 无持久 Z */, 0,
                       first_frame, 1.0f, (int)mode);
  if (len == 0 || len > V3D_3D_CL_MAX) {
    goto out;
  }
  dma_clean(_cl_p, len); /* renderer 从 DRAM 取 CL */
  rc = submit_render(core, _cl_p, _cl_p + len);
  if (rc != 0) {
    goto out;
  }

  v3d_g2d_invalidate_span(fb_iova, (size_t)stride_bytes * h);
  v3d_g2d_op_end();
  return V3D_3D_DONE;

out:
  v3d_g2d_op_end();
  return V3D_3D_FAILED;
}
