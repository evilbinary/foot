
#include <stdio.h>
#include "client.h"
#include "systemd.h"

enum {
  USB_INIT =0,
  USB_TIMER = 1,
  USB_SVC = 2,
  USB_CHECK=3,
};

void LogPrint(const char* message, unsigned int messageLength) {
  (void)messageLength;
  printf("==>%s", message);
}


int count = 0;

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
  } else if(fn==USB_CHECK){
    UsbCheckForChange();
  }
}


void usb_init(){
  UsbInitialise();
}


int main(int argc, char* argv[]) {
  printf("start usbd\n");

  client_t* system = client_get("system");
  if (system == NULL) {
    perror("client get error\n");
    exit(-1);
  }
  printf("get system %s\n", system->name);

  // 0x3F200000UL GPIOBASE
  char* args[] = {system->cid, 0x2000B880UL, 0x2000B880UL, 1024, 1};
  int ret = client_call(system, 2, args);
  if (ret < 0) {
    perror("map addr error\n");
    exit(-1);
  }
  printf("map addr ret %d\n", ret);

  char* args2[] = {system->cid, 0x20003000UL, 0x20003000UL, 1024, 1};
  ret = client_call(system, 2, args2);
  if (ret < 0) {
    perror("map addr error\n");
    exit(-1);
  }

  printf("map addr ret %d\n", ret);

  client_t* usb_client = client_regist("usb");

  printf("get client is ==> %d %d\n", usb_client->cid, usb_client->id);

  client_call(system, SYS_INIT_TIMER, usb_client->id, 1);
  
  
  usb_init();


  client_run(usb_client, service_fn);

  return 0;
}