/*******************************************************************
 * V3D CSD（计算着色器分发）提交骨架（用户态 foot 服务侧，P3）。
 *
 * 对照 Linux drm/v3d/v3d_sched.c: v3d_csd_job_run()：
 *   写 CFG1..CFG6 → 写 CFG7(0) → 写 CFG0 触发。
 * 完成后靠 CSD_STATUS 的 HAVE_QUEUED/HAVE_CURRENT 位归零判断空闲。
 ********************************************************************/
#ifndef V3D_CSD_H
#define V3D_CSD_H

#include <stdint.h>

#include "v3d_hw.h"

/* 提交一次 CSD 分发。
 * cfg[0..6] 对应 CSD_QUEUED_CFG0..CFG6 的原始值（见 v3d_regs.h 位域宏）。
 * 内部先写 CFG1..CFG6、CFG7=0，最后写 CFG0 触发。
 * 注意：cfg[0..2] 的 workgroup 数任一维为 0 时硬件不分发（同 Linux 语义）。 */
void v3d_csd_submit(v3d_dev_t* v, const uint32_t cfg[7]);

/* 读 CSD_STATUS 原始值。 */
uint32_t v3d_csd_status(v3d_dev_t* v);

/* 等 CSD 空闲（HAVE_QUEUED 与 HAVE_CURRENT 都清零）。
 * 返回 0 成功；-1 超时。timeout_ms 为墙钟毫秒。 */
int v3d_csd_wait_idle(v3d_dev_t* v, int timeout_ms);

/* 清 CSD 完成中断（V3D_INT_CSDDONE）。 */
void v3d_csd_clear_int(v3d_dev_t* v);

#endif /* V3D_CSD_H */
