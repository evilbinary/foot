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
#define V3D_CTL_INT_MSK_STS 0x0005cu /* 当前屏蔽状态（读回确认） */

/* CLE control-thread status（core0 窗口）。CTRUN 置位表示线程还在跑；
 * 上一次 gpud 把 CT1 卡死后，若不做 SMS/PM 复位，新进程读到的仍是
 * CTRUN=1 + CA 停在旧 arena 页边界（典型 +4096），新 QBA/QEA 不会被取。 */
#define V3D_CLE_CT1CS 0x00104u
#define V3D_CLE_CT1CA 0x00114u
#define V3D_CLE_CT1CS_CTRUN (1u << 5)

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

/* ---------------- 缓存失效（CSD/CL 提交前必做） ----------------
 * Linux v3d_csd_job_run() 在写 CFG0 之前调 v3d_invalidate_caches()：
 *   v3d_flush_l2t()        → L2T 全部失效（QPU 取指令/uniform 走这里）
 *   v3d_invalidate_slices()→ slice cache 失效（只读缓存）
 * 不做的话，QPU 可能从 L2T/slice 里取到脏的指令或 uniform，表现为
 * "job 收下了但什么都不写"（实测 CSD_STATUS 的 NUM_COMPLETED 不前进）。 */
#define V3D_CTL_SLCACTL 0x00024u /* slice cache 控制 */
#define V3D_SLCACTL_TVCCS_SHIFT 24
#define V3D_SLCACTL_TDCCS_SHIFT 16
#define V3D_SLCACTL_UCC_SHIFT 8
#define V3D_SLCACTL_ICC_SHIFT 0

#define V3D_CTL_SLCACTL 0x00024u
/* per-slice 读侧缓存清除。四个 4 位字段 = "清多少个缓存行"，写 0xF 表示整块清空：
 *   ICC   [3:0]   指令缓存
 *   UCC   [11:8]  uniform 缓存
 *   TDCCS [19:16] TMU 数据缓存
 *   TVCCS [27:24] TMU varying 缓存
 * 【为什么必须有】TMU 的读操作依赖 data/varying 缓存。不先清掉，读会挂在
 * 等待一个永远不会到来的响应上 —— 实测：把一次已确认能成功的写入放到读
 * 之后，写入不再发生，说明线程死在读那里（不是"读到 0"）。 */
#define V3D_SLCACTL_CLEAR_ALL 0x0F0F0F0Fu

#define V3D_CTL_L2TCACTL 0x00030u
#define V3D_L2TCACTL_TMUWCF (1u << 8)  /* TMU 写合并缓冲排空 */
/* L1 Cache Flush Control（packet 76）映射到 L2TCACTL 的高位段：
 *   [15:12] TMU Config Cache Clear   ← 我们从没清过
 *   [11:8]  TMU Data Cache Clear     ← 即 TMUWCF
 * 若配置缓存陈旧，QPU 写 tmuc 不会生效，后续 TMU 读会沿用旧的（无效）
 * 配置而返回 0 —— 正好是 fill（不依赖 config，正常）与 copy（依赖，全 0）
 * 的分界。 */
#define V3D_L2TCACTL_TMUCFCF (1u << 12) /* TMU 配置缓存清除 */
#define V3D_L2TCACTL_FLM_FLUSH 0u     /* 失效（不写回） */
#define V3D_L2TCACTL_FLM_CLEAN 2u     /* 写回并标干净 */
#define V3D_L2TCACTL_FLM_SHIFT 1
#define V3D_L2TCACTL_L2TFLS (1u << 0) /* 忙标志，写完自清 */

#define V3D_CTL_L2TFLSTA 0x00034u /* 失效区间起始 */
#define V3D_CTL_L2TFLEND 0x00038u /* 失效区间结束（含） */

#endif /* V3D_REGS_H */
