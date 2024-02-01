#include <stdio.h>
#include "systemd.h"
#include "client.h"

int count=0;

void* service_fn(void* fn, void* args) {
  printf("fn=>%d\n", fn);
  char** arg = args;

  if (fn == 0) {
    printf("init %d %s\n", fn, args);
    return 888+count++;
  } else if (fn == SYS_TIMER) {
    printf("tick timer %d\n",arg[0]);
    return 2;
  } else if (fn == 2) {
    printf("svc call\n");
    return 2;
  }
}

int main(int argc, char* argv[]) {

  //reg timer
  client_t* systemd = client_get("system");
  if(systemd==NULL){
    printf("error get systemd\n");
    return 0;
  }
  printf("get system is ==> %s\n",systemd->name);
  client_t* client = client_regist("serviceb");

  printf("get client is ==> %d %d\n",client->cid,client->id);

  client_call(systemd, SYS_INIT_TIMER, client->id ,1);

  client_run(client, service_fn);

  return 0;
}