/*******************************************************************
 * V3D 7.1 硬件 bring-up 实现（用户态 foot 服务侧）。
 * 参考 Linux drm/v3d（v3d_power.c / v3d_gem.c / v3d_regs.h），
 * 序列与 EwokOS libvideocore（v3d_g2d.c）一致，真机尚未验证。
 ********************************************************************/
#include "v3d_hw.h"
#include "v3d_regs.h"

#include <unistd.h> /* usleep */

/* 设备内存（PAGE_DEV）访问：volatile 读写 + 数据屏障保证顺序。 */
static inline uint32_t rd32(volatile uint32_t* p) {
  uint32_t v = *p;
  __asm__ volatile("dmb sy" ::: "memory");
  return v;
}

static inline void wr32(volatile uint32_t* p, uint32_t v) {
  *p = v;
  __asm__ volatile("dmb sy" ::: "memory");
}

int v3d_probe(v3d_dev_t* v) {
  if (v == NULL || v->hub == NULL) {
    return -1;
  }
  uint32_t ident0 = rd32(v->hub + V3D_HUB_IDENT0 / 4);
  return ident0 == 0x42554856u ? 0 : -1; /* "VHUB" */
}

int v3d_sms_powerup(v3d_dev_t* v) {
  if (v == NULL || v->sms == NULL) {
    return -1;
  }
  /* 1) TEE 写 CLEAR_POWER_OFF(1<<29)，等 STATE==IDLE(0) */
  wr32(v->sms + V3D_SMS_TEE_CS / 4, V3D_SMS_CLEAR_POWER_OFF);
  for (int i = 0; i < 100; i++) {
    if ((rd32(v->sms + V3D_SMS_TEE_CS / 4) & V3D_SMS_STATE_MASK) ==
        V3D_SMS_STATE_IDLE) {
      break;
    }
    usleep(1000);
  }
  /* 2) REE 写 0x4（kick reset），等 STATE 离开 ISOLATING/RESETTING */
  wr32(v->sms + V3D_SMS_REE_CS / 4, 0x4u);
  for (int i = 0; i < 100; i++) {
    uint32_t st = rd32(v->sms + V3D_SMS_REE_CS / 4) & V3D_SMS_STATE_MASK;
    if (st != V3D_SMS_STATE_ISOLATING_FOR_RESET && st != V3D_SMS_STATE_RESETTING) {
      break;
    }
    usleep(1000);
  }
  /* 3) AXI 最大 burst（EwokOS：hub[0x00] = 0xF） */
  if (v->hub != NULL) {
    wr32(v->hub + V3D_HUB_AXICFG / 4, 0xFu);
  }
  return 0;
}

int v3d_pm_reset(v3d_dev_t* v) {
  if (v == NULL || v->pm == NULL) {
    return -1;
  }
  volatile uint32_t* reg = v->pm + V3D_PM_GRAFX_OFF / 4;
  /* 复位：V3DRSTN 清 0（保留其它位，带 PASSWORD） */
  uint32_t cur = rd32(reg);
  wr32(reg, V3D_PM_PASSWORD | (cur & ~V3D_PM_V3DRSTN));
  usleep(20);
  /* 释放复位：V3DRSTN 置 1 */
  cur = rd32(reg);
  wr32(reg, V3D_PM_PASSWORD | (cur | V3D_PM_V3DRSTN));
  usleep(200);
  return 0;
}

void v3d_ident(v3d_dev_t* v, uint32_t* ver, uint32_t* ncores, uint32_t* iprev) {
  uint32_t ident1 = 0, ident3 = 0;
  if (v != NULL && v->hub != NULL) {
    ident1 = rd32(v->hub + V3D_HUB_IDENT1 / 4);
    ident3 = rd32(v->hub + V3D_HUB_IDENT3 / 4);
  }
  if (ver != NULL) {
    *ver = (ident1 >> V3D_HUB_IDENT1_TVER_SHIFT) & V3D_HUB_IDENT1_TVER_MASK;
  }
  if (ncores != NULL) {
    *ncores = (ident1 >> V3D_HUB_IDENT1_NCORES_SHIFT) & V3D_HUB_IDENT1_NCORES_MASK;
  }
  if (iprev != NULL) {
    *iprev = (ident3 >> V3D_HUB_IDENT3_IPREV_SHIFT) & V3D_HUB_IDENT3_IPREV_MASK;
  }
}

