/*******************************************************************
 * 固件 mailbox（property channel）—— 用户态最小实现。
 *
 * 【为什么需要它】V3D 核心时钟必须显式使能。Linux v3d_power_resume() 的
 * 第一步就是 clk_prepare_enable(v3d->clk)。如果固件把 V3D 时钟留在关闭
 * 状态，寄存器读写照样能进行（APB 时钟独立），但 QPU 阵列不推进：
 * job 被接收、CSD_STATUS 内部序号递增、INT_STS 只有残留位、像素一个都不
 * 写 —— 正是我们实测到的现象。
 ********************************************************************/
#ifndef V3D_MBOX_H
#define V3D_MBOX_H

#include <stdint.h>

/* 初始化：映射 mailbox 寄存器 + 分配 property buffer（物理连续）。
 * 返回 0 成功；-1 表示拿不到 mailbox 窗口或内存。 */
int v3d_mbox_init(void);

/* 查询 clock 的最大可用频率（Hz）。0 = 失败。 */
uint32_t v3d_mbox_get_max_clock(uint32_t clock_id);

/* 查询 clock 的当前频率（Hz）。0 = 失败。 */
uint32_t v3d_mbox_get_clock(uint32_t clock_id);

/* 设置 clock 频率。返回 0 成功。 */
int v3d_mbox_set_clock(uint32_t clock_id, uint32_t rate);

/* V3D 的固件 clock id。 */
#define V3D_MBOX_CLOCK_V3D 5u

#endif /* V3D_MBOX_H */
