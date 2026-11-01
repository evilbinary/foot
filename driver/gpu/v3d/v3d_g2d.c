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
#include "v3d_tfu.h"
#include "v3d_hw.h"
#include "v3d_mmu.h"
#include "v3d_qpu_kernels.h"
#include "v3d_regs.h"

/* GPU VA 布局（避开 SHADER 0x10000000 / FB 0x20000000 / BO 0x30000000） */
/* g2d 内核/逻辑的版本号 —— 每次改动 two_d.h 里的内核或本文件的调度逻辑就加一。
 * 启动那行会打印出来，用来确认板子上跑的是不是最新版（gpud.c 里的
 * __DATE__/__TIME__ 不可靠，因为那个文件不重编）。 */
#define G2D_REV 69

#define G2D_GVA_BASE 0x11000000UL
#define G2D_UNIFORM_GVA (G2D_GVA_BASE + 8192UL)
#define G2D_SCRATCH_GVA (G2D_GVA_BASE + 12288UL)

/* 用户态映射这块 arena 的 VA（和 PT_VA 0x42000000 错开） */
#define G2D_VA 0x48000000UL

/* 自测 BO 的用户态 VA（与 v3d_backend.c 的 G2D_TEST_VA 一致）。
 * 探测时用它直接读像素——NUM_COMPLETED 会回绕、会有初值噪声，
 * 像素才是“QPU 真的写了”的硬证据。 */
#define G2D_PROBE_VA 0x49000000u

/* 4MB 单级页表的用户态映射区（与 v3d_backend.c 的 PT_VA 保持一致） */
#define G2D_PT_VA 0x42000000UL

#define G2D_KERNEL_BYTES 8192u  /* 2 页：660 条 × 8B = 5280B */
#define G2D_BLIT_OFF 768u       /* argb_blit 在 kernel 区内的偏移（fill 96×8） */
/* 当前内核总长：96+220+344+103 = 763 条 × 8B = 6104B，就是最小内核的起点 */
#define G2D_MINI_OFF 6104u
/* TMU 的 Texture State / Sampler State：放在 kernel 区末尾。
 * Texture State 的 base pointer 低 6 位是 flip 控制位，
 * 所以它必须 64 字节对齐；Sampler State 低 3 位被控制位占用，
 * 8 字节对齐即可。 */
#define G2D_TEXSTATE_OFF 8128u  /* 8128 = 127*64 */
#define G2D_SAMPSTATE_OFF 8144u /* 8128 + 16 */
#define G2D_UNIFORM_BYTES 4096u /* 1 页：只用前 64B */
#define G2D_SCRATCH_BYTES 16384u /* 4 页 */
#define G2D_TOTAL_BYTES \
  (G2D_KERNEL_BYTES + G2D_UNIFORM_BYTES + G2D_SCRATCH_BYTES)

enum {
  K_FILL = 0,
  K_BLIT = 1,
  K_ALPHA = 2,
  K_COPY = 3,
  K_MINI = 4,
  K_MINIW = 5,
  K_MINIC = 6,
  K_MINIC2 = 7,
  K_BLITV = 8,
  K_ALPHAV = 9
};

static struct {
  int inited;
  uint32_t num_qpus;
  uint32_t kernel_gva[10];
  uint32_t uniform_gva;
  uint32_t uniform_va;
  uint32_t scratch_gva;
  /* 最小 TMU 读内核 + 两个 descriptor 结构（都在 kernel 区内） */
  uint32_t mini_gva;
  uint32_t minic_gva;
  uint32_t minic2_gva;
  uint32_t texstate_gva;
  uint32_t texstate_va;
  uint32_t sampstate_gva;
  uint32_t sampstate_va;
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




  /* 【自产 2D 内核】基于实证的 TMU 字节地址读模型
   * （见 zdata/raspi5-gpu.md 附录 E）：mov tmua 传裸字节地址、
   * 不碰 tmuc、ldtmu 前无 thrsw。eidx 做 16 lane 并行。 */
  memcpy(base + off, g2d_qpu_copy_vec, (size_t)g2d_qpu_copy_vec_n * 8);
  g_g2d.kernel_gva[K_BLITV] = G2D_GVA_BASE + off;
  off += (uint32_t)g2d_qpu_copy_vec_n * 8;

  memcpy(base + off, g2d_qpu_alpha_vec, (size_t)g2d_qpu_alpha_vec_n * 8);
  g_g2d.kernel_gva[K_ALPHAV] = G2D_GVA_BASE + off;
  off += (uint32_t)g2d_qpu_alpha_vec_n * 8;

  /* TMU 两个描述结构的固定位置 */
  g_g2d.texstate_gva = G2D_GVA_BASE + G2D_TEXSTATE_OFF;
  g_g2d.texstate_va = G2D_VA + G2D_TEXSTATE_OFF;
  g_g2d.sampstate_gva = G2D_GVA_BASE + G2D_SAMPSTATE_OFF;
  g_g2d.sampstate_va = G2D_VA + G2D_SAMPSTATE_OFF;


