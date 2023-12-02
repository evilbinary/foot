/*******************************************************************
 * Copyright 2021-present evilbinary
 * 作者: evilbinary on 01/01/20
 * 邮箱: rootdebug@163.com
 ********************************************************************/
#include "systemd.h"

#include <stdio.h>
#include <sys/syscall.h>

int count = 0;

enum {
  SYS_THREAD_MAP = 518,
};

system_timer_t* timer_client[256];
int timer_client_size = 0;

void* service_fn(void* fn, void* args) {
  int ret = 0;
  char** arg = args;
  switch ((int)fn) {
    case SYS_MAP:
      ret = syscall(SYS_THREAD_MAP, arg[0], arg[1], arg[2], arg[3]);
      break;
    case SYS_INIT_TIMER: {  // register timer
      printf("system timer init %x\n",arg[0]);

      client_t* client = client_id(arg[0]);
      if (client != NULL) {
        system_timer_t* t = malloc(sizeof(system_timer_t));
        t->client = client;
        t->freq = arg[1];
        timer_client[timer_client_size++] = t;
      }
    } break;
    default:
      break;
  }
  for (int i = 0; i < timer_client_size; i++) {
    system_timer_t* c = timer_client[i];
    printf("system tick %s %d\n",c->client->name,c->ticks);

    if (c->freq % count == 0) {
      int ret = client_call(c->client, SYS_TIMER, c->ticks);
      c->ticks++;
    }
  }
  count++;
  return ret;
}

int main(int argc, char* argv[]) {
  client_t* client = client_regist("system");
  client_run(client, service_fn);

  return 0;
}