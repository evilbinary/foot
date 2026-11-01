/*******************************************************************
 * V3D 7.1（VideoCore VII）寄存器常量 —— 从 Linux drm/v3d 的 v3d_regs.h
 * 蓝本裁剪（7.1 用到的子集），并补充窗口布局与 PM 电源域。
 *
 * 窗口基址 0x1002000000（大小 0x40000）：
 *   hub    + 0x00000   共享硬件（MMU / 中断 / AXI）
 *   core0  + 0x08000   CLE / CSD / L2C / L2T
 *   SMS    + 0x30800   电源状态机（TEE 在 +0x400）
 * PM 电源域 0x107D200000（GRAFX，+0x10c 的 bit6 = V3DRSTN）。
 ********************************************************************/
#ifndef V3D_REGS_H
#define V3D_REGS_H

#include <stdint.h>

/* ---- 窗口内偏移 ---- */
#define V3D_HUB_OFF 0x00000u
#define V3D_CORE0_OFF 0x08000u
#define V3D_SMS_OFF 0x30800u

/* ---- hub 寄存器 ---- */
#define V3D_HUB_AXICFG 0x00000u
#define V3D_HUB_IDENT0 0x00008u /* 读回 ASCII "VHUB" = 0x42554856 */
#define V3D_HUB_IDENT1 0x0000cu
#define V3D_HUB_IDENT1_TVER_SHIFT 0
#define V3D_HUB_IDENT1_TVER_MASK 0xFu
#define V3D_HUB_IDENT1_NCORES_SHIFT 8
#define V3D_HUB_IDENT1_NCORES_MASK 0xFu
#define V3D_HUB_IDENT3 0x00014u
#define V3D_HUB_IDENT3_IPREV_SHIFT 8
#define V3D_HUB_IDENT3_IPREV_MASK 0xFFu
#define V3D_HUB_INT_STS 0x00050u
#define V3D_HUB_INT_CLR 0x00058u

/* ---- core0 寄存器 ---- */
#define V3D_CTL_IDENT0 0x00000u
#define V3D_CTL_IDENT1 0x00004u /* NSEM/NTMU/QUPS/NSLC/REV */
#define V3D_CTL_L2CACTL 0x00020u
#define V3D_L2CACTL_L2CCLR (1u << 2)
#define V3D_L2CACTL_L2CENA (1u << 0)
#define V3D_CTL_INT_STS 0x00050u
#define V3D_CTL_INT_CLR 0x00058u
#define V3D_INT_CSDDONE (1u << 6) /* 7.1 的 CSD_DONE 是 bit6 */
#define V3D_CTL_INT_MSK_SET 0x00060u
#define V3D_CTL_INT_MSK_CLR 0x00064u

/* ---- SMS 电源状态机（相对 V3D_SMS_OFF）---- */
#define V3D_SMS_REE_CS 0x000u
#define V3D_SMS_TEE_CS 0x400u
#define V3D_SMS_INTERRUPT (1u << 31)
#define V3D_SMS_POWER_OFF (1u << 30)
#define V3D_SMS_CLEAR_POWER_OFF (1u << 29)
#define V3D_SMS_STATE_MASK 0xFu
#define V3D_SMS_STATE_IDLE 0x0u
#define V3D_SMS_STATE_ISOLATING_FOR_RESET 0xAu
#define V3D_SMS_STATE_RESETTING 0xBu

/* ---- PM 电源域（0x107D200000，映射后按页内偏移访问）---- */
#define V3D_PM_GRAFX_OFF 0x10cu
#define V3D_PM_PASSWORD 0x5A000000u
#define V3D_PM_V3DRSTN (1u << 6)

#endif /* V3D_REGS_H */
