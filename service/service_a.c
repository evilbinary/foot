#include <stdio.h>

#include "client.h"


int main(int argc, char* argv[]) {
  client_t* client = client_get("serviceb");

  for(;;){
    int ret = client_call(client, 0, "hello");
    printf("call ret =%d\n", ret);
    sleep(8);
  }

  return 0;
}