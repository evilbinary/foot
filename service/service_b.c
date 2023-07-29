#include <stdio.h>

#include "client.h"

void* service_fn(void* fn, void* args) {
  printf("fn=>%d\n", fn);

  if (fn == 0) {
    printf("init %d %s\n", fn, args);
    return 888;
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

  printf("client %x\n", client);
  printf("client id %d\n", client->id);

  client_run(client, service_fn);

  return 0;
}