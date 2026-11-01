/*******************************************************************
 * V3D 后端实现（原 v3dd.c 拆分而来）。
 *
 * 【定位】本文件是【库】，不是可执行进程：
 *   - 提供 v3d_init()（capability 映射 + SMS/PM/MMU/L2C + 屏幕初始化）
 *   - 提供 gpu_backend_t v3d_backend（FILL/BLIT/SYNC）
 *   - 服务注册与 client_run 在 foot/service/gpud.c
 *
 * 拆分前 v3dd.c 是"独立服务进程"，拆分后：
 *   gpud（独立进程，注册 "gpu"） → v3d_backend（库） → V3D 硬件
 *
 * 阶段：
 *   P1 窗口映射 + HUB_IDENT0 + SMS 上电 + PM 复位
 *   P2 MMU（4MB 单级页表）+ L2C
 *   P3 CSD 执行 + TFU 硬件填充 + 屏幕输出
 ********************************************************************/
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "client.h"
#include "gpu_backend.h"
#include "v3d_backend.h"
#include "v3d_csd.h"
#include "v3d_hw.h"
#include "v3d_mmu.h"
#include "v3d_regs.h"
#include "v3d_tfu.h"

/* /dev/fb 的 ioctl（与 duck/modules/vga + eggs/libgui/screen.h 一致） */
#define FB_IOC_MAGIC 'v'
#define IOC_READ_FRAMBUFFER _IOW(FB_IOC_MAGIC, 3, int)
#define IOC_READ_FRAMBUFFER_WIDTH _IOW(FB_IOC_MAGIC, 4, int)
#define IOC_READ_FRAMBUFFER_HEIGHT _IOW(FB_IOC_MAGIC, 5, int)
#define IOC_READ_FRAMBUFFER_BPP _IOW(FB_IOC_MAGIC, 6, int)
#define IOC_READ_FRAMBUFFER_FORMAT _IOW(FB_IOC_MAGIC, 9, int)

/* 显示格式常量（与内核 vga.h 的 VGA_FMT_* 对齐） */
#define FB_FMT_UNKNOWN 0
#define FB_FMT_RGB555 1
#define FB_FMT_RGB565 2
#define FB_FMT_BGR565 3
#define FB_FMT_ARGB8888 4

/* 映射目标 VA：页对齐、远低于用户代码段（EXEC_ADDR），落在 MMIO 直通区。 */
#define V3D_MAP_VA 0x40000000UL
#define PM_MAP_VA 0x41000000UL
#define PT_VA 0x42000000UL      /* 4MB 单级页表映射区 */
#define SCRATCH_VA 0x43000000UL /* MMU 未映射访问的落点页 */
#define SH_VA 0x44000000UL      /* shader 代码页 CPU 映射 */
#define UNI_VA 0x44100000UL     /* uniform 页 CPU 映射 */
#define SHADER_GVA 0x10000000UL /* shader 的 GPU 虚拟地址 */
#define UNIFORM_GVA 0x10001000UL /* uniform 的 GPU 虚拟地址 */
#define SRC_VA 0x45000000UL     /* TFU 源 buffer CPU 映射 */
#define DST_VA 0x46000000UL     /* TFU 目标 buffer CPU 映射 */
#define SRC_GVA 0x10002000UL    /* TFU 源 GPU 虚拟地址 */
#define DST_GVA 0x10010000UL    /* TFU 目标 GPU 虚拟地址 */
#define FB_SRC_VA 0x47000000UL  /* 屏幕填充：RGB565 源行 CPU 映射 */
#define FB_SRC_GVA 0x10020000UL /* 屏幕填充：RGB565 源行 GPU 虚拟地址 */
#define FB_GVA 0x20000000UL     /* 屏幕 framebuffer 的 GPU 虚拟地址 */

static v3d_dev_t g_dev;
static uint32_t g_ident0;

/* ---- 屏幕状态（screen_init 填充，screen_fill 使用）---- */
static struct {
  int inited;
  uint32_t fb_pa;  /* framebuffer 物理地址 */
  uint32_t width;  /* 像素宽 */
  uint32_t height; /* 像素高 */
  uint32_t cpp;    /* 每像素字节数 */
  uint32_t tex;    /* V3D Texture Data Format */
  uint32_t pitch;  /* 每行字节数 */
  uint32_t gpu_va; /* framebuffer 的 GPU 虚拟地址 */
} g_screen;

static int screen_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t color);

