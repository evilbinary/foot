/*******************************************************************
 * v3dd：raspi5 V3D 7.1（VideoCore VII）GPU 驱动 · foot 服务端。
 *
 * 用 foot 微内核方式跑 GPU 驱动：本进程不是内核模块，而是用户态守护进程
 * （仿 EwokOS 的 system/gui/drivers/g2dd）。
 *
 *   - 通过 capability（/dev/foot）申请 V3D / PM 设备窗口，映射进自己地址空间；
 *   - 直接读写 V3D 寄存器（PAGE_DEV 非缓存映射）；
 *   - 通过 foot 共享页协议给合成器/应用提供 2D 操作（FILL/BLIT/BLUR…）。
 *
 * 阶段：
 *   P0 窗口映射 + 读 HUB_IDENT0（"VHUB"）
 *   P1 SMS 上电 + PM 复位 + 读 IDENT1/IDENT3（版本/核数/iprev）
 *   P2 MMU（4MB 页表）+ L2C        （后续）
 *   P3 CSD fill 自检              （后续）
 *
 * 跑法：/bin/v3dd（服务端常驻）。
 ********************************************************************/
#include <stdio.h>

#include "client.h"
#include "v3d_csd.h"
#include "v3d_hw.h"
#include "v3d_mmu.h"
#include "v3d_regs.h"

/* 映射目标 VA：页对齐、远低于用户代码段（EXEC_ADDR），落在 MMIO 直通区。
 * 不 mmap：cap_map 直接写页表，VMA 不参与，避开了"匿名页被设备页覆盖"的
 * 缺页重映射冲突。page_destroy 不扫 EXEC_ADDR 以下，进程退出也不会误回收。 */
#define V3D_MAP_VA 0x40000000UL
#define PM_MAP_VA 0x41000000UL
#define PT_VA 0x42000000UL      /* 4MB 单级页表映射区 */
#define SCRATCH_VA 0x43000000UL /* MMU 未映射访问的落点页 */
#define SH_VA 0x44000000UL      /* shader 代码页 CPU 映射 */
#define UNI_VA 0x44100000UL     /* uniform 页 CPU 映射 */
#define SHADER_GVA 0x10000000UL /* shader 的 GPU 虚拟地址 */
#define UNIFORM_GVA 0x10001000UL /* uniform 的 GPU 虚拟地址 */

static volatile uint32_t* g_v3d_va;
static v3d_dev_t g_dev;
static uint32_t g_ident0;

