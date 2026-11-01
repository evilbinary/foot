/*******************************************************************
 * V3D CSD 2D 操作层实现（fill / blit / alpha）。
 *
 * 【内存布局】一个 arena 里分三段，逐页映射到 GPU VA：
 *   pa + 0       → GPU 0x11000000  内核 blob（fill/blit/alpha 连续）
 *   pa + 8192    → GPU 0x11002000  uniform（16 × u32 = 64B，占 1 页）
 *   pa + 12288   → GPU 0x11003000  scratch（16KB，QPU 的写汇）
 *
 * 【提交】cfg 参数照 EwokOS：
 *   cfg[0] = 1 << 16       NUM_WGS_X = 1
 *   cfg[3] = 0x000FF010    WG_SIZE=16 | WGS_PER_SG=2
 *   cfg[4] = num_qpus      batch 数（2D 内核 = 每 QPU 一批）
 *   cfg[5] = kernel IOVA
 *   cfg[6] = uniform IOVA
 ********************************************************************/
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "v3d_csd.h"
#include "v3d_g2d.h"
#include "v3d_hw.h"
#include "v3d_mmu.h"
#include "v3d_qpu_kernels.h"
#include "v3d_regs.h"

/* GPU VA 布局（避开 SHADER 0x10000000 / FB 0x20000000 / BO 0x30000000） */
#define G2D_GVA_BASE 0x11000000UL
#define G2D_UNIFORM_GVA (G2D_GVA_BASE + 8192UL)
#define G2D_SCRATCH_GVA (G2D_GVA_BASE + 12288UL)

/* 用户态映射这块 arena 的 VA（和 PT_VA 0x42000000 错开） */
#define G2D_VA 0x48000000UL

/* 4MB 单级页表的用户态映射区（与 v3d_backend.c 的 PT_VA 保持一致） */
#define G2D_PT_VA 0x42000000UL

#define G2D_KERNEL_BYTES 8192u  /* 2 页：660 条 × 8B = 5280B */
#define G2D_UNIFORM_BYTES 4096u /* 1 页：只用前 64B */
#define G2D_SCRATCH_BYTES 16384u /* 4 页 */
#define G2D_TOTAL_BYTES \
  (G2D_KERNEL_BYTES + G2D_UNIFORM_BYTES + G2D_SCRATCH_BYTES)

enum { K_FILL = 0, K_BLIT = 1, K_ALPHA = 2 };

static struct {
  int inited;
  uint32_t num_qpus;
  uint32_t kernel_gva[3];
  uint32_t uniform_gva;
  uint32_t uniform_va;
  uint32_t scratch_gva;
} g_g2d;

int v3d_g2d_num_qpus(void) { return (int)g_g2d.num_qpus; }