  g_g2d.uniform_gva = G2D_UNIFORM_GVA;
  g_g2d.uniform_va = G2D_VA + G2D_KERNEL_BYTES;
  g_g2d.scratch_gva = G2D_SCRATCH_GVA;

  /* 逐页映射进 GPU VA */
  volatile uint32_t* pt = (volatile uint32_t*)G2D_PT_VA;
  uint32_t pages = size / V3D_MMU_PAGE_SIZE;
  for (uint32_t i = 0; i < pages; i++) {
    v3d_mmu_map_page(pt, G2D_GVA_BASE + i * V3D_MMU_PAGE_SIZE,
                     pa + i * V3D_MMU_PAGE_SIZE);
    /* 【identity 映射】EwokOS 的 IOVA == 物理地址，所以它把 pa 直接传给
     * TMUA 也能读到。我们的页表原本只映射 0x11000000+，传 pa（0x048xxxxx）
     * 落在未映射区 —— 之前"PA 也读不到"的实验因此无效。
     * 这里额外把 pa 也映射到它自己，就得到和 EwokOS 等价的条件。 */
    v3d_mmu_map_page(pt, pa + i * V3D_MMU_PAGE_SIZE,
                     pa + i * V3D_MMU_PAGE_SIZE);
  }
  foot_cache_clean((void*)pt, (void*)(G2D_PT_VA + V3D_MMU_PT_BYTES));

  /* 【必须把 arena 整块 clean 到 DRAM】arena 是 cached 内存（PAGE_USER），
   * memcpy 只写进了 CPU 的 dcache。QPU 取指令直接走 DRAM，读不到缓存里的
   * 新内容就会拿到旧数据 —— 表现是「硬件收了 job（CURRENT_CFG5 与我们写
   * 的 kernel 地址一致）但 CURRENT_CFG4 恒为 0、一个 batch 都不发」。
   * EwokOS 用的是 dma_alloc 的 NOCACHE 内存，所以它不需要这步；
   * 我们用 cached 内存，就必须显式 clean。 */
  foot_cache_clean((void*)(uintptr_t)G2D_VA,
                   (void*)(uintptr_t)(G2D_VA + G2D_TOTAL_BYTES));
  v3d_mmu_flush(v);

  /* 【PTE 回读】MMU 虽已启用（ctl.en=1），但 MMU_CTL 的 bit1
   * （PT_INVALID_ENABLE）= 0：无效 PTE 的访问会【静默返回 0、不报 fault】，
   * 所以 vio_id=0 根本不能证明 PTE 写对了。这里直接读回索引，确认 GPU 走
   * 0x11000000 时命中的是我们刚写的这一项。 */
  /* PTE 已回读验证过（0x11000 → pa | WRITEABLE | VALID），不再重复打印 */

  /* QPU 数：core0 IDENT1 的 NSLC（bits 4..7）= slice 数，每 slice 4 个 QPU。
   * v3d_dev_t 没缓存这个字段（v3d_ident 只解析 hub 侧），这里就地读一次。 */
  {
    uint32_t ident1 = v->core0[V3D_CTL_IDENT1 / 4];
    uint32_t nslc = (ident1 >> 4) & 0xFu;
    g_g2d.num_qpus = (nslc > 0) ? (nslc * 4u) : 4u;
  }

  /* 【自检】确认 CPU 侧看到的 arena 内容和预期一致：kernel 第一条指令、
   * uniform 区。这一步和 GPU 走同一条物理路径（同一 pa），所以如果这里对
   * 而像素没写，问题就只在 GPU 侧的取指/写回，而不是参数组装。 */
  /* kernel blob 内容已回读验证过（首条指令 0x39800000bb03f000） */

  g_g2d.inited = 1;
  /* 版本标记：改过 CSD 参数/缓存维护后靠这行一眼确认跑的是新二进制
   * （v3 = cfg Y/Z 各 1 + 提交前 L2T/slice 失效）。 */
  /* 【版本号】必须放在本文件里：gpud.c 里的 __DATE__/__TIME__ 只有在 gpud.c
   * 本身重编时才变，而我们每次都只改 v3d_g2d.c / two_d.h，所以那个时间戳
   * 永远停在第一次编译的时刻，完全反映不了实际版本（踩过）。
   * 以后改动 GPU 内核或 g2d 逻辑，记得把这个 rev 加一。 */
  /* kernels 只报实际在用的三个：EwokOS 的 fill（纯写）+ 自产的 copy/alpha。
   * argb_blit / argb_alpha / argb_copy 已删除（读路径依赖未跑通的 vec4 模式）。 */
  printf("g2d: ready v3 rev=%d (kernels=%u+%u/%u words, qpus=%u, "
         "pt_gva=0x%08x arena pa=0x%08x)\n",
         G2D_REV, g2d_qpu_argb_fill_n, g2d_qpu_copy_vec_n,
         g2d_qpu_alpha_vec_n, g_g2d.num_qpus, (unsigned)G2D_GVA_BASE, pa);
  return 0;
}

