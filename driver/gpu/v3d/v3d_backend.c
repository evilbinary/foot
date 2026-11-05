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
#include <sys/mman.h>
#include <unistd.h>

#include "client.h"
#include "gpu_backend.h"
#include "v3d_backend.h"
#include "v3d_csd.h"
#include "v3d_g2d.h"
#include "v3d_hw.h"
#include "v3d_mbox.h"
#include "v3d_mmu.h"
#include "v3d_regs.h"
#include "v3d_tfu.h"
#include "v3d_3d.h"

#include <signal.h>
#include <unistd.h>

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
#define FB_GVA 0x20000000UL

/* 【启动自测开关】设为 1 时 gpud 启动后会在离屏 BO 上跑一整套 2D 验证。
 *
 * 默认 0：这些验证已经搬进 app/test/test-gpu.c（走 client_call 的端到端
 * 路径，才是真实用法）。留在 gpud 里有两个问题：
 *   1. 它和 test-gpu 抢同一个 GPU —— 实测两边交错，blit 前 16 个像素被
 *      对方覆盖（d0=0 而 d16 正确），PRESENT 还会 foot wait timeout；
 *   2. 每次起 gpud 都要多花几十毫秒。
 * v3d_g2d_init() 本身始终会调用（它负责加载内核与 arena），只是验证代码
 * 被这个开关包住了。 */
#define G2D_SELFTEST 0

/* 【3D 启动探针开关】默认 0：冷启动后的**第一次 render 提交必须是有效
 * clear**。
 *
 * 历史教训：probe_rcl() 的 probeA/B/C 都是残缺 render CL（单字节 END、
 * Common+END），它们在真机上不会产生 FRDONE/FRDone，也不会让 RFC 递增，
 * 但 wait_flush() 的 CTRUN 兜底会返回 rc=0。结果 probe 看似“通过”，
 * 实际已经把 CT1 的 job 槽位占住；紧接着的真实 clear 成为第 4 次提交，
 * 入队后 CT1 一个字节都不取（ct1ca +0、ct1cs 0x30）。参考仓库的结论也
 * 是「一个未 retire 的 render 会永久卡住 CLE，必须在 fresh boot 后第一次
 * 就提交有效 CL」。调试探针时再手动改成 1，且每次只开一个。 */
#define V3D_3D_STARTUP_PROBES 0

/* 前置声明：自测代码里要调用这些后面才定义的 static 函数 */
static int v3d_bo_create(void* args);
static int v3d_bo_destroy(void* args);
static int v3d_present(void* args);

/* 读 aarch64 物理计数器，用于性能测量（cntfrq_el0 是频率） */
static inline uint64_t g2d_rd_pct(void) {
  uint64_t v;
  __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
  return v;
}
static inline uint64_t g2d_rd_frq(void) {
  uint64_t v;
  __asm__ volatile("isb; mrs %0, cntfrq_el0" : "=r"(v));
  return v;
}     /* 屏幕 framebuffer 的 GPU 虚拟地址 */

/* CSD 2D 自测：256×256 ARGB8888 离屏 BO */
#define G2D_TEST_VA 0x49000000UL  /* 用户态映射 */
#define TRI_VA 0x4A000000UL       /* 3D 三角形测试的顶点缓冲映射 */

/* 3D 精简版（v3d_3d_min.c） */
int v3d_3d_init(void);
uint32_t v3d_3d_cl_base(void);
int v3d_3d_submit_cl(uint32_t bcl_start, uint32_t bcl_end,
                     uint32_t rcl_start, uint32_t rcl_end, uint32_t w,
                     uint32_t h);
int v3d_3d_probe_rcl(void);
int v3d_3d_probe_prefix(void);
int v3d_3d_probe_clear_steps(uint32_t fb_iova, uint32_t stride_bytes,
                             uint32_t fmt, uint32_t w, uint32_t h,
                             uint32_t rgba8);
int v3d_3d_clear_min(uint32_t fb_iova, uint32_t stride_bytes, uint32_t fmt,
                     uint32_t w, uint32_t h, uint32_t rgba8);
int v3d_3d_draw_min(uint32_t fb_iova, uint32_t stride_bytes, uint32_t fmt,
                    uint32_t w, uint32_t h, uint32_t mode, uint32_t nverts,
                    uint32_t pos_iova, uint32_t col_iova, uint32_t clear_rgba,
                    int first_frame);
#define G2D_TEST_GVA 0x31000000UL /* GPU 虚拟地址（避开 BO 0x30000000） */
#define G2D_TEST2_VA 0x4B000000UL /* 第二块离屏 BO（blit/alpha 的 dst）；
                                   * 避开 mailbox 的 0x4A000000 */
#define G2D_TEST2_GVA 0x32000000UL /* 其 GPU VA（避开 0x31000000） */

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

/* 【客观验收】mmap /dev/fb 读回几个像素，确认 3D clear 真的写进了显存。
 * 远端调试看不到屏幕，必须用像素值把「rc=0」和「真的写了」区分开。
 * 清屏区域是屏幕左上 640x480，所以 (0,0)/(320,240)/(639,479) 应该变成
 * 纯色，而 (700,500) 在区域外，作为对照。 */
static void fb_readback_probe(const char *tag) {
  if (!g_screen.inited || g_screen.cpp != 2) {
    printf("%s: readback skipped (inited=%d cpp=%u)\n", tag, g_screen.inited,
           g_screen.cpp);
    return;
  }
  int fd = open("/dev/fb", O_RDWR);
  if (fd < 0) {
    printf("%s: /dev/fb open failed\n", tag);
    return;
  }
  size_t len = (size_t)g_screen.pitch * g_screen.height;
  len = (len + 0xFFFu) & ~(size_t)0xFFFu;
  void *base = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    printf("%s: mmap(%u) failed\n", tag, (unsigned)len);
    close(fd);
    return;
  }
  const uint16_t *p16 = (const uint16_t *)base;
  uint32_t row = g_screen.pitch / 2u;
  printf("%s: fb16 (0,0)=0x%04x (320,240)=0x%04x (639,479)=0x%04x (700,500)=0x%04x\n",
         tag, p16[0], p16[240u * row + 320u], p16[479u * row + 639u],
         p16[500u * row + 700u]);
  munmap(base, len);
  close(fd);
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