int v3d_g2d_init(v3d_dev_t* v) {
  if (v == NULL || v->hub == NULL || v->core0 == NULL) {
    return -1;
  }
  if (g_g2d.inited) {
    return 0;
  }

  uint32_t pa = 0, size = 0;
  foot_cap_t cap = foot_cap_alloc_arena(G2D_TOTAL_BYTES, 0, 0, &pa, &size);
  if (cap == FOOT_CAP_NONE || size < G2D_TOTAL_BYTES) {
    printf("g2d: alloc arena failed\n");
    return -1;
  }
  if (foot_cap_map(cap, (void*)G2D_VA, 0, size) != 0) {
    printf("g2d: map arena failed\n");
    return -1;
  }

  uint8_t* base = (uint8_t*)G2D_VA;
  memset(base, 0, G2D_TOTAL_BYTES);

  /* 三个内核连续摆放，记录各自的 GPU VA */
  uint32_t off = 0;
  memcpy(base + off, g2d_qpu_argb_fill, (size_t)g2d_qpu_argb_fill_n * 8);
  g_g2d.kernel_gva[K_FILL] = G2D_GVA_BASE + off;
  off += (uint32_t)g2d_qpu_argb_fill_n * 8;

  memcpy(base + off, g2d_qpu_argb_blit, (size_t)g2d_qpu_argb_blit_n * 8);
  g_g2d.kernel_gva[K_BLIT] = G2D_GVA_BASE + off;
  off += (uint32_t)g2d_qpu_argb_blit_n * 8;

  memcpy(base + off, g2d_qpu_argb_alpha, (size_t)g2d_qpu_argb_alpha_n * 8);
  g_g2d.kernel_gva[K_ALPHA] = G2D_GVA_BASE + off;
  off += (uint32_t)g2d_qpu_argb_alpha_n * 8;

  g_g2d.uniform_gva = G2D_UNIFORM_GVA;
  g_g2d.uniform_va = G2D_VA + G2D_KERNEL_BYTES;
  g_g2d.scratch_gva = G2D_SCRATCH_GVA;

  /* 逐页映射进 GPU VA */
  volatile uint32_t* pt = (volatile uint32_t*)G2D_PT_VA;
  uint32_t pages = size / V3D_MMU_PAGE_SIZE;
  for (uint32_t i = 0; i < pages; i++) {
    v3d_mmu_map_page(pt, G2D_GVA_BASE + i * V3D_MMU_PAGE_SIZE,
                     pa + i * V3D_MMU_PAGE_SIZE);
  }
  foot_cache_clean((void*)pt, (void*)(G2D_PT_VA + V3D_MMU_PT_BYTES));
  v3d_mmu_flush(v);

  /* QPU 数：core0 IDENT1 的 NSLC（bits 4..7）= slice 数，每 slice 4 个 QPU。
   * v3d_dev_t 没缓存这个字段（v3d_ident 只解析 hub 侧），这里就地读一次。 */
  {
    uint32_t ident1 = v->core0[V3D_CTL_IDENT1 / 4];
    uint32_t nslc = (ident1 >> 4) & 0xFu;
    g_g2d.num_qpus = (nslc > 0) ? (nslc * 4u) : 4u;
  }
  g_g2d.inited = 1;
  printf("g2d: ready (kernels=%u/%u/%u words, qpus=%u, arena pa=0x%08x)\n",
         g2d_qpu_argb_fill_n, g2d_qpu_argb_blit_n, g2d_qpu_argb_alpha_n,
         g_g2d.num_qpus, pa);
  return 0;
}

/* 提交一次 CSD 并等完成。uniform 已由调用方写进 g_g2d.uniform_va。 */
static int g2d_dispatch(v3d_dev_t* v, int kern, uint32_t nq) {
  /* 让 CPU 写进 arena 的 uniform 对 GPU 可见 */
  foot_cache_clean((void*)(uintptr_t)g_g2d.uniform_va,
                   (void*)(uintptr_t)(g_g2d.uniform_va + 64));

  uint32_t cfg[8];
  memset(cfg, 0, sizeof(cfg));
  /* 【三个维度都必须 >= 1】V3D 对 0 的解释是"该维度没有 workgroup"，
   * 总 workgroup 数 = X*Y*Z，任一为 0 就等于 0 个 → 硬件直接不 dispatch
   * （status 的 NUM_COMPLETED 不动、INT_STS 的 CSD_DONE 不置位，看起来像
   * "跑完但没写像素"）。实测漏掉 Y/Z 就是这个症状。 */
  cfg[0] = 1u << 16; /* NUM_WGS_X = 1 */
  cfg[1] = 1u << 16; /* NUM_WGS_Y = 1 */
  cfg[2] = 1u << 16; /* NUM_WGS_Z = 1 */
  cfg[3] = 0x000FF010u; /* WG_SIZE=16 | WGS_PER_SG=15 | BATCHES_PER_SG_M1=255 */
  cfg[4] = nq;          /* batch 数（2D 内核 = 每 QPU 一批） */
  cfg[5] = g_g2d.kernel_gva[kern];
  cfg[6] = g_g2d.uniform_gva;

  /* 【必须先清旧的 DONE 位】下面的等待判据是 INT_STS 的 CSD_DONE，
   * 上一次 job 的 latch 不清掉会立刻误判"已完成"。 */
  v3d_csd_clear_int(v);
  v3d_csd_submit(v, cfg);
  /* 【为什么不用 v3d_csd_wait_idle】它查 CSD_STATUS 的 HAVE_QUEUED|
   * HAVE_CURRENT 是否为 0 —— 但 job 刚写 CFG0 时这两位还没置起来，
   * 函数立即返回 0，CPU 马上读像素（还没写）⇒ 全 0。实测就是这样。
   * 改成等 INT_STS 的 CSD_DONE（EwokOS 同样用这个判据）。 */
  uint32_t st0 = v3d_csd_status(v);
  int done = 0;
  for (int i = 0; i < 3000; i++) {
    if (v->core0[V3D_CTL_INT_STS / 4] & V3D_INT_CSDDONE) {
      done = 1;
      break;
    }
    usleep(1000);
  }
  int rc = done ? 0 : -1;
  uint32_t st1 = v3d_csd_status(v);
  uint32_t intsts = v->core0[V3D_CTL_INT_STS / 4];
  uint32_t vio_id = v->hub[V3D_MMU_VIO_ID / 4];
  uint32_t vio_addr = v->hub[V3D_MMU_VIO_ADDR / 4];
  {
    static int dbg = 0;
    if (dbg++ < 3) {
      printf("g2d: kern=%d nq=%u cfg3=0x%08x cfg4=%u cfg5=0x%08x cfg6=0x%08x\n",
             kern, nq, cfg[3], cfg[4], cfg[5], cfg[6]);
      printf("g2d:   st0=0x%08x st1=0x%08x int=0x%08x vio_id=0x%08x "
             "vio_addr=0x%08x rc=%d\n",
             st0, st1, intsts, vio_id, vio_addr, rc);
    }
  }
  v3d_csd_clear_int(v);
  return rc;
}