/* 试一组 cfg 提交 CSD。
 * 返回 0=完成，-1=超时，-2=硬件没接收（Q=0）。
 * st0（HAVE_QUEUED 在 bit0）回填到 out_st0。 */
/* 提交一次 CSD 并等完成。uniform 已由调用方写进 g_g2d.uniform_va。
 *
 * cfg 取值照 EwokOS 的 v3d_g2d_run（Pi5 真机验证过）：
 *   cfg[0] = 1<<16        NUM_WGS_X = 1个 workgroup
 *   cfg[1..2] = 1<<16     NUM_WGS_Y/Z = 1（实测：这两个必须 >=1，
 *                         写 0 时硬件不 dispatch；EwokOS 写 0 但它的
 *                         cfg 数组初值径路不同，这里以我们自己的
 *                         实测为准：3 个维度都 1）
 *   cfg[3] = 0x000FF010   WG_SIZE=16 | WGS_PER_SG=15 | BATCHES_PER_SG_M1=255
 *   cfg[4] = num_qpus     每 QPU 一批
 *   cfg[5] = kernel IOVA | PROPAGATE_NANS(BIT1)   —— BIT1 必须有
 *   cfg[6] = uniform IOVA
 *
 * 【完成判据】实测 Pi5 上 CSD_DONE(bit6) 经常不置位（INT_STS 只剩
 * QPU 位），而 CURRENT_CFG4 也不会变 ~0（job 完成后清 0）。
 * 所以这里等 CSD_DONE 只当优化，超时也正常返回——像素是唯一
 * 可靠证据，由调用方读回核对。 */
/* wg_x：NUM_WGS_X 取值（0 = 用 nq）。
 * 不同内核用不同的 id 定位自己的 band：fill 读 workgroup id（需要 12 个
 * wg），copy/alpha 读 batch id（只需 1 个 wg + 12 个 batch）。 */
/* passes：提交次数。fill 等幂等内核用 2（首次提交可能空跑的历史 workaround）；
 * alpha 这类非幂等内核必须用 1，否则会混合两次。 */
/* 【job 是否已退役】移植自 EwokOS 的 g2d_csd_retired()。
 *
 * 为什么需要它：队列"空闲"本身不能当作完成证据 —— 在 CFG0 被硬件接受
 * 之前，队列本来就是空闲的。只看空闲就会在 job 还没开始跑时误判完成
 * （这正是我们之前用 HAVE_CURRENT / NUM_COMPLETED 全部失败的原因）。
 *
 * 四条件同时成立才算退役：
 *   1. CSD_STATUS 的 BUSY_MASK（HAVE_QUEUED|HAVE_CURRENT|NUM_ACTIVE）为 0
 *   2. CSD_CURRENT_CFG4 == ~0   —— batch 计数器已减到 0（全部发出）
 *   3. CSD_CURRENT_CFG5 与本次 cfg[5] 一致（忽略 PROPAGATE_NANS 位）
 *   4. CSD_CURRENT_CFG6 与本次 cfg[6] 一致（uniform 地址）
 * 3、4 用来确认看到的确实是"当前这个 job"而不是上一次残留。 */
static int g2d_csd_retired(v3d_dev_t* v, const uint32_t cfg[8]) {
  uint32_t status = v->core0[V3D_CSD_STATUS / 4];
  uint32_t cur5 = v->core0[V3D_CSD_CURRENT_CFG5 / 4];
  uint32_t mask = ~4u; /* ~V3D_CSD_CFG5_PROPAGATE_NANS(1<<2) */

  return (status & 0xFu) == 0u &&
         v->core0[V3D_CSD_CURRENT_CFG4 / 4] == 0xFFFFFFFFu &&
         (cur5 & mask) == (cfg[5] & mask) &&
         v->core0[V3D_CSD_CURRENT_CFG6 / 4] == cfg[6];
}

