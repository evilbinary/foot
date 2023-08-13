#include <stdio.h>

#include "client.h"

int count=0;

void* service_fn(void* fn, void* args) {
  printf("fn=>%d\n", fn);

  if (fn == 0) {
    printf("init %d %s\n", fn, args);
    return 888+count++;
  } else if (fn == 1) {
    printf("tick timer\n");
    return 2;
  } else if (fn == 2) {
    printf("svc call\n");
    return 2;
  }
}

int main(int argc, char* argv[]) {
  client_t* client = client_regist("serviceb");

  client_run(client, service_fn);

  return 0;
}