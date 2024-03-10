
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

void mmio_init() {
  // MMIO_BASE GPIOBASE
  char* args[] = {sys->cid, MMIO_BASE, MMIO_BASE, 1024, 1};
  int ret = client_call(sys, 2, args);
  if (ret < 0) {
    perror("map addr error\n");
    exit(-1);
  }
  printf("map addr ret %d\n", ret);

  char* args2[] = {sys->cid, HCD_DESIGNWARE_BASE, HCD_DESIGNWARE_BASE, 1024, 1};
  ret = client_call(sys, 2, args2);
  if (ret < 0) {
    perror("map addr error\n");
    exit(-1);
  }

  // //Broadcom 2835 with a Designware OTG Core
  char* args3[] = {sys->cid, MMIO_BASE + 0x0000B000, MMIO_BASE + 0x0000B000,
                   1024, 1};
  ret = client_call(sys, 2, args3);
  if (ret < 0) {
    perror("map addr error\n");
    exit(-1);
  }

  char* args4[] = {sys->cid, MMIO_BASE + 0x00003000, MMIO_BASE + 0x00003000,
                   1024, 1};
  ret = client_call(sys, 2, args4);
  if (ret < 0) {
    perror("map addr error\n");
    exit(-1);
  }

  char* args5[] = {sys->cid, MMIO_BASE + 0x2e000, MMIO_BASE + 0x2e000, 1024, 1};
  ret = client_call(sys, 2, args5);
  if (ret < 0) {
    perror("map addr error\n");
    exit(-1);
  }
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