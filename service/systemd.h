/*******************************************************************
 * Copyright 2021-present evilbinary
 * 作者: evilbinary on 01/01/20
 * 邮箱: rootdebug@163.com
 ********************************************************************/
#ifndef SYSTEMD_H
#define SYSTEMD_H
#include "client.h"

enum {
  SYS_TIMER = 1,
  SYS_MAP = 2,
  SYS_INIT_TIMER = 3,

};

typedef struct system_timer {
  int ticks;
  int freq;
  client_t* client;
} system_timer_t;

#endif