/*******************************************************************
 * V3D CSD 2D 操作层（fill / blit / alpha）。
 *
 * 【为什么用 CSD 而不是 TFU】TFU 只能做"同格式拷贝/填充"，没有 alpha
 * 混合；而 CSD 是 QPU 跑 compute shader，能做任意逐像素运算（EwokOS 的
 * 17 个 2D 内核在 Pi5 真机验证通过，桌面合成器在用）。
 *
 * 【像素格式】内核全部按 ARGB8888（4 字节/像素）编写 —— uniform 里的
 * stride / 偏移都乘 4。目标 BO 必须是 ARGB8888。上屏时的 16bpp 转换是
 * 另一层的事（见设计文档），不影响本层的能力验证。
 ********************************************************************/
#ifndef V3D_G2D_H
#define V3D_G2D_H

#include <stdint.h>

#include "v3d_hw.h"

/* 初始化：分配 kernel/uniform/scratch 的物理内存，拷入内核 blob，
 * 逐页映射到 GPU VA。返回 0 成功。 */
int v3d_g2d_init(v3d_dev_t* v);

/* ARGB8888 矩形填充。[x0,x1) × [y0,y1) 是裁剪后的目标矩形（像素）。 */
int v3d_g2d_fill(v3d_dev_t* v, uint32_t dst_gva, uint32_t w, uint32_t h,
                 uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                 uint32_t color);

/* ARGB8888 位块拷贝（src → dst，同尺寸）。先验证 fill 后再启用。 */
int v3d_g2d_blit(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva, uint32_t w,
                 uint32_t h);

/* ARGB8888 alpha 混合（src 叠到 dst，同尺寸）。 */
int v3d_g2d_alpha(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva, uint32_t w,
                  uint32_t h, uint32_t alpha);

/* 已初始化的 QPU 数（CSD cfg[4] 用）。 */
int v3d_g2d_num_qpus(void);

#endif /* V3D_G2D_H */