/* 服务方法：fn==0 返回 HUB_IDENT0（探活）；其余 P0/P1 先回 0。 */
static void* v3d_fn(void* fn, void* args) {
  (void)args;
  if (fn == (void*)0) {
    return (void*)(unsigned long)g_ident0;
  }
  return (void*)0;
}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  printf("v3dd: registering foot service 'v3d'\n");

  /* 1) 映射 V3D 寄存器窗口（hub + core0 + SMS，0x40000） */
  foot_cap_t cap = foot_cap_get_mmio("v3d");
  if (cap == FOOT_CAP_NONE) {
    printf("v3dd: foot_cap_get_mmio(v3d) failed\n");
    return 1;
  }
  if (foot_cap_map(cap, (void*)V3D_MAP_VA, 0, 0x40000) != 0) {
    printf("v3dd: foot_cap_map(v3d) failed\n");
    return 1;
  }
  g_v3d_va = (volatile uint32_t*)V3D_MAP_VA;

  /* 2) 映射 PM GRAFX 电源域（V3DRSTN 复位用，1 页） */
  foot_cap_t pmcap = foot_cap_get_mmio("pm-grafx");
  if (pmcap == FOOT_CAP_NONE) {
    printf("v3dd: foot_cap_get_mmio(pm-grafx) failed\n");
    return 1;
  }
  if (foot_cap_map(pmcap, (void*)PM_MAP_VA, 0, 0x1000) != 0) {
    printf("v3dd: foot_cap_map(pm-grafx) failed\n");
    return 1;
  }

  /* 3) 组装设备视图：hub/core0/sms 都在 v3d 窗口内，pm 单独一页 */
  g_dev.hub = g_v3d_va + V3D_HUB_OFF / 4;
  g_dev.core0 = g_v3d_va + V3D_CORE0_OFF / 4;
  g_dev.sms = g_v3d_va + V3D_SMS_OFF / 4;
  g_dev.pm = (volatile uint32_t*)PM_MAP_VA;

  /* 4) P0：探活 */
  g_ident0 = *(volatile uint32_t*)(g_dev.hub + V3D_HUB_IDENT0 / 4);
  printf("v3dd: HUB_IDENT0 = 0x%08x (%c%c%c%c)\n", g_ident0,
         (char)(g_ident0 & 0xff), (char)((g_ident0 >> 8) & 0xff),
         (char)((g_ident0 >> 16) & 0xff), (char)((g_ident0 >> 24) & 0xff));
  if (v3d_probe(&g_dev) != 0) {
    printf("v3dd: V3D not detected, abort\n");
    return 1;
  }
  printf("v3dd: V3D detected (VHUB ok)\n");

  /* 5) P1：SMS 上电 + PM 复位 */
  if (v3d_sms_powerup(&g_dev) != 0) {
    printf("v3dd: SMS powerup failed\n");
  } else {
    printf("v3dd: SMS powered up\n");
  }
  v3d_pm_reset(&g_dev);
  printf("v3dd: PM reset done\n");

  /* 6) P1：解析 IDENT1/IDENT3 */
  uint32_t ver = 0, ncores = 0, iprev = 0;
  v3d_ident(&g_dev, &ver, &ncores, &iprev);
  printf("v3dd: ident ver=%u ncores=%u iprev=%u\n", ver, ncores, iprev);

  /* 7) P2：MMU（单级 4MB 页表）+ L2C */
  {
    uint32_t pt_pa = 0, pt_size = 0;
    uint32_t scratch_pa = 0, scratch_size = 0;

    foot_cap_t ptcap =
        foot_cap_alloc_arena(V3D_MMU_PT_BYTES, 0, 0, &pt_pa, &pt_size);
    if (ptcap == FOOT_CAP_NONE) {
      printf("v3dd: alloc page table failed (want %u bytes)\n",
             (unsigned)V3D_MMU_PT_BYTES);
    } else if (foot_cap_map(ptcap, (void*)PT_VA, 0, pt_size) != 0) {
      printf("v3dd: map page table failed\n");
    } else {
      /* scratch：未映射访问的落点页，避免 GPU 越界打穿物理内存 */
      foot_cap_t sc = foot_cap_alloc_arena(V3D_MMU_PAGE_SIZE, 0, 0, &scratch_pa,
                                           &scratch_size);
      if (sc == FOOT_CAP_NONE ||
          foot_cap_map(sc, (void*)SCRATCH_VA, 0, scratch_size) != 0) {
        printf("v3dd: alloc/map scratch failed, ILLEGAL_ADDR disabled\n");
        scratch_pa = 0;
      }

      /* 页表清零（全 invalid）→ cache clean → 编程 MMU */
      volatile uint32_t* pt = (volatile uint32_t*)PT_VA;
      for (uint32_t i = 0; i < V3D_MMU_PT_ENTRIES; i++) {
        pt[i] = 0;
      }
      foot_cache_clean((void*)pt, (void*)(PT_VA + V3D_MMU_PT_BYTES));

      int mrc = v3d_mmu_setup(&g_dev, pt, pt_pa, scratch_pa);
      printf("v3dd: MMU setup rc=%d pt_pa=0x%08x pt_size=%u scratch_pa=0x%08x\n",
             mrc, pt_pa, pt_size, scratch_pa);
      printf("v3dd: MMU ctl=0x%08x mmuc=0x%08x debug=0x%08x\n",
             v3d_mmu_ctl(&g_dev), v3d_mmuc_control(&g_dev),
             v3d_mmu_debug_info(&g_dev));

      int lrc = v3d_l2c_enable(&g_dev);
      printf("v3dd: L2C enable rc=%d l2cactl=0x%08x\n", lrc,
             v3d_l2c_status(&g_dev));

      /* 7b) P3：最小 CSD job 自检，shader = "nop ; nop ; thrsw"（线程结束）。
       * QPU 是 64 位指令：add=NOP(0xBB) | mul=NOP(0xE) | sig=THRSW(1)。
       * 只做 1 个 workgroup × 16 线程，不访问任何 buffer，纯粹验证 CSD 通路。 */
      uint32_t sh_pa = 0, sh_size = 0, uni_pa = 0, uni_size = 0;
      foot_cap_t shcap =
          foot_cap_alloc_arena(V3D_MMU_PAGE_SIZE, 0, 0, &sh_pa, &sh_size);
      foot_cap_t unicap =
          foot_cap_alloc_arena(V3D_MMU_PAGE_SIZE, 0, 0, &uni_pa, &uni_size);
      if (shcap != FOOT_CAP_NONE && unicap != FOOT_CAP_NONE &&
          foot_cap_map(shcap, (void*)SH_VA, 0, sh_size) == 0 &&
          foot_cap_map(unicap, (void*)UNI_VA, 0, uni_size) == 0) {
        volatile uint32_t* sh = (volatile uint32_t*)SH_VA;
        volatile uint32_t* uni = (volatile uint32_t*)UNI_VA;

        /* nop(waddr=0) ; nop(waddr=0) ; thrsw —— 单段模式的 program-end */
        sh[0] = 0xBB03F000u; /* bits[31:0]  */
        sh[1] = 0x38200000u; /* bits[63:32] */
        foot_cache_clean((void*)sh, (void*)(SH_VA + 8));

        for (int i = 0; i < 1024; i++) {
          uni[i] = 0;
        }
        foot_cache_clean((void*)uni, (void*)(UNI_VA + V3D_MMU_PAGE_SIZE));

        /* 把 shader / uniform 页映射进 GPU 地址空间，再 flush 页表 */
        v3d_mmu_map_page(pt, SHADER_GVA, sh_pa);
        v3d_mmu_map_page(pt, UNIFORM_GVA, uni_pa);
        foot_cache_clean((void*)pt, (void*)(PT_VA + V3D_MMU_PT_BYTES));
        v3d_mmu_flush(&g_dev);

        /* 1 workgroup × 16 线程 */
        uint32_t cfg[7] = {0};
        cfg[0] = 1u << 16; /* NUM_WGS_X = 1 */
        cfg[1] = 1u << 16; /* NUM_WGS_Y = 1 */
        cfg[2] = 1u << 16; /* NUM_WGS_Z = 1 */
        cfg[3] = (1u << 8) | (0u << 12) | (16u << 0); /* wgs_per_sg/batches/wg_size */
        cfg[4] = 0;           /* num_batches = 1（7.1.6 前是减 1） */
        cfg[5] = SHADER_GVA | V3D_CSD_CFG5_SINGLE_SEG; /* shader VA + 单段 */
        cfg[6] = UNIFORM_GVA; /* uniform 地址（GPU VA，4 字节对齐） */

        printf("v3dd: CSD cfg=[%08x %08x %08x %08x %08x %08x %08x]\n", cfg[0],
               cfg[1], cfg[2], cfg[3], cfg[4], cfg[5], cfg[6]);
        v3d_csd_submit(&g_dev, cfg);
        int crc = v3d_csd_wait_idle(&g_dev, 1000);
        printf(
            "v3dd: CSD job rc=%d status=0x%08x cur_cfg4=0x%08x vio_addr=0x%08x "
            "vio_id=0x%08x\n",
            crc, v3d_csd_status(&g_dev),
            *(volatile uint32_t*)(g_dev.core0 + V3D_CSD_CURRENT_CFG4 / 4),
            v3d_mmu_vio_addr(&g_dev), v3d_mmu_vio_id(&g_dev));
      } else {
        printf("v3dd: CSD selftest skipped (alloc/map failed)\n");
      }
    }
  }

  /* 8) P3：CSD 通路探活（此时没有提交，CSD_STATUS 应为 0 = 空闲） */
  printf("v3dd: CSD status=0x%08x\n", v3d_csd_status(&g_dev));

  /* 9) 注册服务并进入服务循环 */
  client_t* client = client_regist("v3d");
  if (client == NULL) {
    printf("v3dd: client_regist(v3d) failed\n");
    return 1;
  }
  printf("v3dd: service 'v3d' ready\n");
  client_run(client, v3d_fn);
  return 0;
}
