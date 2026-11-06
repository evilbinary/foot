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
#define V3D_INT_CLR  0x58u    /* interrupt clear（write-1-to-clear，Linux 中断里写）*/
#define CLE_INT_MSK_STS 0x5cu /* 中断屏蔽状态（读） */
#define CLE_INT_MSK_SET 0x60u /* 中断屏蔽置位（写 1 = 屏蔽） */
#define CLE_INT_MSK_CLR 0x64u /* 中断屏蔽清除（写 1 = 解除屏蔽） */

/* 是否按 Linux v3d_irq_enable() 解屏蔽 CLE 完成中断。
 *
 * 【当前设为 0：对齐参考仓库已跑通的裸机 clear 路径】
 * videocore7-exps 的 v3d_submit_render() 完全不解屏蔽、不碰 INT_STS，
 * 只轮询 RFC 递增；它在真机上连续跑通了多次 clear。Linux 解屏蔽是为了
 * 用 fence 唤醒调度器，硬件执行/退役本身并不依赖 IRQ mask。YiYiYa 没有
 * 注册 V3D 中断处理函数，解屏蔽只会多一个未处理的中断源，先把变量摘掉。 */
#define V3D_3D_ENABLE_CLE_IRQ 0

/* CT1QCFG(0x178)：上电默认就是 ETFILT|ETPROC = 0xC0。
 *
 * 【绝对不要写 0】附录 I.3 #5 / 历史实测：把 0x178 当 CT1CFG 清 0 之后，
 * CT1 一个字节都取不出来（probe consumed=0）。videocore7-exps 写 0 的那条
 * CLE clear 路径本身没在 Pi5 上作为权威配方验证过（能跑通的是 CSD g2d）。
 * Linux 对这个寄存器是 "XXX: Set the QCFG"（保留默认，不写）。
 * 提交时也不要每次强写 0xC0——保持硬件默认即可。 */
#define CLE_QCFG_ETFILT (1u << 7)
#define CLE_QCFG_ETPROC (1u << 6)
#define CLE_L2TCACTL 0x30u    /* L2T cache 控制（bit0 是失效忙位） */

/* 中断位定义照 drivers/gpu/drm/v3d/v3d_regs.h（V3D 7.1）：
 *   V3D_INT_FRDONE  BIT(0)   render 完成
 *   V3D_INT_FLDONE  BIT(1)   bin 完成
 *   V3D_INT_OUTOMEM BIT(2)   binner 池不够
 *   V3D_INT_CSDDONE BIT(6)   CSD 完成（ver >= 71）
 * 日志里长期看到的 int_sts=0x2 就是 FLDONE。 */
#define V3D_INT_FRDONE  (1u << 0)
#define V3D_INT_FLDONE  (1u << 1)
#define V3D_INT_OUTOMEM (1u << 2)
#define V3D_INT_CSDDONE (1u << 6)
#define CLE_IRQS (V3D_INT_FRDONE | V3D_INT_FLDONE | V3D_INT_OUTOMEM | \
                  V3D_INT_CSDDONE)
/* 【必须开启】GFXH-1742 段（mesa v3dx_rcl.c:638-645）不只是 workaround：
 * 它同时提供了从「load 模式」切到「render 模式」的 END_OF_LOADS(0x1A)，
 * 以及首个 tile 的 CLEAR_RENDER_TARGETS(0x19) + END_OF_TILE_MARKER(0x1B)。
 * 之前为二分把它关掉，结果 CT1 在 0x7C 之后一直等「切模式」信号，
 * 卡住不动（ct1ca 恒 +9、CTRUN 不清）。 */
#define V3D_3D_ENABLE_GFXH1742 1

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

/* CT1QCFG(0x178) 的初值。这个寄存器的默认值一旦被清成 0，CT1 就取不出
 * 命令（历史注释记录过：EwokOS 把它当 CT1CFG 写 0 之后彻底不工作）。
 * 我们在超时现场读到的却一直是 0x0 —— 先确认「初始是不是 0」，再决定
 * 要不要每次提交前恢复。 */
static uint32_t _qcfg_init;

static uint32_t _frag_iova;
static uint32_t _vtx_iova;
static uint32_t _coord_iova;

/* generic tile list：render CL 里的 SUB_LIST 指向它 */
static uint8_t* _gen_va;
static uint32_t _gen_p;

/* 当前作业描述（超时日志里打出来，便于定位是哪次 draw） */
static char _job_desc[128];

/* CT1 卡死后不会自己恢复（本模块没有 reset 通路），后续每次 render 提交都
 * 只会白烧 5M 次 yield 并打一份同样的超时日志，而且 CT1CA 会一直停在上一
 * 次的值上——很容易被误读成「这一次也卡在同一个包」。置位后直接跳过并明确
 * 打一行，日志就能区分「第一次卡死」和「后面的无效提交」。 */
static int _ct1_wedged;

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

/* 等 CT 完成。判据有两套，**必须都看**：
 *
 *  1) BFC/RFC（flush 计数）—— 只有 CL 里出现 FLUSH(0x04) 才会递增。
 *     bin 的 CL 结尾有 FLUSH，所以能用；**Mesa 的 render CL 没有 FLUSH**，
 *     RFC 永远不动 —— 只等它必然 100% 超时（实测 clear/triangle 都这样）。
 *
 *  2) CTnCS 的 CTRUN(BIT5) 清零 —— CT 执行完整个 CL 就清。
 *     这是 render 唯一可靠的完成信号（probeC [Common+END] 实测 ct1cs=0x0
 *     而 RFC 纹丝不动）。
 *
 * 为避免「刚写完 QEA、CT 还没置起 CTRUN 就被判完成」的竞态，
 * CTRUN 判据要等轮询超过 STARTUP_SPIN 次之后才生效。 */
#define CT_DONE_STARTUP_SPIN 200000u

/* 上一次 wait_flush 是走中断判据（无最高位）还是走 CTRUN 兜底（最高位置位） */
static uint32_t _last_spins;

static int wait_flush(volatile uint32_t *core, uint32_t irq_bit,
                      uint32_t cnt_reg, uint32_t cnt_before,
                      uint32_t cs_reg, uint32_t ca_reg, uint32_t ea)
{
    /* 三个判据按可靠性排列：
     * 1) 计数递增（BFC/RFC）—— EwokOS 的判据，job 真正退役才会动；
     * 2) 中断位（FLDONE/FRDONE）—— Linux 的判据，但需要先解屏蔽；
     * 3) CTRUN 清零 + CA 到末尾 —— 兜底，注意 CLE 出错时也会停转。
     * 原来只有 3)，所以「job 没真正结束就写下一个 QBA/QEA」把 CLE 弄坏了
     * （连续 3~5 次后不再取 job，ct1ca 停在 QBA、ct1cs 0x30）。
     *
     * _last_spins 记录走的是哪条（日志里 [CNT] / [INT] / [兜底]）：
     * 只有 3) 命中时置最高位，2) 命中时置次高位。 */
    _last_spins = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < 5000000u; i++) {
        uint32_t sts;

        if (cnt_reg != 0 && core[cnt_reg / 4] != cnt_before) {
            sts = core[V3D_INT_STS / 4];
            if (sts != 0u)
                core[V3D_INT_CLR / 4] = sts;
            _last_spins = i;
            return 0;
        }
        sts = core[V3D_INT_STS / 4];
        if ((sts & irq_bit) != 0u) {
            core[V3D_INT_CLR / 4] = sts;         /* ack 整值，同 Linux */
            __asm__ __volatile__("dsb sy" ::: "memory");
            _last_spins = i | 0x40000000u;
            return 0;
        }
        if (cs_reg != 0 && i > CT_DONE_STARTUP_SPIN &&
            (core[cs_reg / 4] & 0x20u) == 0) {   /* CTRUN 清零 = 停转 */
            /* 【只能认 CA == EA】CLE 出错停转时 CA 可能还在很后面/很前面，
             * 用 >= 会把「CA 跑到 +65536、EA 只有 333」的失败判成成功
             * （triangle 实测）。成功完成时 CA 必须正好停在 CL 末尾。 */
            _last_spins = i | 0x80000000u;
            return (ca_reg == 0 || core[ca_reg / 4] == ea) ? 0 : -1;
        }
        __asm__ __volatile__("yield");
    }
    return -1;
}

