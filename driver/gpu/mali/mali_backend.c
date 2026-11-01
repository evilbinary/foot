/*******************************************************************
 * Mali 后端空实现（占位）。
 *
 * 【当前状态】只给出 gpu_backend_t 结构体和"未实现"返回值，用于：
 *   1) 验证 gpu/ 下多后端并存的组织与构建；
 *   2) 给未来 Mali 驱动接入留好位置。
 *
 * init() 返回 -1，gpud 若选到本后端会打印失败并退出，不会误用。
 ********************************************************************/
#include <stdio.h>

#include "gpu_backend.h"
#include "mali_backend.h"

static int mali_init(void) {
  printf("mali: backend not implemented (stub)\n");
  return -1;
}

static int mali_query(void* args) {
  (void)args;
  return 0; /* 没有屏幕信息 */
}

static int mali_fill(void* args) {
  (void)args;
  return -1;
}

static int mali_blit(void* args) {
  (void)args;
  return -1;
}

static int mali_sync(void* args) {
  (void)args;
  return 0;
}

const gpu_backend_t mali_backend = {
    .name = "mali",
    .backend_id = GPU_BACKEND_MALI,
    .init = mali_init,
    .query = mali_query,
    .fill = mali_fill,
    .blit = mali_blit,
    .sync = mali_sync,
    /* 低层（BO / 命令流提交）待接入 Mali 驱动后补 */
    .bo_create = NULL,
    .bo_mmap = NULL,
    .bo_destroy = NULL,
    .submit_cl = NULL,
    .submit_csd = NULL,
    .submit_tfu = NULL,
    .wait_bo = NULL,
};
