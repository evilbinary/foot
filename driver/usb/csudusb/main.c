
#include <stdio.h>
#include <stdlib.h>

#include "client.h"
#include "configuration.h"
#include "systemd.h"

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

enum {
  USB_INIT = 0,
  USB_TIMER = 1,
  USB_SVC = 2,
  USB_CHECK = 3,
};

int count = 0;
client_t* sys = NULL;

void LogPrint(const char* message, unsigned int messageLength) {
  (void)messageLength;
  printf("%s", message);
}

void* service_fn(void* fn, void* args) {
  printf("fn=>%d\n", fn);
  char** arg = args;

  if (fn == USB_INIT) {
    printf("usb init\n");
    usb_init();
    return 1;
  } else if (fn == USB_TIMER) {
    printf("tick timer %d\n", arg[0]);
    return 2;
  } else if (fn == USB_SVC) {
    printf("svc call\n");
    return 2;
  } else if (fn == USB_CHECK) {
    UsbCheckForChange();
  }
}

void usb_init() {
  UsbInitialise();
  UsbCheckForChange();
}

/* 设备窗口能力句柄（bcm-periph：MMIO_BASE 起 16MB 的整段外设）。
 * 阶段 4 之后内核不再接受用户态自己填物理地址的中转映射（sys_thread_map
 * 需 CAP_MMIO 覆盖），所以映射改成"先换句柄、再用句柄映射"。 */
static foot_cap_t mmio_cap = FOOT_CAP_NONE;

static int mmio_map(unsigned long addr, unsigned long len) {
  if (mmio_cap == FOOT_CAP_NONE) {
    return -1;
  }
  unsigned long off = addr - (unsigned long)MMIO_BASE;
  if (foot_cap_map(mmio_cap, (void*)addr, off, len) < 0) {
    printf("mmio map %lx+%lx failed\n", addr, len);
    return -1;
  }
  return 0;
}

void mmio_init() {
  mmio_cap = foot_cap_get_mmio("bcm-periph");
  if (mmio_cap == FOOT_CAP_NONE) {
    printf("cap get mmio 'bcm-periph' failed\n");
    exit(-1);
  }
  printf("mmio cap=0x%x\n", mmio_cap);

  /* GPIO / DWC2 / SYS_TIMER 等寄存器窗口（映射长度按页对齐） */
  if (mmio_map((unsigned long)MMIO_BASE, 0x1000) < 0 ||
      mmio_map((unsigned long)HCD_DESIGNWARE_BASE, 0x10000) < 0 ||
      mmio_map((unsigned long)MMIO_BASE + 0x0000B000, 0x1000) < 0 ||
      mmio_map((unsigned long)MMIO_BASE + 0x00003000, 0x1000) < 0 ||
      mmio_map((unsigned long)MMIO_BASE + 0x0002E000, 0x1000) < 0) {
    perror("map addr error\n");
    exit(-1);
  }
  printf("mmio map ok\n");
}

int main(int argc, char* argv[]) {
  printf("start usbd\n");

  sys = client_get("system");
  if (sys == NULL) {
    perror("client get error\n");
    exit(-1);
  }
  printf("get system %s\n", sys->name);

  client_t* usb_client = client_regist("usb");
  printf("get client cid=>%d id=>%d\n", usb_client->cid, usb_client->id);

  mmio_init();
  usb_init();

  // char* args[] = {usb_client->id, 1};
  // client_call(sys, SYS_INIT_TIMER, args);

  // client_run(usb_client, service_fn);
  printf("aaa==============>\n");

  while (1) {
    UsbCheckForChange();

    for (int i = 0; i < KeyboardCount(); ++i) {
      int addr = KeyboardGetAddress(i);
      int count = KeyboardGetKeyDownCount(addr);
      int key = KeyboardGetKeyDown(addr, i);

      KeyboardPoll(addr);

      printf("addr %x count %d key %d\n", addr, count, key);
    }
    

    usleep(1000000);
  }

  return 0;
}