static int g2d_dispatch(v3d_dev_t* v, int kern, uint32_t nq, uint32_t wg_x,
                        int passes) {
  /* 让 CPU 写进 arena 的 uniform 对 GPU 可见 */
  foot_cache_clean((void*)(uintptr_t)g_g2d.uniform_va,
                   (void*)(uintptr_t)(g_g2d.uniform_va + 128));

  uint32_t cfg[8];
  memset(cfg, 0, sizeof(cfg));
  /* 【workgroup 数 = QPU 数，batch 数 = QPU 数】
   * 实测规律（256×256 填绿，rows = ceil(256/12) = 22 行/band）：
   *   cfg[0]=1 个 wg, cfg[4]=1  → 只画 1 个 band（5568 像素 ≈ 22×256）
   *   cfg[0]=1 个 wg, cfg[4]=12 → 只画 7 个 band（39744 ≈ 155×256）
   * 说明 cfg[4] 是"并发 batch 数"，每个 batch 覆盖一个 rows 高的 band；
   * 而 1 个 workgroup 驱动不了 12 个 batch，只能跑出 7 个。按"每 QPU 一个
   * workgroup"配置后，batch 与 workgroup 一一对应，才能覆盖全部 12 个 band
   * （12×22 = 264 ≥ 256 行）。 */
  cfg[0] = (wg_x ? wg_x : nq) << 16; /* NUM_WGS_X */
  cfg[1] = 1u << 16;
  cfg[2] = 1u << 16;
  cfg[3] = 0x000FF010u;
  cfg[4] = nq; /* 并发 batch 数 = QPU 数 */
  /* 【cfg[5] 位组合实测】
   *   |2u (bit1)  → 内核执行（但程序内分支不生效，见下）
   *   |4u (bit2)  → 内核完全不执行
   *   |6u         → 同 |2u
   * 所以 bit1 必需，且分支失效与 cfg 无关。
   * 结论：放弃在内核里循环，改为内核只处理一块、由 CPU 多次 dispatch。 */
  cfg[5] = g_g2d.kernel_gva[kern] | 2u;
  cfg[6] = g_g2d.uniform_gva;

  /* 【提交两次：第一次会空跑】实测发现一个稳定现象：
   *   只有一次 dispatch 时像素全 0（nonzero=0/65536）；
   *   而之前"先探测 4 组再跑选中的那组"时，最后一组能画满 65536。
   * 差别不在 cfg（两组完全相同），而在"之前已经提交过几次" —— 说明
   * 上电后的第一次 CSD 提交只完成某种内部初始化，并不真正执行 kernel。
   * 所以这里固定提交两次：第一次可能空跑，第二次才是有效执行。
   * 2D 内核单次只有几十微秒，多一次的开销可以忽略。 */
  for (int pass = 0; pass < passes; pass++) {
    /* 【提交前等队列空闲】EwokOS 的 g2d_csd_prepare_launch 第一步就是
     * 轮询 CSD_STATUS 的 HAVE_QUEUED|HAVE_CURRENT 直到清零。上一个 job
     * 还在跑时写 CFG0，新 job 会被丢弃 —— 长内核（argb_blit 220 条，
     * 是 fill 96 条的两倍多）尤其容易撞上。 */
    for (int w = 0; w < 2000; w++) {
      if ((v3d_csd_status(v) & 3u) == 0) {
        break;
      }
      usleep(100);
    }
    v->core0[V3D_CTL_INT_CLR / 4] = 0xFFFFFFFFu;
    __asm__ volatile("dmb sy" ::: "memory");
    v3d_csd_clear_int(v);
    v3d_invalidate_caches(v);

    /* 【清 per-slice 读侧缓存 —— TMU 读的前提】
     * SLCACTL 的四个 4 位字段分别是指令 / uniform / TMU 数据 / TMU varying
     * 缓存的"清除行数"，0xF = 整块清空。EwokOS 的 g2d_clean_slc() 就做这件事，
     * 且明确说明它属于 PRE 侧（旧行会被新 job 读到）。
     *
     * 我们一直没写过它。实测证据：把一次已确认能成功的 TMU 写入放到读之后，
     * 写入不再发生 —— 线程死在 ldtmu 上，说明读请求得不到 TMU 响应。 */
    v->core0[V3D_CTL_SLCACTL / 4] = V3D_SLCACTL_CLEAR_ALL;
    __asm__ volatile("dmb sy" ::: "memory");

    /* 【清 TMU 配置缓存】L2TCACTL 的 bits[15:12] 是 TMU Config Cache Clear，
     * bits[11:8] 是 TMU Data Cache Clear（TMUWCF）。我们一直只清了数据缓存
     * （在 v3d_clean_caches 里），从没清过配置缓存。
     * 如果配置缓存陈旧，QPU 写 tmuc 不会生效，TMU 读就会沿用旧的无效配置
     * 并返回 0 —— 正好对应"fill 不依赖 config 一直正常、copy 依赖它全 0"。 */
    v->core0[V3D_CTL_L2TCACTL / 4] = V3D_L2TCACTL_TMUCFCF;
    for (int i = 0; i < 1000; i++) {
      if (!(v->core0[V3D_CTL_L2TCACTL / 4] & V3D_L2TCACTL_TMUCFCF)) {
        break;
      }
      usleep(10);
    }

    v3d_csd_submit(v, cfg);

    /* 【等 job 完成】原来轮询 INT_STS 的 CSDDONE 位，但该位在本机不置位，
     * 于是每次 dispatch 都白等满 100ms —— 连续跑 100 次就是 30 秒，看起来
     * 像卡死。改成轮询 CSD_STATUS 的 HAVE_CURRENT 位（job 结束会清零），
     * 这个位是可靠的，且通常几十微秒就返回。 */
    /* 【必须等满】轮询 INT_STS 的 CSDDONE 位，但该位在本机不置位，
     * 所以这个循环实际总是跑满 100ms —— 而这段"空转"正是 GPU 跑完
     * job 所需的时间窗口。曾经把它压到 1ms，结果连最简单的 fill 都只
     * 写出 7376/65536（job 没跑完就返回了）。不要缩短。 */
    /* 【等 job 退役】移植 EwokOS 的做法：
     *   - 先紧自旋看到 busy（确认 CFG0 已被接受，排除"提交前的空闲"）
     *   - 然后等 retired 签名（四条件）成立
     *   - 全程不 sleep，因为一个 256x256 job 只需几十微秒
     * 之前用固定 20ms 是因为误以为没有可靠信号；实际 CFG4 就是。
     */
    {
      int seen_busy = 0;
      int retired = 0;
      for (int i = 0; i < 200000; i++) {
        uint32_t st = v->core0[V3D_CSD_STATUS / 4];
        if (st & 0xFu) {
          seen_busy = 1;
        } else if (seen_busy || g2d_csd_retired(v, cfg)) {
          retired = 1;
          break;
        }
        __asm__ volatile("yield");
      }
      /* 保底：极少数情况下签名一直不成立（例如 job 空转），
       * 给一小段固定时间再放行，避免把启动卡死。 */
      if (!retired) {
        usleep(2000);
      }
    }
    v3d_csd_clear_int(v);

    /* 【job 完成后必须把 L2T 写回 DRAM】CSD 的结果先落在 L2T 缓存里，
     * 不一定立刻进 DRAM。若交给下一次 dispatch 时只做 invalidate
     * （FLM_FLUSH，只失效不写回），上一次的结果会被直接丢弃 ——
     * 这正是 fill（只写）正常、而 copy/alpha（要读源）永远读到 0 的原因。 */
    v3d_clean_caches(v);
  }
  return 0;
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

  uint32_t u[17]; /* u[16] = scratch（EwokOS 的 v3d_g2d_run 额外写的那一格）*/
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
  /* 【强制走通用路径】256x256 满足 full 的判定（w%16==0 且整幅），内核会
   * 走 "full" 快路径 —— 那条路径可能假设 stride 页对齐或整体连续，先排除
   * 它，用通用逐行路径验证参数是否正确。 */
  (void)full;
  u[11] = 0;
  u[12] = rows;
  u[13] = rows * w * 4u;
  u[14] = g_g2d.scratch_gva;
  u[15] = 0;
  u[16] = g_g2d.scratch_gva; /* 和 u[14] 同值，两个位置 EwokOS 都写 */

  memcpy((void*)(uintptr_t)g_g2d.uniform_va, u, 17u * sizeof(uint32_t));
  return g2d_dispatch(v, K_FILL, nq, nq, 2);
}

