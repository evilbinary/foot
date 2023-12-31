#include <stdio.h>
#include "systemd.h"

#include "client.h"

void* service_fn(void* fn, void* args) {
  printf("fn=>%d\n", fn);

  if (fn == 0) {
    printf("init %d %s\n", fn, args);
    return 888;
  } else if (fn == SYS_TIMER) {
    printf("tick timer\n");
    return 2;
  } else if (fn == 2) {
    printf("svc call\n");
    return 2;
  }
}

int main(int argc, char* argv[]) {
  client_t* client = client_regist("vfs");

  client_run(client, service_fn);

  return 0;
}