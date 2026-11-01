/*******************************************************************
 * Mali 后端（占位，尚未实现）。
 *
 * 接入真正的 Mali GPU 驱动（如 Arm Mali-G 系列）时，在 mali_backend.c
 * 里补齐 init/fill/blit/bo_create/submit_cl 等实现即可，gpud.c 里
 * 把后端选择改成 &mali_backend。
 ********************************************************************/
#ifndef MALI_BACKEND_H
#define MALI_BACKEND_H

#include "gpu_backend.h"

extern const gpu_backend_t mali_backend;

#endif /* MALI_BACKEND_H */