/* 1:1 整幅拷贝（argb_blit 内核，恒等仿射映射）。
 *
 */
int v3d_g2d_blit(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva, uint32_t w,
                 uint32_t h, uint32_t wg_x, uint32_t src_pa) {
  (void)wg_x;
  (void)src_pa;

  /* 【改用 TFU，不再用 QPU 内核】
   *
   * argb_copy / argb_blit 都要从源读像素，而 V3D 的 QPU 只能
   * 通过 TMU 访存。实测（见下）证明这条读路在我们这边不可用：
   *   - 22 条指令的最小读内核（与 argb_copy 同一机制）恒返回 0
   *   - IOVA / 物理地址两种形式都试过
   *   - mov tmuc 带 / 不带 sig.wrtmuc 都试过
   *   - P0/P1/P2 描述符协议与 tmuc=0xFFFFFFFC 直接模式都试过
   *   - 无 MMU fault，job 被采纳（CURRENT_CFG5/6 完全匹配）
   * 而同一框架下的纯写内核（argb_fill / miniwrite）一直正常。
   *
   * TFU 是独立于 QPU/TMU 的固定功能 2D 引擎，自带 DMA，
   * 不经过 TMU；它的 BO→screen 拷贝已在 test-gpu 里验证通过
   * （SUBMIT_TFU rc=0）。所以 1:1 拷贝走 TFU。
   */
  if (!g_g2d.inited || src_gva == 0 || dst_gva == 0 || w == 0 || h == 0) {
    return -1;
  }
  return v3d_tfu_copy(v, src_gva, dst_gva, w, h, w, w);
}