int v3d_g2d_fill(v3d_dev_t* v, uint32_t dst_gva, uint32_t w, uint32_t h,
                 uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                 uint32_t color) {
  if (!g_g2d.inited || dst_gva == 0 || w == 0 || h == 0) {
    return -1;
  }
  if (x0 >= x1 || y0 >= y1 || x1 > w || y1 > h) {
    return -2;
  }

  uint32_t g0 = x0 >> 4;
  uint32_t g1 = (x1 + 15u) >> 4;
  uint32_t L = g1 - g0;
  uint32_t band_h = y1 - y0;
  uint32_t nq = g_g2d.num_qpus;
  if (nq > band_h) {
    nq = band_h;
  }
  uint32_t rows = (band_h + nq - 1) / nq;
  uint32_t full =
      ((w & 15u) == 0 && x0 == 0 && y0 == 0 && x1 == w && y1 == h) ? 1u : 0u;

  uint32_t u[16];
  u[0] = color;
  u[1] = L - 1;
  u[2] = w * 4u;
  u[3] = y1 - 1;
  u[4] = L * 64u - w * 4u;
  u[5] = L * rows;
  u[6] = dst_gva + (y0 * w + g0 * 16u) * 4u;
  u[7] = x0;
  u[8] = x1;
  u[9] = y0;
  u[10] = y1;
  u[11] = full;
  u[12] = rows;
  u[13] = rows * w * 4u;
  u[14] = g_g2d.scratch_gva;
  u[15] = 0;

  memcpy((void*)(uintptr_t)g_g2d.uniform_va, u, sizeof(u));
  return g2d_dispatch(v, K_FILL, nq);
}

int v3d_g2d_blit(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva, uint32_t w,
                 uint32_t h) {
  /* 【待实现】argb_blit 的 uniform 布局还没从 EwokOS 的 gpu_blit_op 里
   * 逆向出来。fill 跑通后再补。 */
  (void)v;
  (void)src_gva;
  (void)dst_gva;
  (void)w;
  (void)h;
  return -1;
}

int v3d_g2d_alpha(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva, uint32_t w,
                  uint32_t h, uint32_t alpha) {
  (void)v;
  (void)src_gva;
  (void)dst_gva;
  (void)w;
  (void)h;
  (void)alpha;
  return -1;
}
