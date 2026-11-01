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

/* ---- MMU（hub 窗口；对照 Linux drm/v3d/v3d_regs.h）----
 *   0x01000 MMUC_CONTROL  MMU 控制器（ENABLE / FLUSH）
 *   0x01200 MMU_CTL       MMU 主控制（ENABLE / TLB_CLEAR / 各类 fault 动作）
 *   0x01204 MMU_PT_PA_BASE 单级页表物理基址（写 PFN = pa>>12）
 *   0x0122c VIO_ID        触发越界的 AXI master id
 *   0x01230 ILLEGAL_ADDR  未映射访问重定向到的 scratch 页（PFN | bit31 ENABLE）
 *   0x01234 VIO_ADDR      触发越界的虚拟地址
 *   0x01238 DEBUG_INFO    MMU 版本 / PA/VA 位宽
 *
 * 页表是【单级】的：V3D 4GB 地址空间 / 4KB 页 = 1M 个 u32 表项 = 4MB 连续内存。
 * 表项格式：[27:0]=PFN(pa>>12)，bit28=VALID，bit29=WRITEABLE，
 *          bit30=BIGPAGE(64KB)，bit31=SUPERPAGE(1MB)。 */
#define V3D_MMUC_CONTROL 0x01000u
#define V3D_MMUC_CONTROL_ENABLE (1u << 0)
#define V3D_MMUC_CONTROL_FLUSH (1u << 1)
#define V3D_MMUC_CONTROL_FLUSHING (1u << 2)
#define V3D_MMUC_CONTROL_CLEAR (1u << 11) /* V3D 7.1+；旧版是 bit3 */

#define V3D_MMU_CTL 0x01200u
#define V3D_MMU_CTL_ENABLE (1u << 0)
#define V3D_MMU_CTL_TLB_STATS_ENABLE (1u << 1)
#define V3D_MMU_CTL_TLB_CLEAR (1u << 2)
#define V3D_MMU_CTL_TLB_STATS_CLEAR (1u << 3)
#define V3D_MMU_CTL_TLB_CLEARING (1u << 7)
#define V3D_MMU_CTL_WRITE_VIOLATION_INT (1u << 10)
#define V3D_MMU_CTL_WRITE_VIOLATION_ABORT (1u << 11)
#define V3D_MMU_CTL_WRITE_VIOLATION (1u << 12)
#define V3D_MMU_CTL_PT_INVALID_ENABLE (1u << 16)
#define V3D_MMU_CTL_PT_INVALID_INT (1u << 18)
#define V3D_MMU_CTL_PT_INVALID_ABORT (1u << 19)
#define V3D_MMU_CTL_PT_INVALID (1u << 20)
#define V3D_MMU_CTL_CAP_EXCEEDED_INT (1u << 25)
#define V3D_MMU_CTL_CAP_EXCEEDED_ABORT (1u << 26)
#define V3D_MMU_CTL_CAP_EXCEEDED (1u << 27)

#define V3D_MMU_PT_PA_BASE 0x01204u
#define V3D_MMU_VIO_ID 0x0122cu
#define V3D_MMU_ILLEGAL_ADDR 0x01230u
#define V3D_MMU_ILLEGAL_ADDR_ENABLE (1u << 31)
#define V3D_MMU_VIO_ADDR 0x01234u
#define V3D_MMU_DEBUG_INFO 0x01238u
#define V3D_MMU_DEBUG_VERSION_SHIFT 0
#define V3D_MMU_DEBUG_VERSION_MASK 0xFu
#define V3D_MMU_DEBUG_VA_WIDTH_SHIFT 4
#define V3D_MMU_DEBUG_VA_WIDTH_MASK 0xF0u
#define V3D_MMU_DEBUG_PA_WIDTH_SHIFT 8
#define V3D_MMU_DEBUG_PA_WIDTH_MASK 0xF00u

/* 单级页表项 / 位移 */
#define V3D_MMU_PAGE_SHIFT 12
#define V3D_MMU_PAGE_SIZE (1u << V3D_MMU_PAGE_SHIFT)
#define V3D_MMU_PT_ENTRIES 0x100000u              /* 4GB / 4KB */
#define V3D_MMU_PT_BYTES (V3D_MMU_PT_ENTRIES * 4u) /* 4MB */
#define V3D_PTE_SUPERPAGE (1u << 31)
#define V3D_PTE_BIGPAGE (1u << 30)
#define V3D_PTE_WRITEABLE (1u << 29)
#define V3D_PTE_VALID (1u << 28)
#define V3D_PTE_PFN_MASK 0x0FFFFFFFu