/* 清 CLE 的完成中断状态。Linux 的 v3d 中断处理里必做（往 INT_CLR 写读到的
 * INT_STS）。我们全程轮询、从不写它，于是 INT_STS 从上电起就一直挂着 0x2。
 *
 * 【为什么这是「第二次提交就挂」的元凶候选】实测同一个 CL 连续提交：前 4 次
 * 都正常进队并执行完（ct1ca +len/len），第 5 次起 CT1 一个字节都不消费
 * （ct1ca +0、ct1cs 0x30），而且加 1~2 秒空转也没用 —— 说明不是「收尾没
 * 做完」，而是完成事件没有被回收，硬件那边的 job 槽位回不来。 */
static uint32_t cle_clear_int(volatile uint32_t *core)
{
    uint32_t sts = core[V3D_INT_STS / 4];
    if (sts != 0u) {
        core[V3D_INT_CLR / 4] = sts;            /* write-1-to-clear */
        __asm__ __volatile__("dsb sy" ::: "memory");
    }
    return sts;                                 /* 清之前的值，供日志分析 */
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

    /* 【不写 CT0CS】同 render 侧：Linux 的 v3d_sched.c 只写
     * CT0QMA/CT0QMS/CT0QTS/CT0QBA/CT0QEA，从不写 CT0CS。
     * 写 0 会复位控制线程（bin 反而一直能跑完，说明本来就不需要）。 */
    core[CLE_CT0QMA / 4] = _pool_p;
    core[CLE_CT0QMS / 4] = V3D_3D_POOL_SIZE;
    core[CLE_CT0QTS / 4] = (1u << 1) | _tsda_p;   /* enable | page-aligned tsda */
    core[PTB_BPOS / 4]   = 0;                     /* kernel zeroes overspill */
    __asm__ __volatile__("dsb sy" ::: "memory");
    v3d_g2d_l2_invalidate_all();
    core[CLE_CT0QBA / 4] = start;
    core[CLE_CT0QEA / 4] = end;                   /* write starts the job */
    rc = wait_flush(core, V3D_INT_FLDONE, CLE_BFC, before, CLE_CT0CS,
                    CLE_CT0CA, end);
    (void)cle_clear_int(core);
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
    } else {
        /* 【成功路径也要看 bpca】bfc 递增只说明 bin CL 执行完了，不代表
         * 产出了 tile 描述。bpca == base 意味着 binner 一个 tile 都没写，
         * 那么 render 的 tile list 是空的，CT1 在 START_ADDRESS_OF_
         * GENERIC_TILE_LIST(0x14) 处会死等（实测 ct1ca 恒在 0x14 末尾、
         * ct1ra=0 从未进入子列表）。这是当前唯一还没确认的环节。 */
        slog("v3d_3d: BIN ok bfc %u->%u bpca 0x%x(+%u) bpcs 0x%x cl_in %u\r\n",
             before, core[CLE_BFC / 4], core[PTB_BPCA / 4],
             core[PTB_BPCA / 4] - _pool_p, core[PTB_BPCS / 4],
             end - start);
        /* bin 成功但 ERR_STAT 新置位时单独一行：render 超时日志里的 err 是
         * 粘性锁存，只有把它归到「哪一次提交产生」才有意义。实测 render
         * 超时时 err 恒为 0x1000 且 new=0，说明它在 render 之前就有了 ——
         * 这里能确认它是不是 binner 产生的。 */
        if (core[V3D_ERR_STAT / 4] != err_before)
            slog("v3d_3d: BIN err %x->%x new %x\r\n",
                 err_before, core[V3D_ERR_STAT / 4],
                 core[V3D_ERR_STAT / 4] & ~err_before);
    }
    return rc;
}

/* 【CL hex dump】超时时把 CL 前 80 字节和「已消费位置」一起打出来，
 * 用于判断 CT 停在哪条命令上（裸机环境没有其它手段）。 */
static void dump_cl(const char *tag, uint32_t start, uint32_t consumed) {
    const uint8_t *p = (const uint8_t *)v3d_dma_cpu(start);
    uint32_t n = 80;
    uint32_t i;
    slog("v3d_3d: %s CL dump (consumed=%u):\r\n", tag, consumed);
    for (i = 0; i < n; i += 16) {
        slog("  %3u:%s %02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x\r\n",
             i, (i <= consumed && consumed < i + 16) ? ">" : " ",
             p[i+0], p[i+1], p[i+2], p[i+3], p[i+4], p[i+5], p[i+6], p[i+7],
             p[i+8], p[i+9], p[i+10], p[i+11], p[i+12], p[i+13], p[i+14],
             p[i+15]);
    }
}

/* settle: 写 QBA/QEA 之前先空转的 yield 次数（探针用它区分「CLE 收尾没完」
 * 和「必须 re-arm」两种可能）。clear_cs: 写 QBA/QEA 前把 CT1CS 清 0
 * （EwokOS 裸机 bring-up 的做法，用来验证“每次提交是否需要显式 re-arm”）。 */
static int submit_render_ex(volatile uint32_t *core, uint32_t start,
                            uint32_t end, uint32_t settle, int clear_cs)
{
    uint32_t before = core[CLE_RFC / 4];
    /* ERR_STAT 是粘性锁存：绝对值里混着更早实验的位，只有「这次提交期间新
     * 出现的位」能归因到本次 job。bin 侧早就在做这个差分，render 侧原来只
     * 打绝对值（日志里那个 0x1000 就是没法定性的老锁存）。 */
    uint32_t err_before = core[V3D_ERR_STAT / 4];
    uint32_t ca_before = core[CLE_CT1CA / 4];
    uint32_t i;
    uint32_t sts_after;
    int rc;

    if (_ct1_wedged) {
        slog("v3d_3d: RENDER skipped (CT1 已在之前的提交里卡死，需要重启 GPU 服务/整机)\r\n");
        return -1;
    }

    /* 跨 gpud 进程残留：上一次超时把 CT1 留在 CTRUN=1，本进程若没做
     * SMS reset 就会在这里看见。CA 常停在旧 arena 的 +4096 页边界，
     * 看起来像「这次 CL 跑了 4K」其实是残值。 */
    {
        uint32_t cs_pre = core[CLE_CT1CS / 4];
        if ((cs_pre & 0x20u) != 0u) {
            slog("v3d_3d: RENDER aborted — CT1 already CTRUN "
                 "ct1cs=0x%x ca=0x%x (需要 SMS/PM reset，勿解读为本 CL 进度)\r\n",
                 cs_pre, core[CLE_CT1CA / 4]);
            _ct1_wedged = 1;
            return -1;
        }
    }

    if (settle != 0u) {
        for (i = 0; i < settle; i++)
            __asm__ __volatile__("yield");
    }

    /* 【提交序列——以本仓库已跑通 probeA/B/C 的做法为准】
     *   1) L2T + slice 全失效：bin 刚从同一 _cl_p 取过指，L2T 里可能还
     *      留着旧 bin CL；CPU dma_clean 只推 ARM cache，推不掉 V3D L2T。
     *      Linux v3d_render_job_run() 每次提交前也做 invalidate。
     *   2) 【不写 CT1QCFG】默认 0xC0；写 0 会让 CT1 停取（附录 I.3 #5）。
     *   3) 【不写 CT1CS】Linux 只写 QBA/QEA；写 0 会留下假 CA 残值。
     *      CT1 真卡死只能靠 SMS/PM 复位（backend 启动时检测 CTRUN）。
     *   4) 只写 QBA/QEA 启动；完成靠 wait_flush 的 CTRUN+CA 兜底
     *      （render CL 无 FLUSH，RFC 不会动）。 */
    v3d_g2d_l2_invalidate_all();
    (void)clear_cs;
    __asm__ __volatile__("dsb sy" ::: "memory");
    core[CLE_CT1QBA / 4] = start;
    core[CLE_CT1QEA / 4] = end;
    rc = wait_flush(core, V3D_INT_FRDONE, CLE_RFC, before, CLE_CT1CS,
                    CLE_CT1CA, end);
    sts_after = cle_clear_int(core);
    if (rc != 0) {
        uint32_t ct1ca = core[CLE_CT1CA / 4];
        uint32_t err_after = core[V3D_ERR_STAT / 4];
        slog("v3d_3d: RENDER timeout rfc %u->%u intsts 0x%x ct0cs 0x%x ct1cs 0x%x cl [%u,%u)\r\n",
             before, core[CLE_RFC / 4], core[V3D_INT_STS / 4],
             core[CLE_CT0CS / 4], core[CLE_CT1CS / 4], start, end);
        /* CT1CA/CT1RA 是定位关键：CA=消费到 CL 的哪个字节、
         * RA=子列表返回地址。CT1CA==start 表示一个命令都没消费；
         * ==start+1 表示第一条 opcode 之后停住。
         *
         * 【必须打相对偏移】CL 缓冲换过地址后绝对地址没有可比性；而且
         * 一次卡死之后 CT1 不会自己恢复，后续提交的 CA 会一直停在上一次
         * 留下的值上（三角形那次 +52 就是 clear 留下的残值，不是它自己
         * 的执行进度）。相对偏移配 tools/v3d_cl_decode.py 才能一眼看出
         * 停在哪个包的哪个字节。 */
        slog("v3d_3d: RENDER ct1ca +%d ct1ra 0x%x ct1lc %u err %x->%x new %x\r\n",
             (int)(ct1ca - start), core[CLE_CT1RA / 4],
             core[CLE_CT1LC / 4], err_before, err_after,
             err_after & ~err_before);
        /* 回读队列寄存器：确认「写进去的 QBA/QEA 有没有被硬件接受/改掉」。
         * 若 qba/qea 仍然等于我们写的值而 CA 不动，说明 job 入队了但线程
         * 没被唤醒；若被改掉，说明硬件在校验/丢弃这个 job。 */
        slog("v3d_3d: RENDER qba 0x%x qea 0x%x qcfg 0x%x wrote [0x%x,0x%x)\r\n",
             core[CLE_CT1QBA / 4], core[CLE_CT1QEA / 4],
             core[CLE_CT1QCFG / 4], start, end);
        dump_cl("RENDER", start, ct1ca - start);
        _ct1_wedged = 1;
        stalled("render timeout");
    } else {
        /* 成功也留一行：确认 CA 走到了 CL 末尾、RFC 有没有递增，避免
         * 「rc=0 但什么都没画」的情况没有数据可看。 */
        /* 【假成功识别】CA 在这次提交前后完全没变、又正好等于 EA，说明
         * 硬件很可能根本没执行这个 job（典型场景：同地址同长度重发，
         * QEA 与上次相同）。这种 rc=0 不能当成「跑通了」。 */
        slog("v3d_3d: RENDER ok rfc %u->%u ct1ca +%d/%u ct1cs 0x%x int_sts 0x%x spin 0x%x%s%s\r\n",
             before, core[CLE_RFC / 4], (int)(core[CLE_CT1CA / 4] - start),
             end - start, core[CLE_CT1CS / 4], sts_after, _last_spins,
             (_last_spins & 0x80000000u) ? " [兜底]"
                 : ((_last_spins & 0x40000000u) ? " [INT]" : " [CNT]"),
             (ca_before == core[CLE_CT1CA / 4] && ca_before == end)
                 ? "  <-- CA 未变化，可能未执行" : "");
    }
    return rc;
}

