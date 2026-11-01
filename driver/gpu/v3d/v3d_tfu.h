/*******************************************************************
 * V3D TFU（Texture Formatting Unit）填充 / 拷贝（用户态 foot 服务侧，P3）。
 *
 * V3D 7.1 的 TFU 支持 stride-0 源：把源的第一行广播到所有输出行 ⇒
 * 纯色 / 图案填充不需要 shader、不需要命令流，只写 9 个寄存器。
 * 寄存器在 hub 窗口（0x700 起），对照 Linux v3d_sched.c: v3d_tfu_job_run()。
 ********************************************************************/
#ifndef V3D_TFU_H
#define V3D_TFU_H

#include <stdint.h>

#include "v3d_hw.h"

/* 通用 TFU raster→raster 运行（内部）。
 *   src_stride_pixels  源每行像素数（0 = 广播第一行，即 fill）
 *   dst_stride_pixels  目标每行像素数
 *   tex_type           Texture Data Format（RGBA8UI=101, RGB565=6） */
int v3d_tfu_fill_ex(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                    uint32_t width, uint32_t height,
                    uint32_t dst_stride_pixels, uint32_t tex_type);

/* 用 RGBA8UI（每像素 4 字节）填充：把 src 的第一行广播到 width × height。 */
int v3d_tfu_fill(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                 uint32_t width, uint32_t height);

/* 通用 raster→raster 拷贝（RGBA8UI）。 */
int v3d_tfu_copy(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                 uint32_t width, uint32_t height, uint32_t src_stride_pixels,
                 uint32_t dst_stride_pixels);

/* 带像素格式的 raster→raster 拷贝（tex_type = Texture Data Format，
 * 源/目标同格式；如 RGB565=6、RGBA8UI=101）。 */
int v3d_tfu_copy_ex(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                    uint32_t width, uint32_t height,
                    uint32_t src_stride_pixels, uint32_t dst_stride_pixels,
                    uint32_t tex_type);

/* 读 TFU 控制/状态寄存器。 */
uint32_t v3d_tfu_status(v3d_dev_t* v);

#endif /* V3D_TFU_H */