/* 【优雅关闭】把 V3D 硬件置回复位态。
 *
 * 为什么需要：gpud 被 kill -9 时用户态什么都没做，硬件停在运行态。
 * 下一次启动就会看到
 *     v3d: sms rc=0 pm rc=0 ree_cs=0x80000000   （正常应为 0x00000000）
 * 这种脏状态下 g2d 服务虽然能注册，但客户端拿到的 client 是无效的，
 * 表现为 client_get 之后崩在 libfoot 里（FAR=0x20）。
 *
 * 这里做两件事：
 *   1. 清 PM 的 V3DRSTN —— 把 QPU 阵列拉回复位；
 *   2. 写 SMS 让它回到 IDLE 状态。
 * 都由 MMIO 完成，可以在信号处理函数里安全执行。 */
static void v3d_hw_shutdown(void) {
  /* v3d_dev_t 只有 hub/core0/sms/pm 四个指针，用 pm 判空作为
   * "已初始化"的判据（初始化时最先建立的就是这几个映射）。 */
  if (g_dev.pm == NULL && g_dev.sms == NULL) {
    return;
  }
  /* 1) PM: 清 V3DRSTN（注意高 16 位是 password） */
  if (g_dev.pm != NULL) {
    uint32_t v = g_dev.pm[V3D_PM_GRAFX_OFF / 4];
    v &= ~V3D_PM_V3DRSTN;
    g_dev.pm[V3D_PM_GRAFX_OFF / 4] = (V3D_PM_PASSWORD) | (v & 0xFFFFu);
    __asm__ volatile("dsb sy" ::: "memory");
  }
  /* 2) SMS: 等状态回 IDLE（不完全依赖，只是尽力而为） */
  if (g_dev.sms != NULL) {
    for (int i = 0; i < 50; i++) {
      uint32_t st = g_dev.sms[V3D_SMS_REE_CS / 4] & V3D_SMS_STATE_MASK;
      if (st == V3D_SMS_STATE_IDLE) {
        break;
      }
      usleep(1000);
    }
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

/* 信号处理：先复位硬件再退出。只调用 async-signal-safe 的操作
 * （MMIO 写 + usleep + _exit）。 */
static void v3d_signal_handler(int sig) {
  v3d_hw_shutdown();
  _exit(sig == SIGTERM || sig == SIGINT ? 0 : 1);
}

static void v3d_install_signal_handlers(void) {
  struct sigaction sa;
  sa.sa_handler = v3d_signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  /* 注意：不接 SIGKILL（无法捕获），也不接 SIGSEGV（会让调试信息消失） */
}

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

  /* 4.5) V3D 时钟：Linux v3d_power_resume() 的第一步就是
   * clk_prepare_enable(v3d->clk)。固件若把 V3D 核心时钟留在关闭状态，
   * 寄存器读写照样能用（APB 时钟独立），但 QPU 阵列不推进 —— job 被接收、
   * CSD_STATUS 序号照常递增、INT_STS 只有残留位、像素一个都不写。
   * 这里把时钟拉到固件报告的最大值。 */
  {
    int mrc = v3d_mbox_init();
    unsigned mx = 0, now = 0;
    int setrc = -1;
    if (mrc == 0) {
      mx = (unsigned)v3d_mbox_get_max_clock(V3D_MBOX_CLOCK_V3D);
      if (mx != 0) {
        setrc = v3d_mbox_set_clock(V3D_MBOX_CLOCK_V3D, mx);
      }
      now = (unsigned)v3d_mbox_get_clock(V3D_MBOX_CLOCK_V3D);
    }
    printf("v3d: clock mbox=%d max=%u set_rc=%d now=%u\n", mrc, mx, setrc, now);
  }

  /* 5) P1：SMS 上电 + PM 复位 */
  /* 【SMS 上电 + PM 复位：QPU 阵列的启动开关】
   * 缺这一步 QPU 永远不启动 —— 但 job 提交不会报错：CFG0 写下去硬件收下、
   * CSD_STATUS 有值、INT_STS 只有残留位、像素全 0，看起来像"跑了但没写"。
   * 这两行原来没检查返回值，失败是静默的，这里必须回读状态寄存器。 */
  {
    /* 【先判断硬件是不是已经在跑，避免重复初始化】
     *
     * "已经在跑" 的三个条件同时成立才算：
     *   1. PM.V3DRSTN = 1        —— QPU 阵列已解锁
     *   2. SMS state = IDLE(0)   —— 电源状态机空闲
     *   3. REE_CS 的 bit31 = 0   —— 没有残留的异常标志
     *
     * 第 3 条很关键：上一个 gpud 被 kill -9 后会留下 ree_cs=0x80000000，
     * 这种脏状态下服务虽然能注册，但客户端拿到的 client 无效。
     * 遇到脏状态必须重新走一遍完整初始化，而不是跳过。
     *
     * 注意：MMU 页表 / L2C / 片内缓存是【每个实例自己的】，
     * 不管硬件是否已上电都要重新建立，所以只跳过 SMS+PM 这两步。 */
    uint32_t ree0 = g_dev.sms[V3D_SMS_REE_CS / 4];
    uint32_t pmg0 = g_dev.pm[V3D_PM_GRAFX_OFF / 4];
    uint32_t st0 = ree0 & V3D_SMS_STATE_MASK;
    int rstn0 = (pmg0 & V3D_PM_V3DRSTN) ? 1 : 0;
    int dirty = (ree0 & 0x80000000u) ? 1 : 0;
    /* 上一次 gpud 的 render 超时会把 CT1 留在 CTRUN=1；SMS 看起来仍是
     * IDLE，但 CLE 已经卡死。只看电源状态会错误地 skip reset，于是新
     * 进程第一次 clear 就复现「ct1ca +4096 / ct1cs 0x70」。 */
    uint32_t ct1cs0 = g_dev.core0[V3D_CLE_CT1CS / 4];
    uint32_t ct1ca0 = g_dev.core0[V3D_CLE_CT1CA / 4];
    int ct1_wedged = (ct1cs0 & V3D_CLE_CT1CS_CTRUN) ? 1 : 0;

    if (rstn0 && st0 == V3D_SMS_STATE_IDLE && !dirty && !ct1_wedged) {
      printf("v3d: hw already on (ree_cs=0x%08x pm_grafx=0x%08x "
             "ct1cs=0x%x) -> skip powerup/reset\n",
             ree0, pmg0, ct1cs0);
    } else {
      int rc_sms = v3d_sms_powerup(&g_dev);
      int rc_pm = v3d_pm_reset(&g_dev);
      uint32_t ree = g_dev.sms[V3D_SMS_REE_CS / 4];
      uint32_t pmg = g_dev.pm[V3D_PM_GRAFX_OFF / 4];
      printf("v3d: sms rc=%d pm rc=%d ree_cs=0x%08x(state=0x%x) "
             "pm_grafx=0x%08x rstn=%u%s%s (was ct1cs=0x%x ca=0x%x)\n",
             rc_sms, rc_pm, ree, (unsigned)(ree & V3D_SMS_STATE_MASK), pmg,
             (pmg & V3D_PM_V3DRSTN) ? 1u : 0u,
             dirty ? " [was dirty]" : "",
             ct1_wedged ? " [ct1 wedged]" : "",
             ct1cs0, ct1ca0);
    }
  }

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
  /* 【关键回读】0x11000000 = 272MB 本身就落在 1GB RAM 的物理地址范围内。
   * 如果 MMU 没真正 ENABLE，QPU 会把 0x11000000 当物理地址直通访问 ——
   * 那是内核堆，读到的是垃圾指令：既不报 MMU fault（地址合法），也不写
   * 像素（指令无意义）。这正好解释"无 fault + 像素全 0 + NC 不动"。 */
  printf("v3d: mmu ctl=0x%08x(en=%u) mmuc=0x%08x(en=%u) debug=0x%08x\n",
         v3d_mmu_ctl(&g_dev), (unsigned)(v3d_mmu_ctl(&g_dev) & 1u),
         v3d_mmuc_control(&g_dev),
         (unsigned)(v3d_mmuc_control(&g_dev) & 1u), v3d_mmu_debug_info(&g_dev));

  int lrc = v3d_l2c_enable(&g_dev);
  printf("v3d: L2C enable rc=%d l2cactl=0x%08x\n", lrc,
         v3d_l2c_status(&g_dev));

  /* 【中断全屏蔽 + 全清：QPU 阵列能不能发 batch 的关键】
   * EwokOS 在 init 末尾调 g2d_csd_irq_init()，头两行就是
   *   INT_MSK_SET = ~0;   INT_CLR = ~0;
   * 我们从来没碰过 INT_MSK。上电默认若是"全部使能"，QPU 每完成一个 batch
   * 就拉中断线，而没人清 INT_STS —— 实测一直停在 0x0fff0000（bits16..27
   * 的 QPU 位全挂着）。QPU 会认为中断未被处理而不再发放新 batch，
   * 表现就是 CURRENT_CFG4 恒为 0、像素一个都不写、CSD_DONE 永不置位。 */
  g_dev.core0[V3D_CTL_INT_MSK_SET / 4] = 0xFFFFFFFFu;
  g_dev.core0[V3D_CTL_INT_CLR / 4] = 0xFFFFFFFFu;
  __asm__ volatile("dmb sy" ::: "memory");
  printf("v3d: irq masked msk_sts=0x%08x int_sts=0x%08x\n",
         g_dev.core0[V3D_CTL_INT_MSK_STS / 4],
         g_dev.core0[V3D_CTL_INT_STS / 4]);

  /* 8) 屏幕初始化（不自动填色，避免盖住现有 UI；由应用 FILL 驱动） */
  screen_init();

  printf("v3d: backend ready (ver=%u ncores=%u)\n", ver, ncores);


  /* 从现在起接管 SIGTERM/SIGINT，退出时复位 V3D 硬件。
   * 没有这一步的话，被 kill 后硬件停在运行态，下次启动
   * 会看到 ree_cs=0x80000000 的脏状态。 */
  v3d_install_signal_handlers();

  /* ---- CSD 2D 自测（离屏 ARGB8888，不碰屏幕）----
   * 目的：验证 QPU 2D 内核能跑、uniform 传参正确。屏幕是 16bpp，
   * 上屏要另做格式转换，所以这里只在离屏 BO 上验证。 */
  if (v3d_g2d_init(&g_dev) == 0) {
  /* ---- 3D 管线验证：画一个三角形 ----
   * 只验证 CLE bin/render 这条链路能不能出像素，不做任何复杂效果。
   * 顶点格式：pos = float3(stride 12)、col = RGBA byte4(stride 4)。 */
  if (v3d_3d_init() == 0) {
    /* 用 3D 的 identity DMA arena 分配：iova == pa，免手动映射 */
    uint32_t vpa = v3d_dma_alloc(4096u, 512u);
    if (vpa != 0) {
      float* pos = (float*)v3d_dma_cpu(vpa);
      uint8_t* col = (uint8_t*)v3d_dma_cpu(vpa + 256);
      /* 屏幕坐标：NDC [-1,1] → [0,w]×[0,h]，y 向下（vtx shader 里 y 缩放为负） */
      pos[0] = -0.8f; pos[1] = -0.8f; pos[2] = 0.5f;
      pos[3] = 0.8f;  pos[4] = -0.8f; pos[5] = 0.5f;
      pos[6] = 0.0f;  pos[7] = 0.8f;  pos[8] = 0.5f;
      col[0] = 255; col[1] = 0; col[2] = 0; col[3] = 255;   /* 红 */
      col[4] = 0; col[5] = 255; col[6] = 0; col[7] = 255;   /* 绿 */
      col[8] = 0; col[9] = 0; col[10] = 255; col[11] = 255; /* 蓝 */
      foot_cache_clean(v3d_dma_cpu(vpa), v3d_dma_cpu(vpa + 512));

      /* 【终极内存验证】用已验证的 2D blit 内核读回 arena：
       * GPU 读 identity arena 的结果 == CPU 写入？
       *   diff=0  → 内存通路正常，CT 取指失败是 CT 自身问题
       *   diff>0  → GPU 读 arena 就是坏的——CT「取 1 字节停」完全解释通，
       *             修复方向 = 把 CL/shader 挪进 2D 已验证的 g2d arena */
      {
        /* v3d_g2d_blit_gpu 已由 v3d_g2d.h 声明 */
        uint32_t rdst = vpa + 512;
        int brc = v3d_g2d_blit_gpu(&g_dev, vpa, rdst, 16, 1);
        foot_cache_invalidate(v3d_dma_cpu(rdst), v3d_dma_cpu(rdst + 64));
        volatile unsigned int* s = (volatile unsigned int*)v3d_dma_cpu(vpa);
        volatile unsigned int* d = (volatile unsigned int*)v3d_dma_cpu(rdst);
        int diff = 0;
        for (int k = 0; k < 16; k++) {
          if (s[k] != d[k]) {
            diff++;
          }
        }
        printf("v3d_3d: readback rc=%d diff=%d/16 s0=%08x d0=%08x\r\n", brc,
               diff, s[0], d[0]);
      }

      /* 实验 0a：render CL 前缀二分。
       * 【暂时关闭】它会在 CT 卡死时污染后续所有测试（recover 复位不彻底），
       * 排查告一段落后再开。需要时把下面两行取消注释即可。 */
      /* int pprc = v3d_3d_probe_prefix();
         printf("v3d_3d: probe_prefix rc=%d\r\n", pprc); */

#if V3D_3D_STARTUP_PROBES
      /* 实验 0b：最小 render CL（Common + END），读 CT1CA 定位 CT 消费量。
       * 注意：它会产生「不 retire 的 job」，之后不要再提交真实 clear。 */
      int prc0 = v3d_3d_probe_rcl();
      printf("v3d_3d: probe rc=%d\r\n", prc0);

      /* 实验 0c：clear RCL 前缀梯度。CT1 卡死后无法恢复，所以一次启动
       * 只能拿到「第一个挂住的命令」。 */
      {
        int src = v3d_3d_probe_clear_steps(FB_GVA, (uint32_t)g_screen.pitch,
                                           V3D_3D_FMT_BGR565, 640, 480,
                                           0xFF0000FFu);
        printf("v3d_3d: probe_clear_steps rc=%d\n", src);
      }
#endif

      /* 实验 1：**冷启动后第一次 render 提交就是有效 clear**。
       * 顺序绝不能变：任何残缺 render CL（探针）先跑都会占住 CT1 的 job
       * 槽位，让后面的真实 clear 一个字节都不被消费。 */
      fb_readback_probe("v3d_3d: before clear");
      int crc = v3d_3d_clear_min(FB_GVA, (uint32_t)g_screen.pitch,
                                 V3D_3D_FMT_BGR565, 640, 480, 0xFF0000FFu);
      printf("v3d_3d: clear rc=%d (屏幕左上 640x480 应变纯色)\n", crc);
      fb_readback_probe("v3d_3d: after clear");

      int trc = v3d_3d_draw_min(FB_GVA, (uint32_t)g_screen.pitch,
                                V3D_3D_FMT_BGR565, 640, 480,
                                V3D_3D_MODE_TRIANGLES, 3, vpa, vpa + 256,
                                0xFF000000u, 1);
      printf("v3d_3d: triangle rc=%d (expect %d)\n", trc, V3D_3D_DONE);
      fb_readback_probe("v3d_3d: after triangle");
    } else {
      printf("v3d_3d: vertex buffer alloc failed\n");
    }
  } else {
    printf("v3d_3d: init failed\n");
  }
#if G2D_SELFTEST
    uint32_t tpa = 0, tsize = 0;
    foot_cap_t tcap =
        foot_cap_alloc_arena(256u * 256u * 4u + 8192u, 0, 0, &tpa, &tsize);
    if (tcap != FOOT_CAP_NONE && tsize >= 256u * 256u * 4u &&
        foot_cap_map(tcap, (void*)G2D_TEST_VA, 0, tsize) == 0) {
      volatile uint32_t* pt = (volatile uint32_t*)PT_VA;
      uint32_t tpages = tsize / V3D_MMU_PAGE_SIZE;
      for (uint32_t i = 0; i < tpages; i++) {
        v3d_mmu_map_page(pt, G2D_TEST_GVA + i * V3D_MMU_PAGE_SIZE,
                         tpa + i * V3D_MMU_PAGE_SIZE);
      }
      foot_cache_clean((void*)pt, (void*)(PT_VA + V3D_MMU_PT_BYTES));
      v3d_mmu_flush(&g_dev);

      volatile uint32_t* t32 = (volatile uint32_t*)G2D_TEST_VA;
      for (uint32_t i = 0; i < 256u * 256u; i++) {
        t32[i] = 0;
      }
      foot_cache_clean((void*)t32, (void*)(G2D_TEST_VA + tsize));

      int rc = v3d_g2d_fill(&g_dev, G2D_TEST_GVA, 256, 256, 0, 0, 256, 256,
                            0xFF00FF00u);
      foot_cache_invalidate((void*)t32, (void*)(G2D_TEST_VA + tsize));
      /* 【全量扫描】只看 3 个采样点无法区分"完全没写"和"写了但范围/步长
       * 不对"。统计非 0 像素数 + 记录第一个非 0 的位置，能直接定性：
       *   nonzero=0        → kernel 一条存储指令都没执行
       *   nonzero=65536    → 写满（正常）
       *   0 < nonzero < N  → 写了，但 band/stride 计算和预期不符 */
      {
        unsigned nz = 0, fidx = 0;
        unsigned fval = 0;
        for (unsigned i = 0; i < 256u * 256u; i++) {
          if (t32[i] != 0) {
            if (nz == 0) {
              fval = t32[i];
              fidx = i;
            }
            nz++;
          }
        }
        printf("g2d selftest: rc=%d nonzero=%u/%u first@%u=0x%08x "
               "(expect 65536/65536 first@0=0xff00ff00)\n",
               rc, nz, 256u * 256u, fidx, fval);
      }

      /* ---- blit / alpha 自测（第二块离屏 BO 作 dst）----
       * src 已被上面的 fill 填成了 0xFF00FF00（不透明绿）。 */
      {
        uint32_t dpa = 0, dsize = 0;
        foot_cap_t dcap =
            foot_cap_alloc_arena(256u * 256u * 4u + 8192u, 0, 0, &dpa, &dsize);
        if (dcap != FOOT_CAP_NONE && dsize >= 256u * 256u * 4u &&
            foot_cap_map(dcap, (void*)G2D_TEST2_VA, 0, dsize) == 0) {
          uint32_t dpages = dsize / V3D_MMU_PAGE_SIZE;
          for (uint32_t i = 0; i < dpages; i++) {
            v3d_mmu_map_page(pt, G2D_TEST2_GVA + i * V3D_MMU_PAGE_SIZE,
                             dpa + i * V3D_MMU_PAGE_SIZE);
          }
          foot_cache_clean((void*)pt, (void*)(PT_VA + V3D_MMU_PT_BYTES));
          v3d_mmu_flush(&g_dev);

          volatile uint32_t* d32 = (volatile uint32_t*)G2D_TEST2_VA;
          for (uint32_t i = 0; i < 256u * 256u; i++) {
            d32[i] = 0;
          }
          foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));

          /* 【实验 A：用已验证的 fill 去写 dst2】
           * fill 走的是同一条 CSD 提交路径且一直成功。如果它写不了这块
           * BO，说明问题在 dst2 的 PTE/映射，而不是 copy 内核；
           * 如果它能写满，那 dst2 完全正常，故障就收敛到 copy 内核本身
           * （读源 / batch 划分 / uniform 语义）。 */
          {
            int frc = v3d_g2d_fill(&g_dev, G2D_TEST2_GVA, 256, 256, 0, 0, 256,
                                   256, 0xFFFF0000u);
            foot_cache_invalidate((void*)d32, (void*)(G2D_TEST2_VA + dsize));
            unsigned fnz = 0;
            for (unsigned i = 0; i < 256u * 256u; i++) {
              if (d32[i] != 0) {
                fnz++;
              }
            }
            printf("g2d dst2-fill: rc=%d nonzero=%u/%u px[0]=0x%08x "
                   "(expect 65536, 0xffff0000)\n",
                   frc, fnz, 256u * 256u, d32[0]);
          }

          /* 【CPU alpha 混合（半透明合成的可用实现）】
           * dst2 此刻全是红 0xFFFF0000，src 是绿 0xFF00FF00。
           * 把绿按 50% 叠到红上，期望 0xFF807F00：
           *   有效 alpha a = (255*128)>>8 = 127
           *   r = (0*127 + 255*128)/255 = 128 = 0x80
           *   g = (255*127 + 0*128)/255 = 127 = 0x7f
           * QPU 的 argb_alpha 要读 dst，而 TMU 读在我们这边不可用；
           * TFU 又不支持混合，所以这条路是现在唯一可用的。 */
          {
            const volatile uint32_t* s32 =
                (const volatile uint32_t*)(uintptr_t)G2D_TEST_VA;
            v3d_g2d_alpha_cpu((uint32_t*)(uintptr_t)d32,
                              (const uint32_t*)(uintptr_t)s32, 256, 256, 128);
            foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));
            printf("g2d alpha(cpu): px[0]=0x%08x (expect 0xff807f00, "
                   "green 50%% over red)\n",
                   d32[0]);
          }

          /* 【最小 TMU 读验证】
           * src 已被最前面的 fill 填成 0xFF00FF00（绿）。这里用正确的
           * Texture/Sampler State 读它的 (0,0) 像素，写到 dst2 起始处：
           *   dst2[0] == 0xff00ff00  ⇒ descriptor 布局正确，TMU 读通了
           *   dst2[0] == 0x00000000  ⇒ 仍读不到（descriptor 字段或坐标还有问题）
           * 先把 dst2 清 0，这样结果一目了然。 */
          for (uint32_t i = 0; i < 256u * 256u; i++) {
            d32[i] = 0;
          }
          foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));
          {
            /* 先跑对照：只写常量，不读源。通了说明内核框架和写路径正常，
             * 那 miniread 的失败就只可能出在 TMU 读这一侧。 */
            /* 阶梯探针：四个标记分别代表"经过某一步后仍能写完"。
             *   11111111 = 基线（内核在跑）
             *   22222222 = 经过 mov tmuc ×3
             *   33333333 = 经过 thrsw
             *   44444444 = 经过 ldtmu
             * 第一个缺失的位置就是卡死点。 */
            /* 最小读内核：读是第一个 TMU 操作，读 src 首字后写 dst。
             *   px[0] = 读到的值（期望 ff00ff00）
             *   px[1] = 0x11111111（标记；写出来 = 内核越过了读） */
            /* 预埋两个可区分的像素，用来确定 TMU 坐标编码：
             *   src[1]   = 0x00000001
             *   src[256] = 0x00000100
             * 其余仍是 fill 的 0xff00ff00。 */
            t32[1] = 0x00000001u;
            t32[256] = 0x00000100u;
            foot_cache_clean((void*)t32, (void*)(G2D_TEST_VA + dsize));

            /* 【coordprobe 已移除】它依赖 g2d_mini_write，而那个小内核随
             * mini_kernel.h 一起删除了（stub 成 return -1 只会打印 rc=-1）。
             *
             * 它原本的用途是"证明 TMU 裸字节地址读是通的"。这个能力现在
             * 由下面 blit(gpu) 的 65536/65536 更强地证明 —— blit 能在整幅
             * 图上逐像素读对，比读 4 个地址严格得多。
             * 若将来又要单独验读通路，把 mini_kernel.h 从 git 历史里取回、
             * 恢复 v3d_g2d_miniwrite 即可。 */
            (void)d32;

          /* 【让 src 逐像素不同】之前 src 几乎全是 0xff00ff00，只有 s1、
           * s256 两处不同 —— 读错位置也是绿色，所以"lane 2..15 全对"
           * 根本不能证明它们读对了。这里填成逐像素唯一的图案。 */
          for (uint32_t i = 0; i < 256u * 256u; i++) {
            t32[i] = 0x80000000u | i;
          }
          foot_cache_clean((void*)t32, (void*)(G2D_TEST_VA + dsize));

          /* 【原版 argb_copy，按 EwokOS 探针的 ABI 跑】
           * EwokOS 的 v3d_g2d_vec4_ok() 就是这么调的：拷 2 行 × 256 字节，
           * 单 QPU。如果这版能拷出正确像素，说明之前 blit/alpha 失败纯粹是
           * 我们算错了 uniform 布局；如果仍为 0，则是环境差异（它的 buffer
           * 在 dma NOCACHE 且 IOVA == 物理地址）。 */
          for (uint32_t i = 0; i < 256u * 256u; i++) {
            d32[i] = 0;
          }
          foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));
          {
            int orc = v3d_g2d_copy_orig(&g_dev, G2D_TEST_GVA, G2D_TEST2_GVA,
                                        256u, 2u);
            foot_cache_invalidate((void*)d32, (void*)(G2D_TEST2_VA + dsize));
            unsigned onz = 0;
            for (unsigned i = 0; i < 128u; i++) {
              if (d32[i] != 0) {
                onz++;
              }
            }
            (void)0;
          }

          /* 【原版 argb_copy + 物理地址】EwokOS 是 identity map，它传 pa；
           * 我们刚把 arena 也 identity 映射了，条件等价。 */
          for (uint32_t i = 0; i < 256u * 256u; i++) {
            d32[i] = 0;
          }
          foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));
          {
            int orc2 = v3d_g2d_copy_orig(&g_dev, tpa, dpa, 256u, 2u);
            foot_cache_invalidate((void*)d32, (void*)(G2D_TEST2_VA + dsize));
            unsigned onz2 = 0;
            for (unsigned i = 0; i < 128u; i++) {
              if (d32[i] != 0) {
                onz2++;
              }
            }
            (void)0;
          }

          /* 【GPU 版 blit】src → dst2 整幅拷贝。
           * 特殊值，拷完核对这三处即可判断拷贝是否逐像素正确。 */
          for (uint32_t i = 0; i < 256u * 256u; i++) {
            d32[i] = 0;
          }
          foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));
          {
            int brc = v3d_g2d_blit_gpu(&g_dev, G2D_TEST_GVA, G2D_TEST2_GVA,
                                       256, 256);
            foot_cache_invalidate((void*)d32, (void*)(G2D_TEST2_VA + dsize));
            unsigned bnz = 0;
            for (unsigned i = 0; i < 256u * 256u; i++) {
              if (d32[i] == t32[i]) {
                bnz++;
              }
            }
            /* 诊断：看拷贝落在哪些位置。src 只有 [1]=00000001、[256]=00000100
             * 两处特殊，其余 ff00ff00；dst 初值 0。
             *   d0..d3 = 第一个 chunk 的前 4 个 lane
             *   d15    = 第一个 chunk 的最后一个 lane
             *   d16    = 第二个 chunk 起点
             *   d256   = 第 256 像素（跨行） */
            /* 按 lane 统计错误：每个 chunk 有 16 个 lane，若只有某一个 lane
             * 一直错，说明它的 TMU 请求每轮都被吞掉、结果停在第一轮的值。 */
            unsigned bad[16];
            for (int l = 0; l < 16; l++) {
              bad[l] = 0;
            }
            for (unsigned i = 0; i < 256u * 256u; i++) {
              if (d32[i] != t32[i]) {
                bad[i & 15u]++;
              }
            }
            printf("g2d blit(gpu): rc=%d match=%u/65536 bad-per-lane: "
                   "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
                   brc, bnz, bad[0], bad[1], bad[2], bad[3], bad[4], bad[5],
                   bad[6], bad[7], bad[8], bad[9], bad[10], bad[11], bad[12],
                   bad[13], bad[14], bad[15]);
            printf("  d0=%08x d1=%08x d2=%08x d3=%08x d15=%08x d16=%08x "
                   "d17=%08x d255=%08x d256=%08x d257=%08x\n",
                   d32[0], d32[1], d32[2], d32[3], d32[15], d32[16], d32[17],
                   d32[255], d32[256], d32[257]);
            printf("  src: s0=%08x s1=%08x s15=%08x s16=%08x s255=%08x "
                   "s256=%08x s257=%08x\n",
                   t32[0], t32[1], t32[15], t32[16], t32[255], t32[256],
                   t32[257]);
          }

          /* 【GPU 版 alpha】红底上按 50% 混绿，期望 0xff7f7f00。
           * 注意这里读 dst，所以 dst 的 CPU 预填必须先 clean。 */
          for (uint32_t i = 0; i < 256u * 256u; i++) {
            d32[i] = 0xFFFF0000u;
          }
          foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));
          {
            int arc = v3d_g2d_alpha_gpu(&g_dev, G2D_TEST_GVA, G2D_TEST2_GVA,
                                        256, 256, 128);
            foot_cache_invalidate((void*)d32, (void*)(G2D_TEST2_VA + dsize));
            printf("g2d alpha(gpu): rc=%d px0=%08x (expect bf7f0000: src=80000000 dst=ffff0000 a=128)\n", arc,
                   d32[0]);
          }

          /* 【上屏验证】把 GPU 渲染结果（dst2 里的 ARGB8888）转成 RGB565，
           * 借 src BO 的前 128KB 做中转，再用 TFU 拷到屏幕左上角。
           * 屏幕是 RGB565、每行 1920 像素，所以 dst_stride = 1920。
           * 这一段跑完，屏幕上应该出现一块 256×256 的彩色区域。 */
          {
            volatile uint16_t* o = (volatile uint16_t*)(uintptr_t)t32;
            for (uint32_t i = 0; i < 256u * 256u; i++) {
              uint32_t c = d32[i];
              uint32_t r = (c >> 16) & 0xFFu;
              uint32_t g = (c >> 8) & 0xFFu;
              uint32_t b = c & 0xFFu;
              o[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
            foot_cache_clean((void*)t32, (void*)(G2D_TEST_VA + 128u * 1024u));
            int trc = v3d_tfu_copy_ex(&g_dev, G2D_TEST_GVA, FB_GVA, 256, 256,
                                      256, 1920, 6);
            printf("g2d onscreen: rc=%d (256x256 上屏到 0,0，RGB565)\n", trc);

            /* 【PRESENT API 通路验证】走服务接口把 dst2 的 ARGB8888
             * 直接打成 RGB565 上屏，验证 GPU 内核 + 句柄解析这条链路。
             * 像素正确性靠肉眼（应与上面 TFU 那条几乎一样）。 */
            {
              gpu_bo_import_req ir;
              memset(&ir, 0, sizeof(ir));
              ir.pa = dpa;
              ir.size = dsize;
              ir.flags = GPU_BO_CACHED;
              int ph = v3d_bo_create(&ir);
              if (ph > 0) {
                gpu_present_req pr;
                memset(&pr, 0, sizeof(pr));
                pr.src = (unsigned int)ph;
                pr.w = 256;
                pr.h = 256;
                int prc = v3d_present(&pr);
                printf("g2d api(present): rc=%d (256x256 ARGB8888 -> 屏幕 RGB565)\n",
                       prc);
                gpu_bo_ref br;
                br.handle = (unsigned int)ph;
                v3d_bo_destroy(&br);
              } else {
                printf("g2d api(present): 导入 BO 失败 (%d)\n", ph);
              }
            }
          }

          /* 【直接地址读】用 argb_copy 的真实机制（tmuc=0xFFFFFFFC +
             * tmua=裸字节地址）读 src 的第一个字。这一步能把"TMU 直接读
             * 到底能不能用"单独拎出来验证，不带任何 2D 逻辑。 */
            for (uint32_t i = 0; i < 256u * 256u; i++) {
              d32[i] = 0;
            }
            foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));
            {
              int crc =
                  v3d_g2d_minicopy(&g_dev, G2D_TEST_GVA, G2D_TEST2_GVA);
              foot_cache_invalidate((void*)d32,
                                    (void*)(G2D_TEST2_VA + dsize));
              (void)0;
            }

            /* 【同一实验，改用物理地址】
             * EwokOS 的 IOVA == 物理地址（identity map），它传给 TMUA 的
             * “裸地址”在那边两种解释等价。我们的 src/dst 是 0x31000000/
             * 0x32000000 这类 IOVA，如果 TMU 的直接寻址模式绕过 MMU，
             * 那么它看到的会是一个不存在的物理地址 —— 读回 0 正好吻合。
             * 这里改传 tpa/dpa（arena 的物理基址）做对照。 */
            for (uint32_t i = 0; i < 256u * 256u; i++) {
              d32[i] = 0;
            }
            foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));
            {
              int crc2 = v3d_g2d_minicopy(&g_dev, tpa, dpa);
              foot_cache_invalidate((void*)d32,
                                    (void*)(G2D_TEST2_VA + dsize));
              (void)0;
            }

            /* 【变体：mov tmuc 带 sig.wrtmuc】
             * argb_copy 原文没有这个信号；而 Mesa 提交 TMU 配置靠的正是它。
             * 若这一版能读出来，说明 argb_copy 缺的就是这一步。 */
            for (uint32_t i = 0; i < 256u * 256u; i++) {
              d32[i] = 0;
            }
            foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));
            {
              int crc3 =
                  v3d_g2d_minicopy2(&g_dev, G2D_TEST_GVA, G2D_TEST2_GVA);
              foot_cache_invalidate((void*)d32,
                                    (void*)(G2D_TEST2_VA + dsize));
              (void)0;
            }

            /* 再清 0，跑真正的读测试 */
            for (uint32_t i = 0; i < 256u * 256u; i++) {
              d32[i] = 0;
            }
            foot_cache_clean((void*)d32, (void*)(G2D_TEST2_VA + dsize));

            int mrc = v3d_g2d_miniread(&g_dev, G2D_TEST_GVA, G2D_TEST2_GVA,
                                       256, 256);
            foot_cache_invalidate((void*)d32, (void*)(G2D_TEST2_VA + dsize));
            /* 一次读 4 个分量：源是 0xFF00FF00（内存里按 RGBA8 解释为
             * R=0x00, G=0xFF, B=0x00, A=0xFF），所以期望 00/ff/00/ff。
             * 只要出现 ff 就说明 TMU 读通了 —— 之前只读 R 分量，恰好是 0，
             * 所以"读通"和"读不到"看起来一样。 */
            /* swizzle 修好后，源 0xFF00FF00 按 RGBA8 解析是
             * R=0x00 G=0xFF B=0x00 A=0xFF。所以期望 R/B 为 0、
             * G/A 非 0。具体编码取决于 output_type：32 位浮点时
             * 1.0 = 0x3f800000，16 位整数时 0xFF = 0xFFFF。 */
            /* 探针输出已停用：这条读路径依赖 EwokOS 那个我们没复现的
             * vec4 模式，在本项目的自产路线上恒为 0，不宜出现在正常日志里。
             * 需要时取消下面一行的注释即可恢复。 */
            (void)mrc;
            (void)d32;
          }

          /* 【blit / alpha 已确认不可用，不再做启动自测】
           *
           * 反汇编 + 探针对照得出的结论：
           *   - argb_fill 只用 TMU 写，真机可用；
           *   - argb_blit / argb_alpha / argb_copy 都要【读】源，而 TMU 读
           *     依赖 TMUC 的 vec4 general-access 模式；
           *   - 该模式在 EwokOS 里也只有仿真器验证（v3d_g2d_vec4_ok() 的
           *     注释自述 unverified silicon territory），真机实测读回恒为 0、
           *     不报 MMU fault（PT_INVALID_ENABLE=0，无效 PTE 静默返回 0）。
           *
           * 所以：不透明贴图走 TFU（已验证），半透明合成暂由 CPU 兜底。
           * 等 TMUC 模式逆向清楚或改用编译器生成的 shader 再启用这两个内核。 */
        } else {
          printf("g2d blit/alpha: dst arena alloc/map failed\n");
        }
      }
    } else {
      printf("g2d selftest: arena alloc/map failed\n");
    }
