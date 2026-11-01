/*******************************************************************
 * 固件 mailbox（property channel）实现。
 *
 * 寄存器（Pi5：0x107c013880，与 Pi2/3 的 0xB880 不同）：
 *   +0x00 MBOX_READ   读响应（低 4 位是 channel）
 *   +0x18 MBOX_STATUS bit31=FULL（写给固件前必须为空）bit30=EMPTY
 *   +0x20 MBOX_WRITE  写请求
 *
 * 【物理地址】Pi5 的属性通道填的是 buffer 的**物理地址**（GPU_MEM_BASE=0，
 * 不做 0xC0000000 别名转换，见 bcm2835.h 的 RASPI5 分支）。
 ********************************************************************/
#include "v3d_mbox.h"

#include <string.h>

#include "client.h"

#define MBOX_VA 0x4A000000UL      /* mailbox 寄存器映射的 VA */
#define MBOX_BUF_VA 0x4A001000UL  /* property buffer 的 VA */

#define MBOX_READ 0x00u
#define MBOX_STATUS 0x18u
#define MBOX_WRITE 0x20u

#define MBOX_FULL (1u << 31)
#define MBOX_EMPTY (1u << 30)
#define MBOX_CH_PROP 8u

#define VC_TAG_GET_CLOCK 0x00030002u     /* GET_CLOCK_RATE */
#define VC_TAG_GET_MAX_CLOCK 0x00030004u /* GET_MAX_CLOCK_RATE */
#define VC_TAG_SET_CLOCK 0x00038002u     /* SET_CLOCK_RATE */
#define VC_REQUEST_OK 0x80000000u

static volatile uint32_t* g_mbox;
static volatile uint32_t* g_buf;
static uint32_t g_buf_pa;
static int g_ok;

int v3d_mbox_init(void) {
  if (g_ok) {
    return 0;
  }
  foot_cap_t mc = foot_cap_get_mmio("mailbox");
  if (mc == FOOT_CAP_NONE) {
    return -1;
  }
  if (foot_cap_map(mc, (void*)MBOX_VA, 0, 0x1000) != 0) {
    return -1;
  }
  /* 【偏移 0x880】cap_devwin 授权的是整页窗口 0x107c013000（页对齐），
   * 而 mailbox 寄存器实际在 0x107c013880（Pi5 把 Pi2/3 的 +0xB880 挪成了
   * +0x13880）。之前直接拿窗口基址当偏移 0 用，读到的不是 MBOX_STATUS
   * 而是别的东西 —— EMPTY 位恒为 1，轮询永远超时（call=-1），固件响应
   * 一次都没收到，buffer 里自然还是我们写进去的原样。 */
  g_mbox = (volatile uint32_t*)(uintptr_t)(MBOX_VA + 0x880u);

  uint32_t pa = 0, size = 0;
  foot_cap_t bc = foot_cap_alloc_arena(4096, 0, 0, &pa, &size);
  if (bc == FOOT_CAP_NONE || pa == 0) {
    return -1;
  }
  if (foot_cap_map(bc, (void*)MBOX_BUF_VA, 0, size) != 0) {
    return -1;
  }
  g_buf = (volatile uint32_t*)(uintptr_t)MBOX_BUF_VA;
  g_buf_pa = pa;
  g_ok = 1;
  return 0;
}

/* 一次 mailbox 往返。每次调用都重填消息头，调用方只改 tag 部分。 */
static int mbox_call(void) {
  if (!g_ok) {
    return -1;
  }
  /* property buffer 是缓存内存：写入后要让固件看到 */
  foot_cache_clean((void*)(uintptr_t)MBOX_BUF_VA,
                   (void*)(uintptr_t)(MBOX_BUF_VA + 256));

  uint32_t addr = (g_buf_pa & ~0xFu) | MBOX_CH_PROP;

  for (int i = 0; i < 100000; i++) {
    if (!(g_mbox[MBOX_STATUS / 4] & MBOX_FULL)) {
      break;
    }
  }
  g_mbox[MBOX_WRITE / 4] = addr;

  for (int i = 0; i < 100000; i++) {
    if (!(g_mbox[MBOX_STATUS / 4] & MBOX_EMPTY)) {
      if ((g_mbox[MBOX_READ / 4] & 0xFu) == MBOX_CH_PROP) {
        /* 固件写回了 buffer，取回 CPU 视角 */
        foot_cache_invalidate((void*)(uintptr_t)MBOX_BUF_VA,
                              (void*)(uintptr_t)(MBOX_BUF_VA + 256));
        return 0;
      }
    }
  }
  return -1;
}

static uint32_t clock_query(uint32_t tag, uint32_t clock_id, int* ok) {
  if (ok != NULL) {
    *ok = 0;
  }
  if (!g_ok) {
    return 0;
  }
  g_buf[0] = 32; /* 总字节数 */
  g_buf[1] = 0;  /* 请求码 */
  g_buf[2] = tag;
  g_buf[3] = 8; /* buffer size */
  g_buf[4] = 8; /* data size */
  g_buf[5] = clock_id;
  g_buf[6] = 0;
  g_buf[7] = 0; /* end tag */

  int cr = mbox_call();
  /* 【诊断】把固件的原始响应打出来：g_buf[1] 应为 0x80000000（成功）。
   * 若仍是 0，说明固件没回写（buffer 物理地址或 cache 维护有问题）；
   * 若是 0x80000001，说明固件解析失败（消息格式不对）。 */
  {
    static int dbg = 0;
    if (dbg++ < 4) {
      printf("mbox: tag=%08x call=%d pa=%08x resp=[%08x %08x %08x %08x %08x "
             "%08x %08x %08x]\n",
             tag, cr, g_buf_pa, g_buf[0], g_buf[1], g_buf[2], g_buf[3], g_buf[4],
             g_buf[5], g_buf[6], g_buf[7]);
    }
  }
  if (cr != 0 || g_buf[1] != VC_REQUEST_OK) {
    return 0;
  }
  if (ok != NULL) {
    *ok = 1;
  }
  return g_buf[6];
}

uint32_t v3d_mbox_get_max_clock(uint32_t clock_id) {
  return clock_query(VC_TAG_GET_MAX_CLOCK, clock_id, NULL);
}

uint32_t v3d_mbox_get_clock(uint32_t clock_id) {
  return clock_query(VC_TAG_GET_CLOCK, clock_id, NULL);
}

int v3d_mbox_set_clock(uint32_t clock_id, uint32_t rate) {
  if (!g_ok) {
    return -1;
  }
  g_buf[0] = 36;
  g_buf[1] = 0;
  g_buf[2] = VC_TAG_SET_CLOCK;
  g_buf[3] = 12;
  g_buf[4] = 12;
  g_buf[5] = clock_id;
  g_buf[6] = rate;
  g_buf[7] = 0; /* skip_turbo */
  g_buf[8] = 0; /* end tag */

  if (mbox_call() != 0 || g_buf[1] != VC_REQUEST_OK) {
    return -1;
  }
  return 0;
}
