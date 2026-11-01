/*******************************************************************
 * V3D MMU 编程 + L2C 使能（用户态 foot 服务侧，P2）。
 *
 * MMU 是【单级页表】：V3D 4GB 地址空间 / 4KB 页 = 1M 个 u32 表项，
 * 需要 4MB 物理连续内存（本文件不负责分配，由调用方用
 * foot_cap_alloc_arena 申请后把虚拟地址 + 物理基址传进来）。
 ********************************************************************/
#ifndef V3D_MMU_H
#define V3D_MMU_H

#include <stdint.h>

#include "v3d_hw.h"

/* 编程 MMU 寄存器并 flush（不改页表内容）。
 * 调用方需先把页表清零/填好 PTE，并 foot_cache_clean 到 DRAM 再调本函数。
 * 参数：
 *   v          设备视图
 *   pt         页表虚拟地址（4MB，1M 项，调用前已映射；仅接口校验）
 *   pt_pa      页表物理基址（foot_cap_alloc_arena 回填的 pa）
 *   scratch_pa 未映射访问重定向到的 scratch 页物理地址（0 = 不启用）
 * 返回 0 成功；-1 参数非法；-2 flush 超时。 */
int v3d_mmu_setup(v3d_dev_t* v, volatile uint32_t* pt, uint32_t pt_pa,
                  uint32_t scratch_pa);

/* 在页表里建立一条 4KB 映射（GPU 虚拟地址 va → 物理地址 pa）。 */
void v3d_mmu_map_page(volatile uint32_t* pt, uint32_t va, uint32_t pa);

/* MMUC flush + TLB clear。返回 0 成功。 */
int v3d_mmu_flush(v3d_dev_t* v);

/* 读回状态（验证 / 调试用）。 */
uint32_t v3d_mmu_ctl(v3d_dev_t* v);
uint32_t v3d_mmuc_control(v3d_dev_t* v);
uint32_t v3d_mmu_debug_info(v3d_dev_t* v);
uint32_t v3d_mmu_vio_addr(v3d_dev_t* v);
uint32_t v3d_mmu_vio_id(v3d_dev_t* v);

/* L2C：先清（L2CCLR）再使能（L2CENA）。返回 0 成功；-3 使能不回读。 */
int v3d_l2c_enable(v3d_dev_t* v);
uint32_t v3d_l2c_status(v3d_dev_t* v);

#endif /* V3D_MMU_H */