static int submit_render(volatile uint32_t *core, uint32_t start, uint32_t end)
{
    return submit_render_ex(core, start, end, 0u, 0);
}

static uint32_t build_bcl_empty(uint32_t w, uint32_t h)
{
    uint32_t off = 0;

    off = cl_u8(off, 0x77);                     /* NUMBER_OF_LAYERS */
    off = cl_u8(off, 0x00);                     /* layers - 1 = 0 */
    off = cl_u8(off, 0x78);                     /* TILE_BINNING_MODE_CFG */
    /* [foot] 两个 block size 不一样！mesa v3d_limits.h：
     *   V3D_TILE_ALLOC_INITIAL_BLOCK_SIZE  = 128 → 枚举 1
     *   V3D_TILE_ALLOC_OVERFLOW_BLOCK_SIZE =  64 → 枚举 0
     * 位域：bits4-5 = tile_allocation_block_size（溢出块）
     *       bits2-3 = tile_allocation_initial_block_size（初始块）
     *
     * 曾经把两者都写成 1（128B），结果 binner 每 tile 用 128+128=256B
     * （实测 bpca = 80×256 = 0x5000），而 render 按 64B 溢出块遍历
     * → 每个 tile 错位 64B → CT 卡在第一个 STORE 之后不动。 */
    off = cl_u8(off, (0u << 4) | (1u << 2));    /* overflow 64b, initial 128b */
    /* 16px tiles (log2t=1)，与 build_clear_rcl 的显式逐 tile 网格一致。
     * 参考仓库验证过的空 bin 就是 16px；render 虽然不读 binner 的 tile
     * list，但让 bin/render 的 tile 几何一致能少一个变量。640x480 正好
     * 是 40x30 个整 tile。 */
    off = cl_u8(off, (1u << 3) | (1u << 0));
    off = cl_u8(off, 0x00);
    off = cl_u8(off, 0x00);
    off = cl_u16le(off, w - 1u);
    off = cl_u16le(off, h - 1u);
    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */
    off = cl_u8(off, 0x06);                     /* START_TILE_BINNING */
    off = cl_u8(off, 0x04);                     /* FLUSH */
    return off;
}

/* RCL 前导配置：TILE_RENDERING_MODE_CFG_COMMON + RT_PART1 + ZS_CLEAR_VALUES。
 * build_clear_rcl 与「clear 前缀梯度探针」共用，保证探针测的就是真实 CL 的
 * 前缀，而不是另写一份可能已经跑偏的配置。
 *
 * log2t 是 Mesa 的 "log2 tile size in 8-pixel units"：16px tile -> 1，
 * 64px tile -> 3。RT stride 也必须按 tile 宽度算。 */
static uint32_t rcl_cfg_prefix(uint32_t off, uint32_t w, uint32_t h,
                               uint32_t fmt, uint32_t rgba8, uint32_t log2t)
{
    uint32_t tile = 1u << (log2t + 3u);      /* 16px: log2t=1; 64px: log2t=3 */
    /* clear color 按 V3D_INTERNAL_TYPE_8 的内部格式打包；rgba8 约定 0xAARRGGBB。
     * 【必须是 RGBA8888，不是 RGB565】mesa v3dx_draw.c:1733 对
     * V3D_INTERNAL_TYPE_8 用 PIPE_FORMAT_R8G8B8A8_UNORM 打包，32 位小端
     * 布局是 A<<24 | B<<16 | G<<8 | R。之前在这里算 RGB565，0x1F 落进
     * R 字节，屏幕上整片变成红色（实证）。 */
    uint32_t a8 = (rgba8 >> 24) & 0xFFu;
    uint32_t r8 = (rgba8 >> 16) & 0xFFu;
    uint32_t g8 = (rgba8 >> 8) & 0xFFu;
    uint32_t b8 = rgba8 & 0xFFu;
    uint32_t c32 = (a8 << 24) | (b8 << 16) | (g8 << 8) | r8;
    (void)fmt;                      /* internal type 目前固定 8（见 RT 注释） */

    /* TILE_RENDERING_MODE_CFG_COMMON (0x79, sub_id 0) */
    off = cl_u8(off, 0x79);
    off = cl_u8(off, 0x00);                     /* nrt-1 = 0, sub_id = 0 */
    off = cl_u16le(off, w);
    off = cl_u16le(off, h);
    off = cl_u8(off, (1u << 6) | (1u << 4));    /* early_z_disable, depth_buf_disable */
    /* v71: Log2 Tile Width 在 bit52-54（byte7 bit4-6），Log2 Tile Height
     * 在 bit55-57 —— 跨 byte7 bit7 + byte8 bit0-1。按字段值直接写，
     * 不能假定 log2t 的 bit0 恒为 1（老代码写死 1<<7，只对 64px 成立）。 */
    off = cl_u8(off, (uint8_t)(((log2t & 1u) << 7) | (log2t << 4)));
    off = cl_u8(off, (uint8_t)((log2t >> 1) & 0x3u));

    /* TILE_RENDERING_MODE_CFG_RENDER_TARGET_PART1 (sub_id 2)。
     * v71 字段：sub_id(3) rt(3) base(11,单位512B,start7) stride(7,单位64B,
     * minus_one,start18) bpp(2,start25) type(5,start27) clear_low(32,start32)。
     * 权威参数（mesa v3d_util.c / v3dx_format_table.c）：
     *   stride = tile_width_px * internal_bpp_words / 2
     *            （单位 128-bit，覆盖 2 行，内部 tile 布局的行距，
     *             与图像行距无关——写屏幕靠 STORE 的 32 位地址）
     *   BGR565: internal_type = V3D_INTERNAL_TYPE_8（枚举 8），
     *           internal_bpp = 32（bpp_words = 1）
     *   base_address 是内部 tile 缓冲的相对布局，不是 FB 地址 */
    {
        /* 【字段存的是 stride - 1】v3d_packet_v71_pack.h:3699
         *   cl[3] = bitpack(stride - 1, 2, 8) | base >> 16
         *   cl[4] = bitpack(stride - 1, 2, 8) >> 8 | ...
         * 即 7 位域（cl[3] bits2-7 = 低 6 位，cl[4] bit0 = 第 7 位）。
         * 塞 rt_stride(32) 会让硬件按 stride=33 解析。 */
        uint32_t rt_stride_m1 = tile / 2u - 1u;   /* 16px: 7; 64px: 31 */
        off = cl_u8(off, 0x79);
        off = cl_u8(off, 0x02);                 /* sub_id = 2, rt = 0, base[0:2]=0 */
        off = cl_u8(off, 0x00);                 /* base bits1-8 = 0 */
        off = cl_u8(off, (uint8_t)((rt_stride_m1 & 0x3Fu) << 2));
        off = cl_u8(off, (uint8_t)(((rt_stride_m1 >> 6) & 0x01u) | (8u << 3)));
        off = cl_u32le(off, c32);               /* clear color low 32（RGBA8888） */
    }

    /* TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES (sub_id 1): z=1.0, stencil=0 */
    off = cl_u8(off, 0x79);
    off = cl_u8(off, 0x01);
    off = cl_u8(off, 0x00);
    off = cl_u32le(off, 0x3F800000u);           /* z = 1.0f */
    off = cl_u16le(off, 0x0000);
    return off;
}

