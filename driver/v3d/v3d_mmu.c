/*******************************************************************
 * V3D MMU 编程 + L2C 使能（用户态 foot 服务侧，P2）。
 *
 * setup 序列对照 Linux drm/v3d/v3d_mmu.c: v3d_mmu_set_page_table()：
 *   1) MMU_PT_PA_BASE = pt_pa >> 12              （单级页表物理基址，写 PFN）
 *   2) MMU_CTL        = ENABLE | PT_INVALID_ENABLE/ABORT/INT
 *                             | WRITE_VIOLATION_ABORT/INT
 *                             | CAP_EXCEEDED_ABORT/INT
 *   3) MMU_ILLEGAL_ADDR = (scratch_pa >> 12) | ENABLE
 *   4) MMUC_CONTROL   = ENABLE
 *   5) flush：MMUC_CONTROL = FLUSH|ENABLE，等 FLUSHING 落回 0；
 *             MMU_CTL |= TLB_CLEAR，等 TLB_CLEARING 落回 0。
 *
 * 页表项格式：[27:0]=PFN(pa>>12)，bit28=VALID，bit29=WRITEABLE。
 ********************************************************************/
#include "v3d_mmu.h"

#include <unistd.h>

#include "v3d_regs.h"

/* 寄存器按 word 索引访问：hub + off/4 */
static inline uint32_t rd32(volatile uint32_t* p) {
  uint32_t v = *p;
  __asm__ volatile("dmb sy" ::: "memory");
  return v;
}

static inline void wr32(volatile uint32_t* p, uint32_t v) {
  *p = v;
  __asm__ volatile("dmb sy" ::: "memory");
}

/* 轮询直到 (reg & mask) == expect；超时返回 -1。tries*100us 为墙钟上限。 */
static int wait_bits(volatile uint32_t* reg, uint32_t mask, uint32_t expect,
                     int tries) {
  for (int i = 0; i < tries; i++) {
    if ((rd32(reg) & mask) == expect) {
      return 0;
    }
    usleep(100);
  }
  return -1;
}

int v3d_mmu_flush(v3d_dev_t* v) {
  if (v == NULL || v->hub == NULL) {
    return -1;
  }
  volatile uint32_t* mmuc = v->hub + V3D_MMUC_CONTROL / 4;
  volatile uint32_t* ctl = v->hub + V3D_MMU_CTL / 4;

  /* 1) MMUC flush：写 FLUSH|ENABLE，等 FLUSHING 位落回 0 */
  wr32(mmuc, V3D_MMUC_CONTROL_FLUSH | V3D_MMUC_CONTROL_ENABLE);
  if (wait_bits(mmuc, V3D_MMUC_CONTROL_FLUSHING, 0, 100) != 0) {
    return -1;
  }

  /* 2) TLB clear：MMU_CTL |= TLB_CLEAR，等 TLB_CLEARING 落回 0 */
  wr32(ctl, rd32(ctl) | V3D_MMU_CTL_TLB_CLEAR);
  if (wait_bits(ctl, V3D_MMU_CTL_TLB_CLEARING, 0, 100) != 0) {
    return -2;
  }
  return 0;
}