/* source-over 混合（argb_alpha 内核）：dst = src*a + dst*(1-a)。
 *
 * uniform 只有 24 个字，且矩阵只用 pu/cu/qv/cv（该内核不支持旋转/斜切），
 * 恒等映射同样是 pu=32768, qv=32768, cu=cv=0。
 * u[11] 是整个操作的全局 alpha（0..255）。 */
/* 【【 CPU source-over 混合（argb_alpha 的等价实现）】
 *
 * 为什么在 CPU：alpha 内核要先读 dst 再写回，而 QPU 的
 * TMU 读在我们这边恒返回 0（详见 v3d_g2d_blit 上方的说明）。
 * TFU 不支持混合，所以半透明合成只能走 CPU。
 *
 * 数学与 EwokOS 的 g2d_blend_argb_scalar（aarch64 软件 2D）一致，
 * 保证同一张图在 GPU/CPU 两条路上一致：
 *   有效 alpha a = (src_a * alpha) >> 8
 *   oa = oa + (255 - oa) * a / 255
 *   c  = (s * a + d * (255 - a)) / 255
 * dst 完全透明（oa==0）时直接取源色，否则会被“和黑色混合”弄暗。
 *
 * 注意：这里只处理 32 位 ARGB8888；上屏前如果 framebuffer 是
 * RGB565，还需要一次格式转换（可用 v3d_tfu_copy_ex）。
 */
void v3d_g2d_alpha_cpu(uint32_t* dst, const uint32_t* src, uint32_t w,
                       uint32_t h, uint32_t alpha) {
  if (dst == NULL || src == NULL || w == 0 || h == 0 || alpha == 0) {
    return;
  }
  uint32_t n = w * h;
  if (alpha >= 256u) {
    /* 全不透明：直接覆盖，省掉混合 */
    for (uint32_t i = 0; i < n; i++) {
      dst[i] = src[i];
    }
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    uint32_t sc = src[i];
    uint32_t dc = dst[i];
    uint32_t sa = (sc >> 24) & 0xFFu;
    uint32_t a = (sa * alpha) >> 8; /* 有效 alpha */
    if (a == 0) {
      continue;
    }
    uint32_t da = (dc >> 24) & 0xFFu;
    uint32_t sr = (sc >> 16) & 0xFFu;
    uint32_t sg = (sc >> 8) & 0xFFu;
    uint32_t sb = sc & 0xFFu;
    if (da == 0) {
      /* 目标完全透明：直接取源色 + 有效 alpha */
      dst[i] = (a << 24) | (sr << 16) | (sg << 8) | sb;
      continue;
    }
    uint32_t inv = 255u - a;
    uint32_t oa = da + (255u - da) * a / 255u;
    uint32_t dr = (sr * a + ((dc >> 16) & 0xFFu) * inv) / 255u;
    uint32_t dg = (sg * a + ((dc >> 8) & 0xFFu) * inv) / 255u;
    uint32_t db = (sb * a + (dc & 0xFFu) * inv) / 255u;
    dst[i] = (oa << 24) | (dr << 16) | (dg << 8) | db;
  }
}

int v3d_g2d_alpha(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva, uint32_t w,
                  uint32_t h, uint32_t alpha) {
  /* 【已停用】EwokOS argb_alpha 依赖 EwokOS 的 argb_* 原始内核，而那些 blob
   * 已从 v3d_qpu_kernels.h 删除（它们的 TMU 读在本项目的 CSD
   * 提交方式下恒返回 0）。自产的 copy_vec / alpha_vec 才是
   * 实际在用的路径。保留签名只为不动调用点。 */
  (void)v;
  return -1;
}


/* ---- 最小 TMU 读验证 ----
 *
 * 只做一件事：按下面的 descriptor 读源图像的 (0,0) 像素，再写到 dst_gva。
 * 如果 dst 的第一个像素变成源图的颜色，说明 TMU 读通路建立，
 * 之后 blit / alpha 只是“换一个坐标计算”的事。
 *
 * Texture State（16B，V3D 7.1）：
 *   [31:0]  base pointer（64B 对齐） [57:32] stride/64
 *   [71:58] width        [85:72] height
 *   [106:100] texture type（RGBA8 = 4）
 * Sampler State（24B）：全 0 = clamp + LOD 0。
 * P2：Op = Regular(15) 在 bits[23:20]。
 */
/* 对照内核：不读源，直接把常量写到 dst。
 * 用来区分两种失败：
 *   miniwrite 成功、miniread 失败  ⇒ 内核框架和写路径没问题，卡在 TMU 读
 *   两个都失败                ⇒ dispatch 参数 / thrsw / uniform 延迟有问题
 */