/* 初始化屏幕：拿 /dev/fb 信息、映射 framebuffer 到 GPU VA、准备源行 buffer。 */
static int screen_init(void) {
  int fd = open("/dev/fb", O_RDWR);
  if (fd < 0) {
    printf("v3d: /dev/fb open failed\n");
    return -1;
  }
  uint32_t fb_pa = (uint32_t)ioctl(fd, IOC_READ_FRAMBUFFER, 0);
  uint32_t w = (uint32_t)ioctl(fd, IOC_READ_FRAMBUFFER_WIDTH, 0);
  uint32_t h = (uint32_t)ioctl(fd, IOC_READ_FRAMBUFFER_HEIGHT, 0);
  uint32_t bpp = (uint32_t)ioctl(fd, IOC_READ_FRAMBUFFER_BPP, 0);
  uint32_t fmt = (uint32_t)ioctl(fd, IOC_READ_FRAMBUFFER_FORMAT, 0);
  close(fd);

  /* VGA_FMT_* → (V3D Texture Data Format, 每像素字节) */
  uint32_t tex = 0, cpp = 0;
  int ok = 0;
  switch (fmt) {
    case FB_FMT_RGB555:
      tex = 8; /* RGB5_A1 */
      cpp = 2;
      ok = 1;
      break;
    case FB_FMT_RGB565:
    case FB_FMT_BGR565:
      tex = V3D_TEX_FORMAT_RGB565;
      cpp = 2;
      ok = 1;
      break;
    case FB_FMT_ARGB8888:
      tex = 4; /* RGBA8 */
      cpp = 4;
      ok = 1;
      break;
    default:
      break;
  }
  if (!ok && fmt == FB_FMT_UNKNOWN) {
    if (bpp == 16) {
      tex = V3D_TEX_FORMAT_RGB565;
      cpp = 2;
      ok = 1;
    } else if (bpp == 32) {
      tex = 4;
      cpp = 4;
      ok = 1;
    }
  }
  if (fb_pa == 0 || w == 0 || h == 0 || !ok) {
    printf("v3d: screen unsupported (pa=0x%x %ux%u bpp=%u fmt=%u)\n", fb_pa, w,
           h, bpp, fmt);
    return -1;
  }

  /* 源行 buffer（一页） */
  uint32_t src_pa = 0, src_size = 0;
  foot_cap_t sc =
      foot_cap_alloc_arena(V3D_MMU_PAGE_SIZE, 0, 0, &src_pa, &src_size);
  if (sc == FOOT_CAP_NONE ||
      foot_cap_map(sc, (void*)FB_SRC_VA, 0, src_size) != 0) {
    printf("v3d: screen src alloc/map failed\n");
    return -1;
  }

  volatile uint32_t* pt = (volatile uint32_t*)PT_VA;
  v3d_mmu_map_page(pt, FB_SRC_GVA, src_pa);

  /* framebuffer 逐页映射到 GPU VA（pitch 假定 = width * cpp） */
  uint32_t pitch = w * cpp;
  uint32_t fb_bytes = pitch * h;
  uint32_t pages = (fb_bytes + V3D_MMU_PAGE_SIZE - 1) / V3D_MMU_PAGE_SIZE;
  for (uint32_t i = 0; i < pages; i++) {
    v3d_mmu_map_page(pt, FB_GVA + i * V3D_MMU_PAGE_SIZE,
                     fb_pa + i * V3D_MMU_PAGE_SIZE);
  }
  foot_cache_clean((void*)pt, (void*)(PT_VA + V3D_MMU_PT_BYTES));
  v3d_mmu_flush(&g_dev);

  g_screen.fb_pa = fb_pa;
  g_screen.width = w;
  g_screen.height = h;
  g_screen.cpp = cpp;
  g_screen.tex = tex;
  g_screen.pitch = pitch;
  g_screen.gpu_va = FB_GVA;
  g_screen.inited = 1;

  printf("v3d: screen %ux%u bpp=%u fmt=%u cpp=%u tex=%u pa=0x%08x pages=%u\n", w,
         h, bpp, fmt, cpp, tex, fb_pa, pages);
  return 0;
}

