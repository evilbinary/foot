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

/* ARGB8888 位块拷贝（src → dst，同尺寸）。
 * wg_x：NUM_WGS_X（0 = 用 QPU 数）。copy 内核与 fill 的 band 划分方式不同，
 * 两种取值都试一遍才能确定它按 workgroup id 还是 batch id 定位。 */
int v3d_g2d_blit(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva, uint32_t w,
                 uint32_t h, uint32_t wg_x, uint32_t src_pa);

/* ARGB8888 alpha 混合（src 叠到 dst，同尺寸）。 */
int v3d_g2d_alpha(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva, uint32_t w,
                  uint32_t h, uint32_t alpha);

/* CPU source-over 混合（alpha 的可用实现）。
 * QPU 的 TMU 读在真机上不可用、TFU 不支持混合，所以半透明合成走这里。
 * 参数是 CPU 可访问的指针（同一块 BO 的 CPU 映射）。
 * alpha: 0..256（256 = 全不透明）。 */
void v3d_g2d_alpha_cpu(uint32_t* dst, const uint32_t* src, uint32_t w,
                       uint32_t h, uint32_t alpha);

/* 最小 TMU 读验证：按正确的 Texture/Sampler State 读源图像 (0,0) 像素，
 * 再写到 dst_gva 起始处。用来确认 descriptor 布局是否正确 ——
 * 只要 dst 首像素变成源图颜色，说明 TMU 读通路建立成功。 */
/* 对照内核：只把 color 写到 dst_gva 起点，不读源。 */
/* 最小直接地址读验证：从 src 读一个字写到 dst。 */
int v3d_g2d_minicopy(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva);
int v3d_g2d_minicopy2(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva);

/* 原版 argb_copy，按 EwokOS 探针的 ABI 运行（10 格 uniform + scratch）。 */
int v3d_g2d_copy_orig(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                      uint32_t span_bytes, uint32_t rows);

/* GPU 版 blit：1:1 拷贝（自产内核，TMU 字节地址读）。 */
int v3d_g2d_blit_gpu(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                     uint32_t w, uint32_t h);

/* GPU 版 source-over 混合。a: 0..256。 */
int v3d_g2d_present_gpu(v3d_dev_t* v, uint32_t src_gva,
                        uint32_t dst_gva, uint32_t w,
                        uint32_t h);
int v3d_g2d_alpha_gpu(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                      uint32_t w, uint32_t h, uint32_t a);

int v3d_g2d_miniwrite(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                      uint32_t color);

int v3d_g2d_miniread(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                     uint32_t src_w, uint32_t src_h);

/* 已初始化的 QPU 数（CSD cfg[4] 用）。 */
int v3d_g2d_num_qpus(void);

/* 已移除最小对照实验（v3d_g2d_probe）：它证明 QPU 能发 batch 后完成了
 * 使命；CSD 2D 现已通过 argb_fill 落像素验证（256×256 全绿）。 */

#endif /* V3D_G2D_H */
