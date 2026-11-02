/*
 * v3d_3d_compat.h - 让 EwokOS 的 v3d_3d.c 能直接编译在 foot 驱动里。
 *
 * 只做两件事：补类型/宏，声明 g2d 侧提供的函数。
 * 所有实现都在 v3d_g2d.c（它本来就有 g_g2d、页表、CSD 提交）。
 *
 * 关键前提：3D 后端假设「dma_alloc 返回的物理地址，低 32 位就是 GPU IOVA」。
 * v3d_g2d.c 里的 v3d_dma_alloc 按 identity 映射满足这个假设，
 * 所以 v3d_3d.c 的主体逻辑一行都不用改。
 */
#ifndef V3D_3D_COMPAT_H
#define V3D_3D_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "v3d_3d.h"

/* EwokOS 的类型与日志 */
typedef uint32_t ewokos_addr_t;
#define klog_d(...) printf(__VA_ARGS__)
#define klog(...) printf(__VA_ARGS__)
#define slog(...) printf(__VA_ARGS__)

/* CSD 作业（字段与 EwokOS 的 v3d_g2d_csd_job_t 一致） */
typedef struct {
  uint32_t code_iova;
  uint32_t unif_iova;
  uint32_t wg_count[3];
  uint32_t wg_size;
  uint8_t threads;
  uint8_t single_seg;
  uint32_t dst_phy, dst_span;
  uint32_t aux_phy, aux_span;
  uint32_t src_phy, src_span;
  uint32_t unif_span;
  uint32_t budget_ms;
  uint8_t tag;
} v3d_g2d_csd_job_t;

#define V3D_G2D_RUN_PRE (1u << 0)
#define V3D_G2D_RUN_POST (1u << 1)

/* identity arena 的 VA == PA，cache 接口直接按 PA 用 */
extern void foot_cache_clean(void* start, void* end);

/* ---- 由 v3d_g2d.c 提供 ---- */
int v3d_g2d_ready(void);
volatile uint32_t* v3d_g2d_hub_regs(void);
volatile uint32_t* v3d_g2d_core_regs(void);
void v3d_g2d_op_begin(void);
void v3d_g2d_op_end(void);
int v3d_g2d_phy_valid(uint32_t iova, size_t len);
int v3d_g2d_run_csd(const v3d_g2d_csd_job_t* job, unsigned flags);
void v3d_g2d_invalidate_span(uint32_t iova, size_t len);
void v3d_g2d_l2_invalidate_all(void);   /* PRE：整 L2 失效 */
int v3d_g2d_wedged(void);
void v3d_g2d_recover(const char* why);
int v3d_g2d_mmu_fault_peek(uint32_t* mctl, uint32_t* mvio,
                          uint32_t* mid, uint32_t* mint);
unsigned v3d_g2d_num_slices(void);

/* identity 映射的 DMA 分配（替代 EwokOS 的 dma_alloc） */
ewokos_addr_t v3d_dma_alloc(unsigned align, size_t size);
void v3d_dma_free(ewokos_addr_t pa);
/* GPU VA（iova）→ CPU 可访问地址换算 */
void* v3d_dma_cpu(uint32_t iova);
uint64_t v3d_tic_ms(void);

#endif /* V3D_3D_COMPAT_H */
