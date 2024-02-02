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
      ret = syscall(SYS_THREAD_MAP, arg[0], arg[1], arg[2], arg[3], arg[4]);
      break;
    case SYS_INIT_TIMER: {  // register timer
      printf("system timer init %d %d\n", arg[0], arg[1]);

      client_t* client = client_id(arg[0]);
      if (client != NULL) {
        system_timer_t* t = malloc(sizeof(system_timer_t));
        t->client = client;
        t->freq = arg[1];
        timer_client[timer_client_size++] = t;
        printf("system timer get client id %d init\n", client->id);
      } else {
        printf("system timer get client %d no found\n", arg[0]);
      }
    } break;
    default:
      break;
  }
  for (int i = 0; i < timer_client_size; i++) {
    system_timer_t* c = timer_client[i];
    printf("system tick client %d ticks %d freq %d count %d\n", c->client->id,
           c->ticks, c->freq, count);

    if (count % c->freq == 0) {
      char* args[] = {c->ticks};
      printf("tick call count=%d ticks=%d\n", count, c->ticks);
      int ret = client_call(c->client, SYS_TIMER, args);
      printf("tick call end count=%d ticks=%d\n", count, c->ticks);

      c->ticks++;
    }
  }
  printf("count=%d\n", count);
  count++;
  return ret;
}

int main(int argc, char* argv[]) {
  client_t* client = client_regist("system");
  client_run(client, service_fn);

  return 0;
}