/* tile 列表基础设施：0x7E TILE_LIST_INITIAL_BLOCK_SIZE +
 * 0x7B MULTICORE_RENDERING_TILE_LIST_SET_BASE（binner 写的 tile 池）+
 * 0x7A MULTICORE_RENDERING_SUPERTILE_CFG。
 *
 * 【必须在第一条 TILE_COORDINATES(0x7C) 之前】0x7C 让 CLE 进入「tile 扫描」
 * 状态，这时 tile 列表基址 / 首块大小 / supertile 几何必须已经合法。缺这
 * 三个包的 CL 在 0x7C 处直接停住、一个字节都不消费（实测 ct1ca +0、
 * ct1cs 0x30）。build_clear_rcl 原来就缺这块（draw 路径有），这是清屏从
 * 来没跑通过的直接原因。
 *
 * 顺序照 mesa v3dx_rcl.c:576-617 / 928-932：0x7E 在 RT/ZS 配置之后，
 * 0x7B 在 0x7A 之前。fw/fh 返回 supertile 网格，调用方据此发 0x17。 */
static uint32_t rcl_emit_tile_list_cfg(uint32_t off, uint32_t tx, uint32_t ty,
                                       uint32_t pool_iova, uint32_t mask,
                                       uint32_t *sup_fw, uint32_t *sup_fh)
{
    uint32_t sw = 1, sh = 1, fw, fh;

    for (;;) {
        fw = (tx + sw - 1u) / sw;
        fh = (ty + sh - 1u) / sh;
        if (fw < 256u && fh < 256u && fw * fh <= 256u)
            break;
        if (sw < sh)
            sw++;
        else
            sh++;
    }

    /* mask: bit0 = 0x7E，bit1 = 0x7A，bit2 = 0x7B。
     * 正常路径传 7；探针用它逐个单独发，定位到底是哪个包让 CT1 不启动。 */
    if (mask & 1u) {
        /* auto-chain(bit2) + 首块 128B（枚举 1）。必须与 binner 的
         * tile_allocation_initial_block_size 一致，否则 CLE 按错的块大小
         * 遍历 binner 写出的 tile 描述。 */
        off = cl_u8(off, 0x7E);
        off = cl_u8(off, (1u << 2) | 1u);
    }
    if (mask & 4u) {
        /* tile 池基址（binner 的 CT0QMA） */
        off = cl_u8(off, 0x7B);
        off = cl_u32le(off, pool_iova);
    }
    if (mask & 2u) {
        /* supertile 几何 + 整帧 tile 数 + bin tile list 数(1) + 单核 */
        off = cl_u8(off, 0x7A);
        off = cl_u8(off, (uint8_t)(sw - 1u));
        off = cl_u8(off, (uint8_t)(sh - 1u));
        off = cl_u8(off, (uint8_t)fw);
        off = cl_u8(off, (uint8_t)fh);
        off = cl_u8(off, (uint8_t)(tx & 0xFFu));
        off = cl_u8(off, (uint8_t)(((ty & 0x0Fu) << 4) | ((tx >> 8) & 0x0Fu)));
        off = cl_u8(off, (uint8_t)((ty >> 4) & 0xFFu));
        off = cl_u8(off, 0x00);             /* nblists-1=0, multicore off */
    }

    if (sup_fw) *sup_fw = fw;
    if (sup_fh) *sup_fh = fh;
    return off;
}

/* STORE_TILE_BUFFER_GENERAL（13 字节，v71 包布局见 Mesa pack 头）。
 * buffer: 0 = RASTER(RT0)，8 = NONE（dummy，不写显存）。
 * imgfmt: V3D_3D_FMT_*；stride 是屏幕行距（字节）；addr 是图像基址。
 * 每个 tile 的 STORE 都写同一个基址，硬件用 TILE_COORDINATES + stride
 * 自己算出该 tile 在图像里的位置。 */
static uint32_t rcl_emit_store(uint32_t off, uint32_t buffer, uint32_t imgfmt,
                               int rb_swap, uint32_t stride, uint32_t addr)
{
    off = cl_u8(off, 0x1D);                     /* STORE_TILE_BUFFER_GENERAL */
    off = cl_u8(off, (uint8_t)(buffer & 0x0Fu));
    off = cl_u8(off, (uint8_t)((imgfmt & 0x0Fu) << 4));
    off = cl_u8(off, (uint8_t)((rb_swap ? 0x10u : 0u) |
                               ((imgfmt >> 4) & 0x03u)));
    off = cl_u8(off, (uint8_t)((stride & 0x0Fu) << 4));
    off = cl_u8(off, (uint8_t)((stride >> 4) & 0xFFu));
    off = cl_u8(off, (uint8_t)((stride >> 12) & 0xFFu));
    off = cl_u16le(off, 0x00);                  /* height（mip 层用，0） */
    off = cl_u32le(off, addr);
    return off;
}

/* dummy STORE：buffer_to_store = NONE(8)，不产生任何写，只用来把 TLB 的
 * 内部 type/size 变更和后续 QPU/TLB 操作隔开（GFXH-1742）。 */
static uint32_t rcl_emit_store_none(uint32_t off)
{
    return rcl_emit_store(off, 8u, 0u, 0, 0u, 0u);
}

/*
 * clear RCL —— 【照 videocore7-exps v3d_build_clear_rcl() 的真机验证配方】
 *
 * 关键点：**不使用** tile-list 基础设施（0x7E / 0x7B / 0x7A）、
 * 不使用 generic sub-list（0x14/0x15/0x7D/0x17），而是用 0x7C
 * TILE_COORDINATES 显式遍历每一个 tile。参考仓库就是在 Pi5 上这样
 * 连续跑通 64x64、640x480、显示 BGR565 三次 clear 的；它的 RCL 里
 * 明确注释「no tile-list machinery ... the explicit per-tile
 * TILE_COORDINATES below drive the render without reading the binner's
 * tile lists, which are garbage for an empty frame」。
 *
 * 640x480 用 16px tile 正好是 40x30=1200 个整 tile（无残边），CL 约
 * 23KB，仍在 64KB 的 V3D_3D_CL_MAX 内。之前用 64px tile 时 480 不是
 * 64 的整数倍，最后一个 tile 是残 tile，属于未验证变量。
 */