/* 填一个矩形区域。color 按屏幕格式解释（RGB565 只取低 16 位）。 */
static int screen_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t color) {
  if (!g_screen.inited || w == 0 || h == 0) {
    return -1;
  }
  if (x + w > g_screen.width || y + h > g_screen.height) {
    return -2;
  }
  uint32_t max_px = V3D_MMU_PAGE_SIZE / g_screen.cpp;
  if (w > max_px) {
    return -3;
  }

  volatile uint32_t* s32 = (volatile uint32_t*)FB_SRC_VA;
  uint32_t pat;
  if (g_screen.cpp == 4) {
    pat = color;
  } else {
    uint32_t c16 = color & 0xFFFFu;
    pat = c16 | (c16 << 16);
  }
  uint32_t bytes = w * g_screen.cpp;
  uint32_t nw = (bytes + 3) / 4;
  for (uint32_t i = 0; i < nw; i++) {
    s32[i] = pat;
  }
  foot_cache_clean((void*)s32, (void*)(FB_SRC_VA + V3D_MMU_PAGE_SIZE));

  uint32_t dst_gva = g_screen.gpu_va + y * g_screen.pitch + x * g_screen.cpp;
  uint32_t stride_px = g_screen.pitch / g_screen.cpp;
  return v3d_tfu_fill_ex(&g_dev, FB_SRC_GVA, dst_gva, w, h, stride_px,
                         g_screen.tex);
}

/* ================== gpu_backend_t 实现 ================== */

static int v3d_init(void) {
  /* 1) 映射 V3D 寄存器窗口（hub + core0 + SMS，0x40000） */
  foot_cap_t cap = foot_cap_get_mmio("v3d");
  if (cap == FOOT_CAP_NONE) {
    printf("v3d: foot_cap_get_mmio(v3d) failed\n");
    return -1;
  }
  if (foot_cap_map(cap, (void*)V3D_MAP_VA, 0, 0x40000) != 0) {
    printf("v3d: foot_cap_map(v3d) failed\n");
    return -1;
  }
  volatile uint32_t* v3d_va = (volatile uint32_t*)V3D_MAP_VA;

  /* 2) 映射 PM GRAFX 电源域（V3DRSTN 复位用，1 页） */
  foot_cap_t pmcap = foot_cap_get_mmio("pm-grafx");
  if (pmcap == FOOT_CAP_NONE) {
    printf("v3d: foot_cap_get_mmio(pm-grafx) failed\n");
    return -1;
  }
  if (foot_cap_map(pmcap, (void*)PM_MAP_VA, 0, 0x1000) != 0) {
    printf("v3d: foot_cap_map(pm-grafx) failed\n");
    return -1;
  }

  /* 3) 组装设备视图 */
  g_dev.hub = v3d_va + V3D_HUB_OFF / 4;
  g_dev.core0 = v3d_va + V3D_CORE0_OFF / 4;
  g_dev.sms = v3d_va + V3D_SMS_OFF / 4;
  g_dev.pm = (volatile uint32_t*)PM_MAP_VA;

  /* 4) P0：探活 */
  g_ident0 = *(volatile uint32_t*)(g_dev.hub + V3D_HUB_IDENT0 / 4);
  printf("v3d: HUB_IDENT0 = 0x%08x (%c%c%c%c)\n", g_ident0,
         (char)(g_ident0 & 0xff), (char)((g_ident0 >> 8) & 0xff),
         (char)((g_ident0 >> 16) & 0xff), (char)((g_ident0 >> 24) & 0xff));
  if (v3d_probe(&g_dev) != 0) {
    printf("v3d: V3D not detected, abort\n");
    return -1;
  }

  /* 5) P1：SMS 上电 + PM 复位 */
  v3d_sms_powerup(&g_dev);
  v3d_pm_reset(&g_dev);

  /* 6) P1：解析 IDENT1/IDENT3 */
  uint32_t ver = 0, ncores = 0, iprev = 0;
  v3d_ident(&g_dev, &ver, &ncores, &iprev);
  printf("v3d: ident ver=%u ncores=%u iprev=%u\n", ver, ncores, iprev);

  /* 7) P2：MMU（单级 4MB 页表）+ L2C */
  uint32_t pt_pa = 0, pt_size = 0;
  uint32_t scratch_pa = 0, scratch_size = 0;
  foot_cap_t ptcap =
      foot_cap_alloc_arena(V3D_MMU_PT_BYTES, 0, 0, &pt_pa, &pt_size);
  if (ptcap == FOOT_CAP_NONE) {
    printf("v3d: alloc page table failed (want %u bytes)\n",
           (unsigned)V3D_MMU_PT_BYTES);
    return -1;
  }
  if (foot_cap_map(ptcap, (void*)PT_VA, 0, pt_size) != 0) {
    printf("v3d: map page table failed\n");
    return -1;
  }

  foot_cap_t sc = foot_cap_alloc_arena(V3D_MMU_PAGE_SIZE, 0, 0, &scratch_pa,
                                       &scratch_size);
  if (sc == FOOT_CAP_NONE ||
      foot_cap_map(sc, (void*)SCRATCH_VA, 0, scratch_size) != 0) {
    printf("v3d: alloc/map scratch failed, ILLEGAL_ADDR disabled\n");
    scratch_pa = 0;
  }

  volatile uint32_t* pt = (volatile uint32_t*)PT_VA;
  for (uint32_t i = 0; i < V3D_MMU_PT_ENTRIES; i++) {
    pt[i] = 0;
  }
  foot_cache_clean((void*)pt, (void*)(PT_VA + V3D_MMU_PT_BYTES));

  int mrc = v3d_mmu_setup(&g_dev, pt, pt_pa, scratch_pa);
  printf("v3d: MMU setup rc=%d pt_pa=0x%08x scratch_pa=0x%08x\n", mrc, pt_pa,
         scratch_pa);

  int lrc = v3d_l2c_enable(&g_dev);
  printf("v3d: L2C enable rc=%d l2cactl=0x%08x\n", lrc,
         v3d_l2c_status(&g_dev));

  /* 8) 屏幕初始化（不自动填色，避免盖住现有 UI；由应用 FILL 驱动） */
  screen_init();

  printf("v3d: backend ready (ver=%u ncores=%u)\n", ver, ncores);
  return 0;
}