/* ---- TFU（Texture Formatting Unit，hub 窗口 0x700 起；V3D 7.x）----
 * 用途：图像/内存格式转换与拷贝。V3D 7.1 支持 stride-0 源（把第一行
 * 广播到所有输出行）⇒ 可做纯色/图案填充，不需要 shader、不需要命令流。
 * 提交流程对照 Linux drm/v3d/v3d_sched.c: v3d_tfu_job_run()：
 *   写 IIA/IIS/ICA/IUA/IOA/IOC/IOS/COEF* → 最后写 ICFG（带 IOC 位）触发。 */
#define V3D_TFU_CS 0x00700u /* 控制/状态 */
#define V3D_TFU_CS_BUSY (1u << 0)
#define V3D_TFU_CS_NFREE_SHIFT 8
#define V3D_TFU_CS_NFREE_MASK 0x3F00u
#define V3D_TFU_CS_CVTCT_SHIFT 16
#define V3D_TFU_CS_CVTCT_MASK 0xFF0000u
#define V3D_TFU_CS_TFURST (1u << 31)

#define V3D_TFU_SU 0x00704u
#define V3D_TFU_ICFG 0x00708u /* 输入配置；写下去即触发 job */
#define V3D_TFU_ICFG_IOC (1u << 0) /* 完成时产生中断 */
#define V3D_TFU_IIA 0x0070cu   /* 输入图像地址（GPU VA） */
#define V3D_TFU_ICA 0x00710u   /* 输入 chroma 地址 */
#define V3D_TFU_IIS 0x00714u   /* 输入图像 stride（stride/cpp；0 = 广播） */
#define V3D_TFU_IUA 0x00718u   /* 输入 U-plane 地址 */
#define V3D_V7_TFU_IOC 0x0071cu /* 输出图像配置（V3D 7.x 专用） */
#define V3D_V7_TFU_IOC_FORMAT_SHIFT 12
#define V3D_V7_TFU_IOC_FORMAT_RASTER 0u
#define V3D_V7_TFU_IOC_FORMAT_LINEARTILE 3u
#define V3D_V7_TFU_IOC_STRIDE_SHIFT 16
#define V3D_V7_TFU_IOC_NUMMM_SHIFT 4
#define V3D_V7_TFU_IOC_DIMTW (1u << 0)
#define V3D_TFU_IOA 0x00720u /* 输出图像地址（GPU VA） */
#define V3D_TFU_IOS 0x00724u /* 输出尺寸：(height << 16) | width */
#define V3D_TFU_COEF0 0x00728u
#define V3D_TFU_COEF1 0x0072cu
#define V3D_TFU_COEF2 0x00730u
#define V3D_TFU_COEF3 0x00734u

#define V3D_TFU_ICFG_IFORMAT_SHIFT 23
#define V3D_TFU_ICFG_FORMAT_RASTER 0u
#define V3D_TFU_ICFG_FORMAT_LINEARTILE 3u
#define V3D_TFU_ICFG_OTYPE_SHIFT 16
/* Texture Data Format（v3d_packet.xml） */
#define V3D_TEX_FORMAT_RGBA8UI 101u /* 32bpp RGBA */
#define V3D_TEX_FORMAT_RGB565 6u    /* 16bpp（B in low bits） */

/* HUB 中断状态/清除位 */
#define V3D_HUB_INT_TFUC (1u << 1) /* TFU 转换完成 */
#define V3D_HUB_INT_TFUF (1u << 0) /* TFU fifo */

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

/* ---- CSD（计算着色器分发，core0 窗口；V3D 7.x 偏移）----
 * 提交流程（对照 Linux drm/v3d/v3d_sched.c: v3d_csd_job_run）：
 *   写 CFG1..CFG6 → 写 CFG7（7.x 置 0）→ 最后写 CFG0 触发 job。
 * CFG0..CFG2 = 各维 workgroup 数（高 16 位）+ 偏移（低 16 位）。
 * CFG3 = WG_SIZE(7:0) | WGS_PER_SG(11:8) | BATCHES_PER_SG_M1(19:12)
 *       | MAX_SG_ID(25:20) | OVERLAP(26)。
 * CFG4 = 批次数（V3D 7.1.6 以前是 "减 1"）。
 * CFG5 = shader 地址记录（地址 + pnan/singleseg/threading 标志）。
 * CFG6 = uniform 地址（4 字节对齐）。 */