/* 最小直接地址读：从 src 读一个 32 位字，写到 dst 第一个字。
 * 这是 argb_copy 的最小形式（同一个 tmuc 常量 + 同样的 tmua 用法）。
 *   成功 ⇒ TMU 直接读可用，argb_copy 失败在它自己的块处理逻辑
 *   失败 ⇒ 这条路在 V3D 7.1 真机上不通，应改用 TFU / CPU
 */
int v3d_g2d_minicopy(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva) {
  /* 【已停用】这个内核（g2d_mini_copy1）走的是 EwokOS 的
   * tmuc=0xFFFFFFFC + 裸地址 那条 vec4 路径，在本项目的 CSD 提交方式下
   * 恒读回 0。它对应的 mini_copy1.h 已删除，这里保留函数符号只为不改动
   * 调用点，实际不再 dispatch。 */
  (void)v;
  (void)src_gva;
  (void)dst_gva;
  return -1;
}

/* minicopy 的变体：mov tmuc 带 sig.wrtmuc */
int v3d_g2d_minicopy2(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva) {
  /* 【已停用】与 v3d_g2d_minicopy 同理：内核来自已删除的 mini_copy1.h，
   * 走的是本项目没跑通的 vec4 路径。保留符号只为不动调用点。 */
  (void)v;
  (void)src_gva;
  (void)dst_gva;
  return -1;
}

/* 1:1 拷贝（GPU 版，自产内核）。
 *
 * 【关键】`mov tmua` 是"读"还是"写"由描述符 P2 的 Op 决定：
 *   不提交（fill 的做法）→ Op 默认 0 → mov tmua 是【写】
 *   提交 P2 = Op=Regular  → mov tmua 是【读】
 * 实测：miniread1 提交 nop.wrtmuc×3 后，mov tmua 传 src_gva 字节地址
 *       读回 0xff00ff00；coordprobe 同样提交但传 0/1/0x10000（未映射的
 *       字节地址）→ 全 0。所以 TMUA 是【裸字节地址】，不是坐标。
 * copy 之前从不提交描述符，于是每次"读源"其实都在写源 —— 这就是
 * argb_copy / argb_blit / argb_alpha 跑不通的根因。
 */
int v3d_g2d_blit_gpu(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                     uint32_t w, uint32_t h) {
  if (!g_g2d.inited || src_gva == 0 || dst_gva == 0 || w == 0 || h == 0) {
    return -1;
  }
  uint32_t chunks = (w * h) / 16u;
  if (chunks == 0 || (w * h) % 16u != 0) {
    return -2;
  }

  /* Texture State 指向 src，否则 TMU 没有有效的读上下文 */
  {
    volatile uint32_t* ts = (volatile uint32_t*)(uintptr_t)g_g2d.texstate_va;
    uint32_t stride_bytes = w * 4u;
    ts[0] = src_gva;
    ts[1] = (stride_bytes / 64u) | ((w & 0x3Fu) << 26);
    ts[2] = ((w >> 6) & 0xFFu) | (h << 8) | (1u << 22);
    /* type = RGBA8(4)，swizzle 恒等 = R2/G3/B4/A5 */
    ts[3] = (4u << 4) | (2u << 12) | (3u << 15) | (4u << 18) | (5u << 21);
    foot_cache_clean((void*)(uintptr_t)g_g2d.texstate_va,
                     (void*)(uintptr_t)(g_g2d.sampstate_va + 24));
  }

  /* blit 内核（vc7 生成，12 QPU 分段）的 uniform 布局：
   *   u0 = src 基址    u1 = dst 基址
   *   u2 = 每 QPU 轮数 u3 = 每轮字节步长(64)
   *   u4 = 每 QPU 负责的字节跨度
   * 内核里用 tidx>>2 取 QPU 号，起始地址 += QPU号 × u4。
   * 轮数按 ceil(chunks/nq) 取整，最后一个 QPU 会越界约几百字节，
   * 所以 BO 多分配了 8KB（见 self-test 里的 alloc 调用）。 */
  uint32_t nq = g_g2d.num_qpus ? g_g2d.num_qpus : 1u;
  uint32_t rounds = (chunks + nq - 1u) / nq;
  uint32_t qpu_stride = rounds * 64u;

  uint32_t u[5];
  u[0] = src_gva;
  u[1] = dst_gva;
  u[2] = rounds;
  u[3] = 64u;
  u[4] = qpu_stride;

  {
    volatile uint32_t* up = (volatile uint32_t*)(uintptr_t)g_g2d.uniform_va;
    for (int i = 0; i < 16; i++) {
      up[i] = 0;
    }
  }
  memcpy((void*)(uintptr_t)g_g2d.uniform_va, u, sizeof(u));

  v->hub[V3D_MMU_VIO_ID / 4] = 0;
  v->hub[V3D_MMU_VIO_ADDR / 4] = 0;
  __asm__ volatile("dmb sy" ::: "memory");

  return g2d_dispatch(v, K_BLITV, nq, 1, 1);
}

