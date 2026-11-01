/*******************************************************************
 * V3D 7.1 硬件 bring-up（用户态 foot 服务侧）。
 * 寄存器窗口经 foot_cap_get_mmio + foot_cap_map 映射进本进程，
 * 这里只持有映射后的虚拟地址指针，直接读写寄存器（PAGE_DEV 非缓存）。
 ********************************************************************/
#ifndef V3D_HW_H
#define V3D_HW_H

#include <stdint.h>

typedef struct v3d_dev {
  volatile uint32_t* hub;   /* hub   窗口（MMU/中断/AXI） */
  volatile uint32_t* core0; /* core0 窗口（CLE/CSD/L2C/L2T） */
  volatile uint32_t* sms;   /* SMS   电源状态机 */
  volatile uint32_t* pm;    /* PM GRAFX 电源域 */
} v3d_dev_t;

/* 读 HUB_IDENT0 校验 "VHUB"。返回 0 = 探测到 V3D。 */
int v3d_probe(v3d_dev_t* v);

/* SMS 上电 + kick reset。见 Linux v3d_power.c / v3d_gem.c:reset_sms。 */
int v3d_sms_powerup(v3d_dev_t* v);

/* PM 电源域复位 QPU 阵列（V3DRSTN 清 0 → 置 1）。缺这步 QPU 永不启动。 */
int v3d_pm_reset(v3d_dev_t* v);

/* 解析 HUB_IDENT1/IDENT3：版本号、核数、IP 版本。 */
void v3d_ident(v3d_dev_t* v, uint32_t* ver, uint32_t* ncores, uint32_t* iprev);

#endif /* V3D_HW_H */