/* 失效 slice cache + 整个 L2T（Linux v3d_invalidate_caches 的 7.1 等价物：
 * ver>=41 跳过 L3 flush、ver>=33 跳过 L2C invalidate，只剩这两步）。
 *
 * 【为什么必须在 CSD 提交前做】QPU 取指令、读 uniform 都经过 L2T/slice
 * cache。若里面残留上一次 job 的行（或上电后的未初始化状态），QPU 会把
 * 指令流读错，硬件既不报 MMU fault 也不前进 NUM_COMPLETED —— 表现就是
 * "CFG0 写下去了，job 却像没发生"。 */
void v3d_invalidate_caches(v3d_dev_t* v) {
  if (v == NULL || v->core0 == NULL) {
    return;
  }
  volatile uint32_t* core = v->core0;

  /* 1) slice cache：TVCCS / TDCCS / UCC / ICC 各 0xf（= 全部失效） */
  wr32(core + V3D_CTL_SLCACTL / 4,
       (0xfu << V3D_SLCACTL_TVCCS_SHIFT) | (0xfu << V3D_SLCACTL_TDCCS_SHIFT) |
           (0xfu << V3D_SLCACTL_UCC_SHIFT) | (0xfu << V3D_SLCACTL_ICC_SHIFT));

  /* 2) L2T：区间设成整个地址空间，FLM_FLUSH（只失效、不写回），
   *    写 L2TFLS 触发；忙位由硬件自清。 */
  wr32(core + V3D_CTL_L2TFLSTA / 4, 0u);
  wr32(core + V3D_CTL_L2TFLEND / 4, 0xFFFFFFFFu);
  wr32(core + V3D_CTL_L2TCACTL / 4,
       V3D_L2TCACTL_L2TFLS |
           (V3D_L2TCACTL_FLM_FLUSH << V3D_L2TCACTL_FLM_SHIFT));

  for (int i = 0; i < 1000; i++) {
    if (!(rd32(core + V3D_CTL_L2TCACTL / 4) & V3D_L2TCACTL_L2TFLS)) {
      break;
    }
    usleep(100);
  }
}

/* 把 L2T 的脏行写回 DRAM（FLM_CLEAN），并先排空 TMU 写合并缓冲。
 * 对应 Linux v3d_clean_caches() / EwokOS g2d_caches_clean()。
 *
 * 【为什么必须有】CSD 写完的结果先落在 L2T，不一定马上进 DRAM。而
 * v3d_invalidate_caches 用的是 FLM_FLUSH（只失效、不写回）——如果下一
 * 次 dispatch 前直接 invalidate，上一次的结果会被【丢弃】。实测现象：
 *   fill（只写，不读源）  → 一直正常
 *   copy/alpha（要读源）  → 永远读回 0，写出来也是 0
 * 所以每次 job 完成、结果还在 L2T 里时，必须先 clean 再交给下一次使用。 */
void v3d_clean_caches(v3d_dev_t* v) {
  if (v == NULL || v->core0 == NULL) {
    return;
  }
  volatile uint32_t* core = v->core0;

  /* 1) 排空 TMU 写合并缓冲（Linux 在 clean 前必做，GFXH-1897） */
  wr32(core + V3D_CTL_L2TCACTL / 4, V3D_L2TCACTL_TMUWCF);
  for (int i = 0; i < 1000; i++) {
    if (!(rd32(core + V3D_CTL_L2TCACTL / 4) & V3D_L2TCACTL_TMUWCF)) {
      break;
    }
    usleep(10);
  }

  /* 2) L2T 写回：FLM_CLEAN（写回脏行、标干净，不失效） */
  wr32(core + V3D_CTL_L2TFLSTA / 4, 0u);
  wr32(core + V3D_CTL_L2TFLEND / 4, 0xFFFFFFFFu);
  wr32(core + V3D_CTL_L2TCACTL / 4,
       V3D_L2TCACTL_L2TFLS |
           (V3D_L2TCACTL_FLM_CLEAN << V3D_L2TCACTL_FLM_SHIFT));

  for (int i = 0; i < 2000; i++) {
    if (!(rd32(core + V3D_CTL_L2TCACTL / 4) & V3D_L2TCACTL_L2TFLS)) {
      break;
    }
    usleep(100);
  }
}
