/*******************************************************************
 * V3D CSD（计算着色器分发）提交骨架（用户态 foot 服务侧，P3）。
 *
 * 提交序列严格对照 Linux drm/v3d/v3d_sched.c: v3d_csd_job_run()：
 *   for (i = 1; i <= 6; i++)
 *       V3D_CORE_WRITE(0, CFG0 + 4*i, cfg[i]);
 *   if (ver >= 71) V3D_CORE_WRITE(0, V7_CSD_QUEUED_CFG7, 0);
 *   V3D_CORE_WRITE(0, CFG0, cfg[0]);   // CFG0 写下去即触发
 ********************************************************************/
#include "v3d_csd.h"

#include <unistd.h>

#include "v3d_regs.h"

static inline uint32_t rd32(volatile uint32_t* p) {
  uint32_t v = *p;
  __asm__ volatile("dmb sy" ::: "memory");
  return v;
}

static inline void wr32(volatile uint32_t* p, uint32_t v) {
  *p = v;
  __asm__ volatile("dmb sy" ::: "memory");
}

void v3d_csd_submit(v3d_dev_t* v, const uint32_t cfg[7]) {
  if (v == NULL || v->core0 == NULL || cfg == NULL) {
    return;
  }
  volatile uint32_t* cfg0 = v->core0 + V3D_CSD_QUEUED_CFG0 / 4;

  /* CFG1..CFG6 */
  for (int i = 1; i <= 6; i++) {
    wr32(cfg0 + i, cfg[i]);
  }
  /* V3D 7.x 的第八个配置寄存器，不使用必须清零 */
  wr32(v->core0 + V3D_CSD_QUEUED_CFG7 / 4, 0);

  /* CFG0 写下去即触发 job */
  wr32(cfg0, cfg[0]);
}

uint32_t v3d_csd_status(v3d_dev_t* v) {
  return (v != NULL && v->core0 != NULL)
             ? rd32(v->core0 + V3D_CSD_STATUS / 4)
             : 0;
}

int v3d_csd_wait_idle(v3d_dev_t* v, int timeout_ms) {
  if (v == NULL || v->core0 == NULL) {
    return -1;
  }
  for (int i = 0; i < timeout_ms; i++) {
    uint32_t st = v3d_csd_status(v);
    if (!(st & (V3D_CSD_STATUS_HAVE_QUEUED |
                V3D_CSD_STATUS_HAVE_CURRENT))) {
      return 0;
    }
    usleep(1000);
  }
  return -1;
}

void v3d_csd_clear_int(v3d_dev_t* v) {
  if (v != NULL && v->core0 != NULL) {
    wr32(v->core0 + V3D_CTL_INT_CLR / 4, V3D_INT_CSDDONE);
  }
}