/* 返回值：(screen_w << 16) | (screen_h & 0xFFFF)。
 * 【为什么不用 gpu_info_t 输出】foot 的 client_call 把 args 复制到共享页、
 * 语义是"只读输入"（client_call_ex 的 args 是 const，服务端改共享页后
 * 客户端拿不到）。完整信息（backend_id/flags）留到阶段 2 用共享内存传。 */
static int v3d_query(void* args) {
  (void)args;
  if (!g_screen.inited) {
    return 0;
  }
  return (int)((g_screen.width << 16) | (g_screen.height & 0xFFFF));
}

static int v3d_fill(void* args) {
  gpu_fill_req* r = (gpu_fill_req*)args;
  if (r == NULL) {
    return -1;
  }
  return screen_fill(r->x, r->y, r->w, r->h, r->color);
}

/* ================== 低层：BO 管理 ==================
 * buffer 由应用/Mesa 自己分配（foot_cap_alloc_frame 拿 pa），BO_CREATE 把它
 * 逐页映射进 GPU VA 空间并返回句柄；BO_OFFSET 拿 GPU VA（命令流里拼地址）；
 * BO_DESTROY 清页表项。CPU 映射始终在应用侧，gpud 只管 GPU 侧。 */
#define V3D_MAX_BOS 32
#define V3D_BO_GVA_BASE 0x30000000UL /* 应用 buffer 区（避开屏幕/shader 区） */

static struct {
  uint32_t gva;
  uint32_t pa;
  uint32_t size;
  int in_use;
} g_bos[V3D_MAX_BOS];
static uint32_t g_bo_next_gva = V3D_BO_GVA_BASE;