static uint32_t build_clear_rcl(uint32_t fb_iova, uint32_t stride_bytes,
                                uint32_t w, uint32_t h, uint32_t tile,
                                uint32_t fmt, uint32_t rgba8, int rb_swap)
{
    const uint32_t log2t = 1;       /* 16px tile（Mesa: 16px -> 1） */
    uint32_t off = 0;
    uint32_t tx = (w + tile - 1u) / tile;
    uint32_t ty = (h + tile - 1u) / tile;
    uint32_t x, y, i;

    /* 本配方只对 16px tile 验证过；调用方传别的尺寸时直接按 16px 算。 */
    tile = 16u;
    tx = (w + tile - 1u) / tile;
    ty = (h + tile - 1u) / tile;

    off = rcl_cfg_prefix(off, w, h, fmt, rgba8, log2t);

    /* initial TLB clear：2 轮（GFXH-1742 workaround）。
     * 两轮都发 0x7C(0,0) + END_OF_LOADS + dummy STORE，
     * CLEAR_RENDER_TARGETS 只在第一轮发（mesa 对非 double-buffer 的条件）。 */
    for (i = 0; i < 2u; i++) {
        off = cl_u8(off, 0x7C);                 /* TILE_COORDINATES (0,0) */
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x00);
        off = cl_u8(off, 0x1A);                 /* END_OF_LOADS */
        off = rcl_emit_store_none(off);
        if (i == 0)
            off = cl_u8(off, 0x19);             /* CLEAR_RENDER_TARGETS */
        off = cl_u8(off, 0x1B);                 /* END_OF_TILE_MARKER */
    }

    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */

    /* 显式逐 tile：坐标 -> 结束 load 模式 -> STORE 到 FB -> tile 结束 */
    for (y = 0; y < ty; y++) {
        for (x = 0; x < tx; x++) {
            off = cl_u8(off, 0x7C);             /* TILE_COORDINATES */
            off = cl_u8(off, (uint8_t)(x & 0xFFu));
            off = cl_u8(off, (uint8_t)(((y & 0x0Fu) << 4) |
                                       ((x >> 8) & 0x0Fu)));
            off = cl_u8(off, (uint8_t)((y >> 4) & 0xFFu));
            off = cl_u8(off, 0x1A);             /* END_OF_LOADS */
            off = rcl_emit_store(off, 0u, fmt, rb_swap, stride_bytes, fb_iova);
            off = cl_u8(off, 0x1B);             /* END_OF_TILE_MARKER */
        }
    }

    off = cl_u8(off, 0x0D);                     /* END_OF_RENDERING */
    /* 【不要在 render CL 里发 FLUSH(0x04)】它只对 bin 线程有意义。 */
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
    off = cl_u8(off, (0u << 4) | (1u << 2));    /* overflow 64b, initial 128b */
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
    /* supertile 几何在 0x7A 里算出、在末尾的 0x17 列表里复用 */
    uint32_t sup_w = 1, sup_h = 1, sup_fw, sup_fh;
    uint32_t sx, sy;

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
    off = cl_u8(off, (1u << 2) | (1u << 0));   /* auto-chain + 128b block */
    /* MULTICORE_RENDERING_TILE_LIST_SET_BASE (0x7B) -> binning pool.
     * 顺序与 mesa v3dx_rcl.c:576/580 一致：SET_BASE 在前、SUPERTILE_CFG 在后。 */
    off = cl_u8(off, 0x7B);
    off = cl_u32le(off, _pool_p);
    /* MULTICORE_RENDERING_SUPERTILE_CFG (0x7A)
     *
     * 字段布局（小端 64 位参数，v3d_packet.xml code=122）：
     *   bit0-7    supertile_width_in_tiles  - 1
     *   bit8-15   supertile_height_in_tiles - 1
     *   bit16-23  total_frame_width_in_supertiles
     *   bit24-31  total_frame_height_in_supertiles
     *   bit32-43  total_frame_width_in_tiles
     *   bit44-55  total_frame_height_in_tiles
     *   bit56     multicore_enable
     *   bit60     supertile_raster_order
     *   bit61-63  number_of_bin_tile_lists - 1
     *
     * 【曾经的错误】原来写的是「supertile = tx×ty（10×8）、frame = 1×1」——
     * 把两个概念写反了。硬件 supertile 尺寸有上限，10×8 直接让 CT1 挂死
     * （前缀探针实测：prefix 0x7A STUCK、ct1cs=0x20 不清零，且此后
     * CT1 再也无法恢复，连 probeA 单字节 END 都失败）。
     *
     * mesa 的算法（v3dx_rcl.c:583-606）：从 1×1 起，若 supertile 数量
     * 超 V3D_MAX_SUPERTILES(256) 就增长较小的一边。10×8 tiles 时
     * 1×1 就满足（80 ≤ 256），所以 supertile=1×1、frame=10×8。 */
    {
        for (;;) {
            sup_fw = (tx + sup_w - 1u) / sup_w;
            sup_fh = (ty + sup_h - 1u) / sup_h;
            if (sup_fw < 256u && sup_fh < 256u && sup_fw * sup_fh <= 256u) {
                break;
            }
            if (sup_w < sup_h) {
                sup_w++;
            } else {
                sup_h++;
            }
        }
        off = cl_u8(off, 0x7A);
        off = cl_u8(off, (uint8_t)(sup_w - 1u));/* supertile w - 1 (tiles) */
        off = cl_u8(off, (uint8_t)(sup_h - 1u));/* supertile h - 1 (tiles) */
        off = cl_u8(off, (uint8_t)sup_fw);      /* frame w in supertiles */
        off = cl_u8(off, (uint8_t)sup_fh);      /* frame h in supertiles */
        off = cl_u8(off, (uint8_t)(tx & 0xFFu));/* total frame w in tiles */
        off = cl_u8(off, (uint8_t)(((ty & 0x0Fu) << 4) | ((tx >> 8) & 0x0Fu)));
        off = cl_u8(off, (uint8_t)((ty >> 4) & 0xFFu));
        off = cl_u8(off, 0x00);                 /* nblists-1=0, multicore off */
    }
    /* TILE_COORDINATES (0x7C, 4 字节)：告诉硬件「从 tile (0,0) 开始扫描」。
     *
     * 【曾经被误删】禁用 GFXH-1742 段时把这段里仅有的两个 0x7C 一起删了，
     * 结果 CT1 不知道从哪里开始，直接挂住不动 —— 实测 ct1ca 恒为
     * start+9（Common 末尾）、ct1cs 保持 0x20/0x60（CTRUN 不清），
     * 且此后 CT1 完全无法恢复（连单字节 END 都跑不了）。
     *
     * mesa v3dx_rcl.c:620 在 MULTICORE_RENDERING_SUPERTILE_CFG 之后
     * 立即发这条，binning 起点才确定。 */
    off = cl_u8(off, 0x7C);                     /* TILE_COORDINATES (0,0) */
    off = cl_u8(off, 0x00);                     /* tile column number */
    off = cl_u8(off, 0x00);                     /* tile row number */
    off = cl_u8(off, 0x00);                     /* 补齐到 4 字节 */
    /* initial TLB clear (2x, global, GFXH-1742 workaround): only on the first
     * dispatch of a frame, which initialises colour (RT_PART1) + Z (ZS_CLEAR)
     * for the whole frame.  Later dispatches LOAD the persisted buffers, so
     * clearing here would erase the frame accumulated so far. */
    /* 【临时禁用 GFXH-1742 段做二分】这段是「TLB 内部 type/size 变化时
     * QPU 拿到旧状态」的 race workaround（mesa v3dx_rcl.c:638）。实测
     * CT 稳定停在 start+52（= 0x7C 之后 / STORE 参数中段），先确认
     * 问题是否出在这段：禁用后若 ct1ca 越过 43，说明就是它；若仍停，
     * 说明在更后面（gen tile list / STORE）。
     *
     * 权威长度（v3d_packet_v71_pack.h）：
     *   TILE_COORDINATES            = 4  （不是 9！）
     *   STORE_TILE_BUFFER_GENERAL   = 13
     *   TILE_RENDERING_MODE_CFG_COMMON = 9
     */
#if V3D_3D_ENABLE_GFXH1742
    if (first_frame) {
        for (i = 0; i < 2; i++) {
            if (i > 0) {
                off = cl_u8(off, 0x7C);         /* TILE_COORDINATES (0,0) */
                off = cl_u8(off, 0x00);         /* tile column (12 bit) */
                off = cl_u8(off, 0x00);         /* tile row (12 bit) */
                off = cl_u8(off, 0x00);         /* 共 4 字节 */
            }
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
            if (i == 0)                         /* 同 build_clear_rcl：
                                                 * mesa 只在 i==0 清 */
                off = cl_u8(off, 0x19);         /* CLEAR_RENDER_TARGETS */
            off = cl_u8(off, 0x1B);             /* END_OF_TILE_MARKER */
        }
    }
