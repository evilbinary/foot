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
