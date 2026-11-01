/*******************************************************************
 * V3D TFU（Texture Formatting Unit）填充 / 拷贝（用户态 foot 服务侧，P3）。
 *
 * 提交序列对照 Linux drm/v3d/v3d_sched.c: v3d_tfu_job_run()：
 *   WRITE(IIA) WRITE(IIS) WRITE(ICA) WRITE(IUA) WRITE(IOA)
 *   if (ver >= 71) WRITE(V7_TFU_IOC)
 *   WRITE(IOS) WRITE(COEF0..3)
 *   WRITE(ICFG, icfg | IOC)     ← ICFG 写下去即触发
 *
 * 参数构造对照 Mesa v3dvx_meta_common.c: meta_emit_tfu_job()（RASTER 分支）：
 *   ios  = (height << 16) | width
 *   iis  = src_stride_bytes / cpp        （0 = 广播第一行）
 *   ioc  = (RASTER << 12) | (dst_stride_bytes / cpp << 16)
 *   icfg = (RASTER << 23) | (tex_type << 16)   （tex_type: RGBA8UI = 101）
 ********************************************************************/
#include "v3d_tfu.h"

#include <unistd.h>

#include "v3d_regs.h"

static inline uint32_t rd32(volatile uint32_t* p) {
  uint32_t v = *p;
  __asm__ volatile("dmb sy" ::: "memory");
  return v;
}

static inline void wr32(volatile uint32_t* p, uint32_t v) {
  *p = v;
  __asm__ volatile("dmb sy" ::: "memory");
}

int v3d_tfu_copy(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                 uint32_t width, uint32_t height, uint32_t src_stride_pixels,
                 uint32_t dst_stride_pixels) {
  if (v == NULL || v->hub == NULL || width == 0 || height == 0) {
    return -1;
  }
  volatile uint32_t* hub = v->hub;

  /* 输入：地址 + stride（像素）；chroma / U-plane 不用置 0 */
  wr32(hub + V3D_TFU_IIA / 4, src_gva);
  wr32(hub + V3D_TFU_IIS / 4, src_stride_pixels);
  wr32(hub + V3D_TFU_ICA / 4, 0);
  wr32(hub + V3D_TFU_IUA / 4, 0);

  /* 输出：地址 + 配置（RASTER 格式 + 像素 stride） */
  wr32(hub + V3D_TFU_IOA / 4, dst_gva);
  wr32(hub + V3D_V7_TFU_IOC / 4,
       (V3D_V7_TFU_IOC_FORMAT_RASTER << V3D_V7_TFU_IOC_FORMAT_SHIFT) |
           (dst_stride_pixels << V3D_V7_TFU_IOC_STRIDE_SHIFT));

  /* 输出尺寸 + YUV 系数（不用置 0） */
  wr32(hub + V3D_TFU_IOS / 4, (height << 16) | (width & 0xFFFFu));
  wr32(hub + V3D_TFU_COEF0 / 4, 0);
  wr32(hub + V3D_TFU_COEF1 / 4, 0);
  wr32(hub + V3D_TFU_COEF2 / 4, 0);
  wr32(hub + V3D_TFU_COEF3 / 4, 0);

  /* ICFG 触发：iformat = RASTER(0)，otype = RGBA8UI(101)，IOC = 1 */
  wr32(hub + V3D_TFU_ICFG / 4,
       (V3D_TFU_ICFG_FORMAT_RASTER << V3D_TFU_ICFG_IFORMAT_SHIFT) |
           (V3D_TEX_FORMAT_RGBA8UI << V3D_TFU_ICFG_OTYPE_SHIFT) |
           V3D_TFU_ICFG_IOC);

  /* 等 TFU 空闲（BUSY 清零） */
  for (int i = 0; i < 1000; i++) {
    if (!(rd32(hub + V3D_TFU_CS / 4) & V3D_TFU_CS_BUSY)) {
      return 0;
    }
    usleep(1000);
  }
  return -2;
}

int v3d_tfu_fill(v3d_dev_t* v, uint32_t src_gva, uint32_t dst_gva,
                 uint32_t width, uint32_t height) {
  /* src stride = 0：TFU 把第一行广播到所有输出行 */
  return v3d_tfu_copy(v, src_gva, dst_gva, width, height, 0, width);
}

uint32_t v3d_tfu_status(v3d_dev_t* v) {
  return (v != NULL && v->hub != NULL) ? rd32(v->hub + V3D_TFU_CS / 4) : 0;
}