#endif /* V3D_3D_ENABLE_GFXH1742 */
    off = cl_u8(off, 0x13);                     /* FLUSH_VCD_CACHE */
    /* generic tile list built directly into _gen (the cl_* helpers write
     * into _cl and would clobber the RCL being assembled there). */
    {
        uint8_t *gp = _gen_va;
        uint32_t g = 0;
        int zpersist = (depth_en && z_iova != 0);
        /* 【开头必须是 0x7D TILE_COORDINATES_IMPLICIT，中段才是 0x15】
         * mesa v3dx_rcl.c:344-385 的 generic per-tile list 顺序是：
         *   0x7D TILE_COORDINATES_IMPLICIT   ← 取本 tile 的坐标，开启一个 tile
         *   [LOAD_TILE_BUFFER_GENERAL ...]   ← 需要 load 的缓冲
         *   0x1A END_OF_LOADS                ← 切 load 模式 -> render 模式
         *   0x38 PRIM_LIST_FORMAT
         *   0x36 SET_INSTANCEID
         *   0x15 BRANCH_TO_IMPLICIT_TILE_LIST ← 执行 binner 写好的本 tile 图元
         *   [STORE_TILE_BUFFER_GENERAL ...]
         *   0x1B END_OF_TILE_MARKER
         *   0x12 RETURN_FROM_SUB_LIST
         * 之前把开头的 0x7D 换成了 0x15（理由写在旧注释里），结果整个
         * tile 循环没有「开始一个新 tile」的命令，CT 执行完主 CL 的 0x14
         * 之后就没有合法入口（ct1ca 恒在 0x14 处、CTRUN 不清）。0x15 只
         * 该出现在中段那一个位置。 */
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
        /* 【必须 clean】generic tile list 也是 CPU 写的 GPU 数据。
         * 之前漏了这一步：CL 主体 clean 了（所以 0x14 之前的命令都能
         * 执行），但 _gen 里的内容停在 CPU cache，GPU 从 0x4b064000
         * 读到全 0 → CT 执行完 0x14（Start Address of Generic Tile
         * List）之后就等着，永远等不到合法命令
         * （实测 ct1ca 恒等于 0x14 命令末尾、ct1cs 带 CTRUN）。 */
        dma_clean(_gen_p, g);
        off = cl_u8(off, 0x14);                 /* START_ADDRESS_OF_GENERIC_TILE_LIST */
        off = cl_u32le(off, _gen_p);
        off = cl_u32le(off, _gen_p + g);
    }
    /* SUPERTILE_COORDINATES：这条命令的含义是「本 supertile 有内容，按它
     * 的 tile list 执行上面 0x14 指向的 generic list」。mesa 对每个和
     * scissor 相交的 supertile 发一条（v3dx_rcl.c:686-697）。
     * 这里原来只发 (0,0)：640x480 / 64px tile 是 10x8 个 tile、supertile
     * 1x1，所以只有左上角那一个 tile 会被渲染，其余 79 个永远不画。全帧
     * 覆盖就按 supertile 网格全发（scissor 剔除以后再做）。 */
    for (sy = 0; sy < sup_fh; sy++) {
        for (sx = 0; sx < sup_fw; sx++) {
            off = cl_u8(off, 0x17);             /* SUPERTILE_COORDINATES */
            off = cl_u8(off, (uint8_t)sx);
            off = cl_u8(off, (uint8_t)sy);
        }
    }
    off = cl_u8(off, 0x0D);                     /* END_OF_RENDERING */
    return off;
}


/* ============ 正式 CL 提交 API（Mesa 桥接用） ============
 * CL 由调用者（Mesa/应用）生成，[start,end) 是 GPU VA。
 * 这里只做「提交」：boot halt 清除、PTB/QMA/QMS/QTS、L2 维护、
 * 写 QBA/QEA 启动、等 BFC/RFC 递增。CL 内容的正确性由调用者保证。 */
uint32_t v3d_3d_cl_base(void) { return _cl_p; }

int v3d_3d_submit_cl(uint32_t bcl_start, uint32_t bcl_end,
                     uint32_t rcl_start, uint32_t rcl_end,
                     uint32_t qma, uint32_t qms, uint32_t qts,
                     uint32_t w, uint32_t h) {
  volatile uint32_t* core;
  uint32_t bfc0, rfc0;
  uint32_t pool = qma ? qma : _pool_p;
  uint32_t pool_size = qms ? qms : V3D_3D_POOL_SIZE;
  uint32_t tsda = qts ? qts : _tsda_p;
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
  /* 不写 CT0CS（同 submit_bin）：Linux 只写 QMA/QMS/QTS/QBA/QEA。 */
  core[CLE_CT0QMA / 4] = pool;
  core[CLE_CT0QMS / 4] = pool_size;
  core[CLE_CT0QTS / 4] = (1u << 1) | tsda;
  core[PTB_BPOS / 4] = 0;
  __asm__ __volatile__("dsb sy" ::: "memory");
  v3d_g2d_l2_invalidate_all();
  core[CLE_CT0QBA / 4] = bcl_start;
  core[CLE_CT0QEA / 4] = bcl_end;
  rcb = wait_flush(core, V3D_INT_FLDONE, CLE_BFC, bfc0, CLE_CT0CS,
                   CLE_CT0CA, bcl_end);
  if (rcb != 0) {
    slog("v3d_3d: submit_cl BIN timeout bfc %u->%u ct0ca 0x%x ct0cs 0x%x\r\n",
         bfc0, core[CLE_BFC / 4], core[CLE_CT0CA / 4],
         core[CLE_CT0CS / 4]);
  } else {
    printf("v3d_3d: submit_cl BIN ok bfc %u->%u ct0ca 0x%x bpca 0x%x bpcs 0x%x\n",
           bfc0, core[CLE_BFC / 4], core[CLE_CT0CA / 4],
           core[PTB_BPCA / 4], core[PTB_BPCS / 4]);
  }

  /* ---- render ---- */
  rfc0 = core[CLE_RFC / 4];
  /* 不写 CT1CS（同 submit_render_ex）：写 0 会留下假 CA 残值。 */
  __asm__ __volatile__("dsb sy" ::: "memory");
  v3d_g2d_l2_invalidate_all();
  core[CLE_CT1QBA / 4] = rcl_start;
  core[CLE_CT1QEA / 4] = rcl_end;
  rcr = wait_flush(core, V3D_INT_FRDONE, CLE_RFC, rfc0, CLE_CT1CS,
                   CLE_CT1CA, rcl_end);
  if (rcr != 0) {
    slog("v3d_3d: submit_cl RENDER timeout rfc %u->%u ct1ca 0x%x ct1cs 0x%x\r\n",
         rfc0, core[CLE_RFC / 4], core[CLE_CT1CA / 4],
         core[CLE_CT1CS / 4]);
  } else {
    printf("v3d_3d: submit_cl RENDER ok rfc %u->%u ct1ca 0x%x ct1cs 0x%x int_sts 0x%x\n",
           rfc0, core[CLE_RFC / 4], core[CLE_CT1CA / 4],
           core[CLE_CT1CS / 4], core[V3D_INT_STS / 4]);
  }

  v3d_g2d_op_end();
  return (rcb == 0 && rcr == 0) ? V3D_3D_DONE : V3D_3D_FAILED;
}

static void stalled(const char *why)
{
    slog("v3d_3d: %s on job: %s\r\n", why, _job_desc);
    v3d_g2d_recover(why);
}

/* 【CL 前缀二分探针】把 render CL 按命令边界逐步截断后提交，
 * 看从哪一条开始 CT 的 CTRUN 不再清零。
 *
 * 命令边界来自 CL dump 实测（triangle 路径）：
 *   9=Common 18=RT_PART1 27=sub_id1 29=0x7E 38=0x7A
 *   43=0x7B 44=FLUSH_VCD 53=START_GENERIC_TILE_LIST 57=END
 *
 * 每个前缀只等 ~0.3s（不等完整超时），避免整轮太慢。 */
