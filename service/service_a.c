#include <stdio.h>

#include "client.h"


int main(int argc, char* argv[]) {
  client_t* client = client_get("serviceb");

  printf("client %x\n", client);
  printf("client id %d\n", client->id);

  int ret = client_call(client, 0, "hello");
  printf("call ret =%d\n", ret);

  return 0;
}