static int v3d_bo_create(void* args) {
  gpu_bo_import_req* r = (gpu_bo_import_req*)args;
  if (r == NULL || r->pa == 0 || r->size == 0) {
    return -1;
  }
  if ((r->pa & 0xFFFu) || (r->size & 0xFFFu)) {
    return -2; /* 必须页对齐 */
  }
  int slot = -1;
  for (int i = 0; i < V3D_MAX_BOS; i++) {
    if (!g_bos[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    return -3; /* 句柄耗尽 */
  }

  uint32_t gva = g_bo_next_gva;
  g_bo_next_gva += r->size;
  if (g_bo_next_gva > 0x40000000UL) {
    return -4; /* GPU VA 区耗尽（0x30000000-0x40000000 共 256MB） */
  }

  volatile uint32_t* pt = (volatile uint32_t*)PT_VA;
  uint32_t pages = r->size / V3D_MMU_PAGE_SIZE;
  for (uint32_t i = 0; i < pages; i++) {
    v3d_mmu_map_page(pt, gva + i * V3D_MMU_PAGE_SIZE,
                     r->pa + i * V3D_MMU_PAGE_SIZE);
  }
  foot_cache_clean((void*)pt, (void*)(PT_VA + V3D_MMU_PT_BYTES));
  v3d_mmu_flush(&g_dev);

  g_bos[slot].gva = gva;
  g_bos[slot].pa = r->pa;
  g_bos[slot].size = r->size;
  g_bos[slot].in_use = 1;
  return slot + 1; /* handle = slot+1（0 保留为"无"） */
}

static int v3d_bo_offset(void* args) {
  gpu_bo_ref* r = (gpu_bo_ref*)args;
  if (r == NULL || r->handle == 0 || r->handle > V3D_MAX_BOS) {
    return -1;
  }
  if (!g_bos[r->handle - 1].in_use) {
    return -2;
  }
  return (int)g_bos[r->handle - 1].gva;
}

static int v3d_bo_destroy(void* args) {
  gpu_bo_ref* r = (gpu_bo_ref*)args;
  if (r == NULL || r->handle == 0 || r->handle > V3D_MAX_BOS) {
    return -1;
  }
  int slot = r->handle - 1;
  if (!g_bos[slot].in_use) {
    return -2;
  }
  /* 清页表项（VA 段不回收，bump 分配；进程生命周期内够用） */
  volatile uint32_t* pt = (volatile uint32_t*)PT_VA;
  uint32_t pages = g_bos[slot].size / V3D_MMU_PAGE_SIZE;
  for (uint32_t i = 0; i < pages; i++) {
    v3d_mmu_map_page(pt, g_bos[slot].gva + i * V3D_MMU_PAGE_SIZE, 0);
  }
  foot_cache_clean((void*)pt, (void*)(PT_VA + V3D_MMU_PT_BYTES));
  v3d_mmu_flush(&g_dev);

  g_bos[slot].in_use = 0;
  return 0;
}

/* 按 BO 句柄提交 TFU 拷贝（src/dst 同格式，tex 取 dst）。
 * handle=0 表示屏幕；offset 是字节偏移（屏幕 = y*pitch + x*cpp）。 */
static int v3d_submit_tfu(void* args) {
  gpu_submit_tfu_req* r = (gpu_submit_tfu_req*)args;
  if (r == NULL || r->w == 0 || r->h == 0) {
    return -1;
  }

  uint32_t src_gva = 0, dst_gva = 0;
  uint32_t src_stride = 0, dst_stride = 0;
  uint32_t tex = 0;

  /* ---- 源 ---- */
  if (r->src_handle == 0) {
    if (!g_screen.inited) {
      return -2;
    }
    src_gva = FB_GVA + r->src_offset;
    src_stride = g_screen.pitch / g_screen.cpp;
  } else {
    int i = (int)r->src_handle - 1;
    if (i < 0 || i >= V3D_MAX_BOS || !g_bos[i].in_use) {
      return -2;
    }
    src_gva = g_bos[i].gva + r->src_offset;
    src_stride = r->w; /* 紧凑：每行 w 像素 */
  }

  /* ---- 目标 ---- */
  if (r->dst_handle == 0) {
    if (!g_screen.inited) {
      return -2;
    }
    dst_gva = FB_GVA + r->dst_offset;
    dst_stride = g_screen.pitch / g_screen.cpp;
    tex = g_screen.tex;
  } else {
    int i = (int)r->dst_handle - 1;
    if (i < 0 || i >= V3D_MAX_BOS || !g_bos[i].in_use) {
      return -2;
    }
    dst_gva = g_bos[i].gva + r->dst_offset;
    dst_stride = r->w;
    tex = (r->dst_fmt == GPU_FMT_RGB565) ? V3D_TEX_FORMAT_RGB565 : 4;
  }

  /* TFU 是异步 DMA，copy_ex 内部已轮询 BUSY 到 0（同步语义）。
   * GPU_SUBMIT_ASYNC 留给以后做 fence；当前实现仍同步返回。 */
  return v3d_tfu_copy_ex(&g_dev, src_gva, dst_gva, r->w, r->h, src_stride,
                         dst_stride, tex);
}

static int v3d_blit(void* args) {
  (void)args;
  /* TODO：阶段 2 用 TFU copy（src_stride != 0）实现 */
  return -1;
}

static int v3d_sync(void* args) {
  (void)args;
  /* FILL/BLIT 默认同步（内部已等 BUSY 清零），这里再确认一次 */
  if (v3d_tfu_status(&g_dev) & V3D_TFU_CS_BUSY) {
    return -2;
  }
  return 0;
}

const gpu_backend_t v3d_backend = {
    .name = "v3d",
    .backend_id = GPU_BACKEND_V3D,
    .init = v3d_init,
    .query = v3d_query,
    .fill = v3d_fill,
    .blit = v3d_blit,
    .sync = v3d_sync,
    /* 低层：BO 管理已实现；SUBMIT_CL/CSD 随阶段推进补 */
    .bo_create = v3d_bo_create,
    .bo_mmap = NULL, /* CPU 映射在应用侧（foot_cap_alloc_frame） */
    .bo_destroy = v3d_bo_destroy,
    .bo_offset = v3d_bo_offset,
    .submit_cl = NULL,
    .submit_csd = NULL,
    .submit_tfu = v3d_submit_tfu,
    .wait_bo = NULL,
};