int v3d_3d_probe_prefix(void)
{
    /* 用「功能单元」而不是「单条命令」作边界：0x7B(tile list base) 和
     * 0x7A(supertile cfg) 必须成对出现，单独截断到 0x7B 会让硬件缺
     * supertile 配置而假性卡住（上一轮 prefix 0x7B STUCK 即是此假象）。
     *
     * 边界（build_draw_rcl 发射顺序）：
     *   Common(9) RT1(18) sub1(27) 0x7E(29) [0x7B+0x7A](43) 0x7C(47)
     *   0x13(48) 0x14(57) 0x17(60) END(61) */
    static const uint32_t bounds[] = {9, 18, 27, 29, 47, 48, 57, 61};
    static const char *names[] = {
        "Common", "RT1", "sub1", "0x7E", "7B+7A+7C", "0x13", "0x14", "END"};
    volatile uint32_t *core;
    uint32_t len;

    if (!_ready) {
        return V3D_3D_UNSUPPORTED;
    }
    core = v3d_g2d_core_regs();
    if (core == NULL) {
        return V3D_3D_FAILED;
    }

    /* 用 triangle 的 render CL（有真图元）作为基准 */
    len = build_draw_rcl(0x3f800000u /* FB_GVA */, 2u * 640u /* stride */,
                         V3D_3D_FMT_BGR565, 0 /* rgba8 */, 0 /* rb_swap */,
                         640, 480, 0 /* depth_en */, 0 /* z_iova */,
                         0 /* z_stride */, 1 /* first_frame */, 0.0f,
                         1 /* mode: triangles */);
    if (len < bounds[sizeof(bounds) / sizeof(bounds[0]) - 1]) {
        slog("v3d_3d: probe_prefix CL too short (%u)\r\n", len);
        return V3D_3D_FAILED;
    }
    dma_clean(_cl_p, len);

    v3d_g2d_op_begin();
    for (uint32_t i = 0; i < sizeof(bounds) / sizeof(bounds[0]); i++) {
        uint32_t off = bounds[i];
        if (off > len) {
            break;
        }
        core[CLE_CT1CS / 4] = 0;
        __asm__ __volatile__("dsb sy" ::: "memory");
        core[CLE_CT1QBA / 4] = _cl_p;
        core[CLE_CT1QEA / 4] = _cl_p + off;

        /* 短等待：CTRUN 清零即通过 */
        int done = 0;
        for (uint32_t k = 0; k < 600000u; k++) {
            if (k > CT_DONE_STARTUP_SPIN && (core[CLE_CT1CS / 4] & 0x20u) == 0) {
                done = 1;
                break;
            }
            __asm__ __volatile__("yield");
        }
        slog("v3d_3d: prefix %-6s(%2u) %s ct1cs=0x%02x ct1ca=+%u\r\n",
             names[i], off, done ? "OK  " : "STUCK",
             core[CLE_CT1CS / 4], core[CLE_CT1CA / 4] - _cl_p);
        if (!done) {
            /* CT0/CT1 卡住后复位，后面的前缀才有意义 */
            v3d_g2d_recover("prefix stuck");
        }
    }
    v3d_g2d_op_end();
    return V3D_3D_DONE;
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

  _ct1_wedged = 0;
  {
    volatile uint32_t *c = v3d_g2d_core_regs();
    if (c != NULL) {
      _qcfg_init = c[CLE_CT1QCFG / 4];
#if V3D_3D_ENABLE_CLE_IRQ
      /* 照 Linux v3d_irq_enable()（v3d_irq.c:303-321）：
       *   先清挂起 -> 全屏蔽 -> 只解屏蔽关心的位
       * 寄存器语义：MSK 位 1 = masked，所以 MSK_SET 写 ~IRQS、MSK_CLR 写 IRQS。
       * 不解屏蔽时 FRDONE/FLDONE 的完成事件不会被硬件上报，job 永远不 retire
       * ——这是「RFC 恒 0、连续 3~4 次后不再取 job」的唯一还没验证过的解释。 */
      c[V3D_INT_CLR / 4] = CLE_IRQS;
      __asm__ __volatile__("dsb sy" ::: "memory");
      c[CLE_INT_MSK_SET / 4] = ~CLE_IRQS;
      __asm__ __volatile__("dsb sy" ::: "memory");
      c[CLE_INT_MSK_CLR / 4] = CLE_IRQS;
      __asm__ __volatile__("dsb sy" ::: "memory");
      printf("v3d_3d: irq unmask fdone|frdone -> msk_sts 0x%x\n",
             c[CLE_INT_MSK_STS / 4]);
#endif
    }
  }
  _ready = 1;
  printf("v3d_3d: ready (cl=%u pool=%u tsda=%u shader=%u) qcfg_init=0x%x\n",
         _cl_p, _pool_p, _tsda_p, _shaders_p, _qcfg_init);
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

  /* render 探针放在 _cl 的下一页，避开 empty-bin 刚取指过的 cache line；
   * 同时把该页清成 0（Halt），防止 QEA 之外残留 bin 包被误读。 */
  {
    const uint32_t probe_off = 4096u;
    uint32_t base = _cl_p + probe_off;

    memset(_cl_va + probe_off, 0, 256u);

    /* ---- A: 单字节 END_OF_RENDERING ---- */
    off = probe_off;
    off = cl_u8(off, 0x0D);                     /* END_OF_RENDERING */
    dma_clean(base, off - probe_off);
    rc = submit_render(core, base, _cl_p + off);
    printf("v3d_3d: probeA [END] rc=%d consumed=%u ct1cs=0x%x\r\n", rc,
           core[CLE_CT1CA / 4] - base, core[CLE_CT1CS / 4]);

    /* ---- B: 0x79 Common + END_OF_RENDERING ---- */
    memset(_cl_va + probe_off, 0, 256u);
    off = probe_off;
    off = cl_u8(off, 0x79);
    off = cl_u8(off, 0x00);
    off = cl_u16le(off, 640);
    off = cl_u16le(off, 480);
    off = cl_u8(off, (1u << 6) | (1u << 4));
    off = cl_u8(off, (uint8_t)((1u << 7) | (log2t << 4)));
    off = cl_u8(off, (uint8_t)((log2t >> 1) & 0x3u));
    off = cl_u8(off, 0x0D);                     /* END_OF_RENDERING */
    dma_clean(base, off - probe_off);
    rc = submit_render(core, base, _cl_p + off);
    printf("v3d_3d: probeB [Common+END] rc=%d consumed=%u ct1cs=0x%x\r\n",
           rc, core[CLE_CT1CA / 4] - base, core[CLE_CT1CS / 4]);

    /* ---- C: 同一个 CL 再发一次（验证 retire 后能立刻重发） ---- */
    dma_clean(base, off - probe_off);
    rc = submit_render(core, base, _cl_p + off);
    printf("v3d_3d: probeC [repeat] rc=%d consumed=%u ct1cs=0x%x\r\n", rc,
           core[CLE_CT1CA / 4] - base, core[CLE_CT1CS / 4]);
  }

  v3d_g2d_op_end();
  return rc;
}

/* ============ clear RCL 前缀梯度探针 ============
 * CT1 卡死后无法恢复，所以一次启动只能拿到「第一个失败点」。这里按命令
 * 边界把 clear 的 RCL 逐级加长，每级都以 END_OF_RENDERING 收尾，一次启动
 * 一路跑下去：第一级挂住就是答案，后面的级因为 _ct1_wedged 会立刻跳过，
 * 不会白等满超时，也不会把残值当成新证据。
 *
 * 级别（都是「前导配置 + 逐级追加」）：
 *   0 配置                  Common + RT_PART1 + ZS_CLEAR
 *   1 +TILE_COORDINATES
 *   2 +END_OF_LOADS
 *   3 +STORE(NONE)
 *   4 +CLEAR_RENDER_TARGETS
 *   5 +END_OF_TILE_MARKER
 *   6 +第二轮 (7C/1A/STORE/1B)
 *   7 +FLUSH_VCD_CACHE
 *   8 +tile(0,0) 的真实 STORE
 *   9 完整 CL（= build_clear_rcl，含全部 tile）
 *
 * 判读：
 *   0/1 挂 → 渲染目标配置本身不合法（stride/type/bpp/尺寸）
 *   2/3 挂 → END_OF_LOADS 的 load->render 切换 或 STORE 的 TLB 操作
 *   4/5 挂 → CLEAR_RENDER_TARGETS 或 tile 结束握手
 *   6   挂 → 在同一个 tile 上重开一轮
 *   7/8/9 挂 → 真实地址的 STORE / tile 遍历 */
