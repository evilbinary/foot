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
