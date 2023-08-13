#include <stdio.h>
#include <sys/syscall.h>

#include "client.h"

int count = 0;

enum {
  SYS_THREAD_MAP = 518,
};

void* service_fn(void* fn, void* args) {
  int ret = 0;
  char** arg = args;
  switch ((int)fn) {
    case 2:
      ret = syscall(SYS_THREAD_MAP, arg[0], arg[1], arg[2], arg[3]);
      break;
    default:
      break;
  }
  count++;
  return ret;
}

int main(int argc, char* argv[]) {
  client_t* client = client_regist("system");

  client_run(client, service_fn);

  return 0;
}