int v3d_3d_probe_clear_steps(uint32_t fb_iova, uint32_t stride_bytes,
                             uint32_t fmt, uint32_t w, uint32_t h,
                             uint32_t rgba8)
{
    static const char *names[] = {
        "cfg only",
        "+0x7E init block",
        "+0x7A supertile cfg",
        "+0x7E+0x7A",
        "+0x7B list base",
        "7E+7B+7A",
        "+TILE_COORDINATES",
        "+END_OF_LOADS",
        "+STORE(NONE)",
        "+CLEAR_RENDER_TARGETS",
        "+END_OF_TILE_MARKER",
        "+iter2 7C/1A/ST/1B",
        "+FLUSH_VCD_CACHE",
        "+generic list (0x14)",
        "+supertile coords (0x17)",
        "full clear CL",
    };
    /* 前 6 级逐个试 tile 列表基础设施：bit0=0x7E bit1=0x7A bit2=0x7B。
     * 一条都不发也能过（0 级），说明 CT1 拒绝的是某一个具体包。 */
    static const uint32_t prefix_mask[] = {0u, 1u, 2u, 3u, 4u, 7u};
    volatile uint32_t *core;
    uint32_t off = 0, len = 0;
    uint32_t tile = 64u;
    uint32_t tx = (w + tile - 1u) / tile;
    uint32_t ty = (h + tile - 1u) / tile;
    uint32_t fw = 1, fh = 1;
    int rc = 0;
    int level;
    int nlevel = (int)(sizeof(names) / sizeof(names[0]));

    if (!_ready) {
        return V3D_3D_UNSUPPORTED;
    }
    core = v3d_g2d_core_regs();
    if (core == NULL) {
        return V3D_3D_FAILED;
    }

    v3d_g2d_op_begin();

    /* 对照实验：**轮转 CL 缓冲**。前 3 次 probe 全部从同一个 `_cl_p`
     * 提交、只是 QEA 递增，这跟真实驱动（Linux/mesa 每次用 ring buffer
     * 里的新位置）完全不同。
     *
     * 已知事实链：
     *   - 同一个地址连续提交：第 4~5 次就会挂（两轮上限还不一样，5 次/4 次）；
     *   - 空转 1~2 秒、清 INT_CLR、提交前写 CT1CS=0 都救不回来；
     *   - 挂住时 qba/qea 回读等于写入值、qcfg 0x0、ct1ca 停在 QBA；
     *   - render 完成后 int_sts 恒为 0x0（中断屏蔽下没有完成状态位），
     *     所以「等中断」这条路也走不通。
     *
     * 这一轮每一级都把 CL 写到 `_cl_p + (p+1)*256` 这个**不同地址**上，
     * QBA/QEA 随之都变。判读：
     *   8 次全过  → 根因就是「同一 QBA 重复提交」，修复 = CL 缓冲轮转；
     *   仍在中途挂 → 与地址无关，是真正的 job 回收问题，接着查 CT1QCFG。 */
    {
        int p;

        for (p = 0; p < 8; p++) {
            /* 槽位放在 _cl 缓冲靠后处，避开 build_clear_rcl 用的 _cl_va 头部 */
            uint32_t slot = _cl_p + 4096u + (uint32_t)p * 1024u;

            /* 【必须用完整 CL】残缺 CL（只有 Common+END，没有任何
             * supertile 工作）大概永远不会产生 FRDONE —— 硬件认为没有
             * tile 要画，job 不算"完成"，于是 CTRUN 清零但 job 不退役。
             * 用 build_clear_rcl 的完整 CL（含 0x14 子列表 + 0x17
             * supertile）才可能走中断判据。 */
            len = build_clear_rcl(fb_iova, stride_bytes, w, h, tile, fmt,
                                  rgba8, 0);
            if (len == 0 || len > 1024u) {
                slog("v3d_3d: clstep pre%d bad clear CL len %u\r\n", p, len);
                break;
            }
            memcpy(v3d_dma_cpu(slot), _cl_va, len);
            dma_clean(slot, len);
            rc = submit_render_ex(core, slot, slot + len, 0u, 0);
            slog("v3d_3d: clstep pre%d full-clear slot+%u len=%u rc=%d ct1ca +%d/%u ct1cs 0x%x spin 0x%x\r\n",
                 p, 4096u + (uint32_t)p * 1024u, len, rc,
                 (int)(core[CLE_CT1CA / 4] - slot), len,
                 core[CLE_CT1CS / 4], _last_spins);
            if (rc != 0) {
                v3d_g2d_op_end();
                return rc;
            }
        }
    }

    for (level = 0; level < nlevel; level++) {
        if (level == nlevel - 1) {
            len = build_clear_rcl(fb_iova, stride_bytes, w, h, tile, fmt,
                                  rgba8, 0);
        } else {
            uint32_t mask = (level < 6) ? prefix_mask[level] : 7u;

            off = 0;
            off = rcl_cfg_prefix(off, w, h, fmt, rgba8, 3u);
            if (mask != 0u)
                off = rcl_emit_tile_list_cfg(off, tx, ty, _pool_p, mask,
                                             &fw, &fh);
            if (level >= 6) {
                off = cl_u8(off, 0x7C);         /* TILE_COORDINATES (0,0) */
                off = cl_u8(off, 0x00);
                off = cl_u8(off, 0x00);
                off = cl_u8(off, 0x00);
            }
            if (level >= 7)
                off = cl_u8(off, 0x1A);         /* END_OF_LOADS */
            if (level >= 8)
                off = rcl_emit_store_none(off); /* STORE (NONE) */
            if (level >= 9)
                off = cl_u8(off, 0x19);         /* CLEAR_RENDER_TARGETS */
            if (level >= 10)
                off = cl_u8(off, 0x1B);         /* END_OF_TILE_MARKER */
            if (level >= 11) {                  /* 第二轮：重开同一个 tile */
                off = cl_u8(off, 0x7C);
                off = cl_u8(off, 0x00);
                off = cl_u8(off, 0x00);
                off = cl_u8(off, 0x00);
                off = cl_u8(off, 0x1A);
                off = rcl_emit_store_none(off);
                off = cl_u8(off, 0x1B);
            }
            if (level >= 12)
                off = cl_u8(off, 0x13);         /* FLUSH_VCD_CACHE */
            if (level >= 13) {                  /* generic per-tile 子列表 */
                uint8_t *gp = _gen_va;
                uint32_t g = 0;

                gp[g++] = 0x7D;                 /* TILE_COORDINATES_IMPLICIT */
                gp[g++] = 0x1A;                 /* END_OF_LOADS */
                gp[g++] = 0x38;                 /* PRIM_LIST_FORMAT */
                gp[g++] = 0x02;                 /* List Triangles */
                gp[g++] = 0x36;                 /* SET_INSTANCEID (5 字节) */
                gp[g++] = 0x00;
                gp[g++] = 0x00;
                gp[g++] = 0x00;
                gp[g++] = 0x00;
                gp[g++] = 0x15;                 /* BRANCH_TO_IMPLICIT_TILE_LIST */
                gp[g++] = 0x00;
                gen_emit_store_load(gp, &g, 0x1D, 0, fmt, 0, stride_bytes,
                                    fb_iova);
                gp[g++] = 0x19;                 /* CLEAR_RENDER_TARGETS */
                gp[g++] = 0x1B;                 /* END_OF_TILE_MARKER */
                gp[g++] = 0x12;                 /* RETURN_FROM_SUB_LIST */
                dma_clean(_gen_p, g);
                off = cl_u8(off, 0x14);         /* START_ADDRESS_OF_GENERIC_TILE_LIST */
                off = cl_u32le(off, _gen_p);
                off = cl_u32le(off, _gen_p + g);
            }
            if (level >= 14) {                  /* 逐个 supertile 驱动 */
                uint32_t sx, sy;
                for (sy = 0; sy < fh; sy++) {
                    for (sx = 0; sx < fw; sx++) {
                        off = cl_u8(off, 0x17);
                        off = cl_u8(off, (uint8_t)sx);
                        off = cl_u8(off, (uint8_t)sy);
                    }
                }
            }
            off = cl_u8(off, 0x0D);             /* END_OF_RENDERING */
            len = off;
        }
        if (len == 0 || len > V3D_3D_CL_MAX) {
            slog("v3d_3d: clstep %d %s bad len %u\r\n", level, names[level], len);
            break;
        }
        dma_clean(_cl_p, len);
        rc = submit_render(core, _cl_p, _cl_p + len);
        slog("v3d_3d: clstep %d %-22s len=%u rc=%d ct1ca +%d/%u ct1cs 0x%x\r\n",
             level, names[level], len, rc,
             (int)(core[CLE_CT1CA / 4] - _cl_p), len,
             core[CLE_CT1CS / 4]);
        if (rc != 0) {
            break;
        }
    }
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
  len = build_clear_rcl(fb_iova, stride_bytes, w, h, 16u, fmt, rgba8, 0);
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
  /* 【depth_en 必须与 z_iova 一致】原来传 1（开深度测试）但 z_iova=0
   * （没有 Z 缓冲）——硬件要跑深度测试却找不到 Z 面，会在 STORE 处挂住
   * （实测 ct1ca 停在 STORE 参数中段、CTRUN 不清）。先按纯颜色渲染走通。 */
  len = build_draw_rcl(fb_iova, stride_bytes, fmt, clear_rgba, 0, w, h,
                       0 /* depth test off（z_iova 为 0 时必须关） */,
                       0 /* 无持久 Z */, 0,
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