#define V3D_CSD_STATUS 0x00900u
#define V3D_CSD_STATUS_NUM_COMPLETED_SHIFT 4
#define V3D_CSD_STATUS_NUM_COMPLETED_MASK 0xFF0u
#define V3D_CSD_STATUS_NUM_ACTIVE_SHIFT 2
#define V3D_CSD_STATUS_NUM_ACTIVE_MASK 0xCu
#define V3D_CSD_STATUS_HAVE_CURRENT (1u << 1)
#define V3D_CSD_STATUS_HAVE_QUEUED (1u << 0)

#define V3D_CSD_QUEUED_CFG0 0x00930u
#define V3D_CSD_CFG0_NUM_WGS_X_SHIFT 16
#define V3D_CSD_CFG0_NUM_WGS_X_MASK 0xFFFF0000u
#define V3D_CSD_CFG0_WG_X_OFFSET_SHIFT 0
#define V3D_CSD_CFG0_WG_X_OFFSET_MASK 0x0000FFFFu

#define V3D_CSD_QUEUED_CFG1 0x00934u
#define V3D_CSD_CFG1_NUM_WGS_Y_SHIFT 16
#define V3D_CSD_CFG1_NUM_WGS_Y_MASK 0xFFFF0000u
#define V3D_CSD_CFG1_WG_Y_OFFSET_SHIFT 0
#define V3D_CSD_CFG1_WG_Y_OFFSET_MASK 0x0000FFFFu

#define V3D_CSD_QUEUED_CFG2 0x00938u
#define V3D_CSD_CFG2_NUM_WGS_Z_SHIFT 16
#define V3D_CSD_CFG2_NUM_WGS_Z_MASK 0xFFFF0000u
#define V3D_CSD_CFG2_WG_Z_OFFSET_SHIFT 0
#define V3D_CSD_CFG2_WG_Z_OFFSET_MASK 0x0000FFFFu

#define V3D_CSD_QUEUED_CFG3 0x0093cu
#define V3D_CSD_CFG3_WG_SIZE_SHIFT 0
#define V3D_CSD_CFG3_WG_SIZE_MASK 0x000000FFu
#define V3D_CSD_CFG3_WGS_PER_SG_SHIFT 8
#define V3D_CSD_CFG3_WGS_PER_SG_MASK 0x00000F00u
#define V3D_CSD_CFG3_BATCHES_PER_SG_M1_SHIFT 12
#define V3D_CSD_CFG3_BATCHES_PER_SG_M1_MASK 0x000FF000u
#define V3D_CSD_CFG3_MAX_SG_ID_SHIFT 20
#define V3D_CSD_CFG3_MAX_SG_ID_MASK 0x03F00000u
#define V3D_CSD_CFG3_OVERLAP_WITH_PREV (1u << 26)

#define V3D_CSD_QUEUED_CFG4 0x00940u /* 批次数（7.1.6 前为 "减 1"） */
#define V3D_CSD_QUEUED_CFG5 0x00944u /* shader 地址记录 */
#define V3D_CSD_CFG5_THREADING (1u << 0)      /* 4 线程/段 */
#define V3D_CSD_CFG5_SINGLE_SEG (1u << 1)     /* 单段（无中间 thread switch） */
#define V3D_CSD_CFG5_PROPAGATE_NANS (1u << 2) /* V3D 7.x 保留（不再使用） */
#define V3D_CSD_QUEUED_CFG6 0x00948u /* uniform 地址（4 字节对齐） */
#define V3D_CSD_QUEUED_CFG7 0x0094cu /* V3D 7.x only，未用需置 0 */

#define V3D_CSD_CURRENT_CFG0 0x00958u
#define V3D_CSD_CURRENT_CFG1 0x0095cu
#define V3D_CSD_CURRENT_CFG2 0x00960u
#define V3D_CSD_CURRENT_CFG3 0x00964u
#define V3D_CSD_CURRENT_CFG4 0x00968u
#define V3D_CSD_CURRENT_CFG5 0x0096cu
#define V3D_CSD_CURRENT_CFG6 0x00970u
#define V3D_CSD_CURRENT_ID0 0x00978u
#define V3D_CSD_CURRENT_ID1 0x0097cu

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