int v3d_mmu_setup(v3d_dev_t* v, volatile uint32_t* pt, uint32_t pt_pa,
                  uint32_t scratch_pa) {
  if (v == NULL || v->hub == NULL || pt == NULL || pt_pa == 0) {
    return -1;
  }

  /* 页表内容（清零 / 填 PTE）由调用方准备，并确保已 cache clean 到 DRAM
   * 再调本函数——否则 GPU MMU 读到的可能是脏数据。pt 仅作接口校验。 */
  (void)pt;

  /* 1) 页表物理基址（写 PFN，不是字节地址） */
  wr32(v->hub + V3D_MMU_PT_PA_BASE / 4, pt_pa >> V3D_MMU_PAGE_SHIFT);

  /* 3) 未映射访问兜底到 scratch 页，避免真的打到任意物理内存 */
  if (scratch_pa != 0) {
    wr32(v->hub + V3D_MMU_ILLEGAL_ADDR / 4,
         (scratch_pa >> V3D_MMU_PAGE_SHIFT) | V3D_MMU_ILLEGAL_ADDR_ENABLE);
  }

  /* 4) 打开 MMU；各类 fault 都置 abort + int（与 Linux 一致） */
  wr32(v->hub + V3D_MMU_CTL / 4,
       V3D_MMU_CTL_ENABLE | V3D_MMU_CTL_PT_INVALID_ENABLE |
           V3D_MMU_CTL_PT_INVALID_ABORT | V3D_MMU_CTL_PT_INVALID_INT |
           V3D_MMU_CTL_WRITE_VIOLATION_ABORT |
           V3D_MMU_CTL_WRITE_VIOLATION_INT | V3D_MMU_CTL_CAP_EXCEEDED_ABORT |
           V3D_MMU_CTL_CAP_EXCEEDED_INT);

  /* 5) 使能 MMU 控制器 */
  wr32(v->hub + V3D_MMUC_CONTROL / 4, V3D_MMUC_CONTROL_ENABLE);

  /* 6) flush + TLB clear */
  return v3d_mmu_flush(v);
}

void v3d_mmu_map_page(volatile uint32_t* pt, uint32_t va, uint32_t pa) {
  if (pt == NULL) {
    return;
  }
  uint32_t idx = (va >> V3D_MMU_PAGE_SHIFT) & (V3D_MMU_PT_ENTRIES - 1);
  pt[idx] = ((pa >> V3D_MMU_PAGE_SHIFT) & V3D_PTE_PFN_MASK) |
            V3D_PTE_WRITEABLE | V3D_PTE_VALID;
  __asm__ volatile("dmb sy" ::: "memory");
}

uint32_t v3d_mmu_ctl(v3d_dev_t* v) {
  return (v != NULL && v->hub != NULL) ? rd32(v->hub + V3D_MMU_CTL / 4) : 0;
}

uint32_t v3d_mmuc_control(v3d_dev_t* v) {
  return (v != NULL && v->hub != NULL) ? rd32(v->hub + V3D_MMUC_CONTROL / 4) : 0;
}

uint32_t v3d_mmu_debug_info(v3d_dev_t* v) {
  return (v != NULL && v->hub != NULL)
             ? rd32(v->hub + V3D_MMU_DEBUG_INFO / 4)
             : 0;
}

uint32_t v3d_mmu_vio_addr(v3d_dev_t* v) {
  return (v != NULL && v->hub != NULL) ? rd32(v->hub + V3D_MMU_VIO_ADDR / 4) : 0;
}

uint32_t v3d_mmu_vio_id(v3d_dev_t* v) {
  return (v != NULL && v->hub != NULL) ? rd32(v->hub + V3D_MMU_VIO_ID / 4) : 0;
}

int v3d_l2c_enable(v3d_dev_t* v) {
  if (v == NULL || v->core0 == NULL) {
    return -1;
  }
  volatile uint32_t* l2c = v->core0 + V3D_CTL_L2CACTL / 4;

  /* 先清：写 L2CCLR，等它自清 */
  wr32(l2c, V3D_L2CACTL_L2CCLR);
  if (wait_bits(l2c, V3D_L2CACTL_L2CCLR, 0, 100) != 0) {
    return -2;
  }
  /* 再使能，读回确认 */
  wr32(l2c, V3D_L2CACTL_L2CENA);
  return (rd32(l2c) & V3D_L2CACTL_L2CENA) ? 0 : -3;
}

uint32_t v3d_l2c_status(v3d_dev_t* v) {
  return (v != NULL && v->core0 != NULL)
             ? rd32(v->core0 + V3D_CTL_L2CACTL / 4)
             : 0;
}