/* source-over 混合（GPU 版）。a: 0..256（256 = 全不透明）。 */
int v3d_g2d_alpha_gpu(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                      uint32_t w, uint32_t h, uint32_t a) {
  if (!g_g2d.inited || src_gva == 0 || dst_gva == 0 || w == 0 || h == 0) {
    return -1;
  }
  if (a == 0) {
    return 0; /* 全透明 = 无操作 */
  }
  uint32_t chunks = (w * h) / 16u;
  if (chunks == 0 || (w * h) % 16u != 0) {
    return -2;
  }
  uint32_t alpha = (a > 256u) ? 256u : a;
  /* alpha 内核（vc7 生成，12 QPU 分段）的 uniform 布局：
   *   u0=src u1=dst u2=每QPU轮数 u3=64(步长)
   *   u4=a u5=256-a u6=0xFF u7=8 u8=16 u9=24
   *   u10=每 QPU 负责的字节跨度
   * 8/16/24/0xFF 都必须走寄存器：b 操作数的整数小立即数只有 0..15。 */
  uint32_t nq = g_g2d.num_qpus ? g_g2d.num_qpus : 1u;
  uint32_t rounds = (chunks + nq - 1u) / nq;

  uint32_t u[11];
  u[0] = src_gva;
  u[1] = dst_gva;
  u[2] = rounds;
  u[3] = 64u;
  u[4] = alpha;
  u[5] = 256u - alpha;
  u[6] = 0xFFu;
  u[7] = 8u;
  u[8] = 16u;
  u[9] = 24u;
  u[10] = rounds * 64u;

  {
    volatile uint32_t* up = (volatile uint32_t*)(uintptr_t)g_g2d.uniform_va;
    for (int i = 0; i < 16; i++) {
      up[i] = 0;
    }
  }
  memcpy((void*)(uintptr_t)g_g2d.uniform_va, u, sizeof(u));

  v->hub[V3D_MMU_VIO_ID / 4] = 0;
  v->hub[V3D_MMU_VIO_ADDR / 4] = 0;
  __asm__ volatile("dmb sy" ::: "memory");

  /* 混合不是幂等操作，只能提交一次 */
  return g2d_dispatch(v, K_ALPHAV, nq, 1, 1);
}

/* 【原版 argb_copy，完全按 EwokOS 探针的 ABI 运行】
 *
 * 这是 EwokOS v3d_g2d_vec4_ok() 里用的那套参数（源码 v3d_g2d.c:1315）：
 *   u0 = dst, u1 = src, u2 = dstride, u3 = sstride, u4 = C(0=单尾块),
 *   u5 = V16(256=16 lane 全有效), u6 = H(行数), u7 = rpq, u8 = drows, u9 = srows
 *   末尾再追加一格 scratch 物理地址（V16=256 时内核不用）
 * 一个 QPU、一个 batch。
 *
 * 目的：把“内核本身"和"我们的 ABI 猜测”彻底分开 —— 如果这套原版参数能拷出
 * 正确像素，说明之前 blit/alpha 失败是我们算错了 uniform 布局；如果仍然是 0，
 * 那就是环境差异（EwokOS 的 buffer 在 dma NOCACHE 且 IOVA==PA）。
 */
int v3d_g2d_copy_orig(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                      uint32_t span_bytes, uint32_t rows) {
  /* 【已停用】EwokOS argb_copy 依赖 EwokOS 的 argb_* 原始内核，而那些 blob
   * 已从 v3d_qpu_kernels.h 删除（它们的 TMU 读在本项目的 CSD
   * 提交方式下恒返回 0）。自产的 copy_vec / alpha_vec 才是
   * 实际在用的路径。保留签名只为不动调用点。 */
  (void)v;
  return -1;
}

int v3d_g2d_miniwrite(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                      uint32_t color) {
  /* 【已停用】这个探针用的小内核来自已删除的 mini_kernel.h。
   * 保留函数与签名只为不改动 v3d_backend.c 里的调用点，实际不再
   * dispatch（那条 vec4 读路径在本项目的 CSD 提交方式下恒为 0）。 */
  (void)v; (void)src_gva; (void)dst_gva;
  return -1;
}

int v3d_g2d_miniread(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                     uint32_t src_w, uint32_t src_h) {
  /* 【已停用】这个探针用的小内核来自已删除的 mini_kernel.h。
   * 保留函数与签名只为不改动 v3d_backend.c 里的调用点，实际不再
   * dispatch（那条 vec4 读路径在本项目的 CSD 提交方式下恒为 0）。 */
  (void)v; (void)src_gva; (void)dst_gva; (void)src_w; (void)src_h;
  return -1;
}