#endif /* G2D_SELFTEST */
  }
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

/* CL 提交（Mesa 桥接）：handle=0 用内部测试 CL，否则解析 BO 的 GPU VA */
static int v3d_submit_cl(void* args) {
  gpu_submit_cl_req* r = (gpu_submit_cl_req*)args;
  if (r == NULL) {
    return -1;
  }
  if (v3d_3d_init() != 0) {
    return -2;
  }

  uint32_t bs, be, rs, re;
  if (r->bcl_handle == 0) {
    uint32_t base = v3d_3d_cl_base();
    bs = base + r->bcl_start;
    be = base + r->bcl_end;
  } else {
    if (r->bcl_handle > V3D_MAX_BOS || !g_bos[r->bcl_handle - 1].in_use) {
      return -3;
    }
    uint32_t bg = g_bos[r->bcl_handle - 1].gva;
    bs = bg + r->bcl_start;
    be = bg + r->bcl_end;
  }
  if (r->rcl_handle == 0) {
    uint32_t base = v3d_3d_cl_base();
    rs = base + r->rcl_start;
    re = base + r->rcl_end;
  } else {
    if (r->rcl_handle > V3D_MAX_BOS || !g_bos[r->rcl_handle - 1].in_use) {
      return -3;
    }
    uint32_t rg = g_bos[r->rcl_handle - 1].gva;
    rs = rg + r->rcl_start;
    re = rg + r->rcl_end;
  }
  return v3d_3d_submit_cl(bs, be, rs, re, r->w, r->h);
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

/* 【上屏】ARGB8888 画布 → 屏幕 RGB565。
 *
 * 每帧要有一次：屏幕是 16bpp、画布是 32bpp，格式不匹配必须转换。
 * 这里把转换放到 GPU 上做（v3d_g2d_present_gpu），省掉 CPU 每帧
 * 1920x1080 的 8.3MB 读 + 4.1MB 写。
 *
 * 对齐要求：内核每 lane 处理 2 个像素，一轮 32 像素，所以宽度必须是
 * 32 的倍数。宽度不对齐时补齐会多读源数据，而源 stride 未知（BO 里
 * 没记录），紧凑布局下会越界 —— 所以这里直接拒绝，由调用方保证对齐
 * （1920 宽的整屏画布天然满足）。 */
static int v3d_present(void* args) {
  gpu_present_req* r = (gpu_present_req*)args;
  if (r == NULL || r->w == 0 || r->h == 0 || r->src == 0) {
    return -1;
  }
  if (!g_screen.inited) {
    return -2;
  }

  int i = (int)r->src - 1;
  if (i < 0 || i >= V3D_MAX_BOS || !g_bos[i].in_use) {
    return -2;
  }

  uint32_t w32 = (r->w + 31u) & ~31u;
  if (w32 != r->w) {
    return -3; /* 宽度须为 32 的倍数 */
  }

  uint32_t src_stride = r->w * 4u; /* 源按紧凑 ARGB8888 */
  uint32_t sb = g_bos[i].gva + r->sy * src_stride + r->sx * 4u;

  /* 第 1 步：GPU 转成紧凑 RGB565，落在 g2d 的 scratch 区 */
  uint32_t scratch_gva = 0;
  int rc = v3d_g2d_present_gpu(&g_dev, sb, r->w, r->h, &scratch_gva);
  if (rc != 0) {
    return rc;
  }

  /* 第 2 步：TFU 带 stride 贴上屏。
   * 屏幕 pitch 是 3840 字节 = 1920 像素；内容宽 r->w 像素。
   * 两者不同，正是必须用 TFU 的原因 —— GPU 内核是线性扫描，
   * 直接写屏幕会破坏行对齐，逐行 dispatch 又会累计超时。 */
  uint32_t dst_stride_px = g_screen.pitch / g_screen.cpp;
  uint32_t db = FB_GVA + (r->dy * dst_stride_px + r->dx) * g_screen.cpp;
  return v3d_tfu_copy_ex(&g_dev, scratch_gva, db, r->w, r->h, r->w,
                         dst_stride_px, V3D_TEX_FORMAT_RGB565);
}

/* 把 (句柄, 宽度) 解析成 GPU VA 与行字节跨度。
 * handle=0 表示屏幕；>0 查 BO 表。BO 里没有记录 stride，沿用紧凑假设
 * （每行 = w 像素），这也是 Wayland 合成画布的常见布局。 */
static int v3d_resolve(uint32_t handle, uint32_t w, uint32_t* gva,
                       uint32_t* stride4) {
  if (handle == 0) {
    if (!g_screen.inited) {
      return -1;
    }
    *gva = FB_GVA;
    *stride4 = g_screen.pitch; /* 屏幕 pitch 已是字节 */
    return 0;
  }
  int i = (int)handle - 1;
  if (i < 0 || i >= V3D_MAX_BOS || !g_bos[i].in_use) {
    return -1;
  }
  *gva = g_bos[i].gva;
  *stride4 = w * 4u;
  return 0;
}

/* 【GPU blit】用自产的 copy_vec 内核做 1:1 拷贝。
 *
 * 内核要求一次处理的像素数是 16 的倍数，而 Wayland 窗口宽度几乎都不会
 * 正好对齐 16。这里把宽度补齐到 16 的倍数（多拷最多 15 个像素），
 * 前提是【目标行确实够宽】—— 补出来的那几个像素落在行尾 padding 里。
 * 若 dst 是紧凑布局且宽度未对齐，就拒绝（返回 -3），以免写坏邻接内容。 */
static int v3d_blit(void* args) {
  gpu_blit_req* r = (gpu_blit_req*)args;
  if (r == NULL || r->w == 0 || r->h == 0) {
    return -1;
  }

  uint32_t sg, dg, ss, ds;
  if (v3d_resolve(r->src, r->w, &sg, &ss) != 0) {
    return -2;
  }
  if (v3d_resolve(r->dst, r->w, &dg, &ds) != 0) {
    return -2;
  }

  uint32_t w16 = (r->w + 15u) & ~15u;
  if (ds < w16 * 4u) {
    return -3; /* 目标行不足，补齐会越界 */
  }

  uint32_t sb = sg + r->sy * ss + r->sx * 4u;
  uint32_t db = dg + r->dy * ds + r->dx * 4u;

  if (ss == w16 * 4u && ds == w16 * 4u) {
    /* 两边都紧凑且已对齐 → 一次整块拷贝 */
    return v3d_g2d_blit_gpu(&g_dev, sb, db, w16, r->h);
  }

  /* 否则逐行（每行宽度已对齐，满足内核要求） */
  for (uint32_t y = 0; y < r->h; y++) {
    int rc = v3d_g2d_blit_gpu(&g_dev, sb + y * ss, db + y * ds, w16, 1);
    if (rc != 0) {
      return rc;
    }
  }
  return 0;
}

/* 【GPU alpha】source-over 混合：dst = src*a + dst*(1-a)，a 取 0..256。
 * 宽度补齐与 v3d_blit 同理。 */
static int v3d_alpha(void* args) {
  gpu_alpha_req* r = (gpu_alpha_req*)args;
  if (r == NULL || r->w == 0 || r->h == 0) {
    return -1;
  }
  if (r->alpha == 0) {
    return 0; /* 全透明 = 无操作 */
  }

  uint32_t sg, dg, ss, ds;
  if (v3d_resolve(r->src, r->w, &sg, &ss) != 0) {
    return -2;
  }
  if (v3d_resolve(r->dst, r->w, &dg, &ds) != 0) {
    return -2;
  }

  uint32_t w16 = (r->w + 15u) & ~15u;
  if (ds < w16 * 4u) {
    return -3;
  }

  uint32_t a = (r->alpha > 256u) ? 256u : r->alpha;
  uint32_t sb = sg + r->sy * ss + r->sx * 4u;
  uint32_t db = dg + r->dy * ds + r->dx * 4u;

  if (ss == w16 * 4u && ds == w16 * 4u) {
    return v3d_g2d_alpha_gpu(&g_dev, sb, db, w16, r->h, a);
  }

  for (uint32_t y = 0; y < r->h; y++) {
    int rc = v3d_g2d_alpha_gpu(&g_dev, sb + y * ss, db + y * ds, w16, 1, a);
    if (rc != 0) {
      return rc;
    }
  }
  return 0;
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
    .alpha = v3d_alpha,
    .present = v3d_present,
    /* 低层：BO 管理已实现；SUBMIT_CL/CSD 随阶段推进补 */
    .bo_create = v3d_bo_create,
    .bo_mmap = NULL, /* CPU 映射在应用侧（foot_cap_alloc_frame） */
    .bo_destroy = v3d_bo_destroy,
    .bo_offset = v3d_bo_offset,
    .submit_cl = v3d_submit_cl,
    .submit_csd = NULL,
    .submit_tfu = v3d_submit_tfu,
    .wait_bo = NULL,
};
