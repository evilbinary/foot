/*******************************************************************
 * 通用 GPU 服务壳：把 foot 服务方法号分发到 gpu_backend_t 的函数指针，
 * 并负责 client_regist + client_run。
 *
 * 【为什么放 driver/gpu 而不是 service/gpud】
 *   这段逻辑对所有 GPU 后端都一样（方法号 → 后端函数），属于"驱动框架"，
 *   和具体后端（v3d/mali/sw）一起演进。gpud.c 只是"入口"，选后端 + 调
 *   本函数；以后加后端，gpud 里改一行 g_be 即可。
 *
 * 【调用方式】
 *   foot/service/gpud.c:
 *     extern const gpu_backend_t v3d_backend;
 *     int main(void) { return gpu_service_run(&v3d_backend, "gpu") ? 1 : 0; }
 ********************************************************************/
#include <stdio.h>

#include "client.h"
#include "gpu_backend.h"

static const gpu_backend_t* g_be;

/* 服务方法分发：返回值只能是"值"（整数/错误码），不能传跨进程指针。 */
static void* gpu_service_fn(void* fn, void* args) {
  if (g_be == NULL) {
    return (void*)(long)-1;
  }
  switch ((int)(long)fn) {
    /* 高层 */
    case GPU_FN_QUERY:
      return (void*)(long)(g_be->query ? g_be->query(args) : 0);
    case GPU_FN_FILL:
      return (void*)(long)(g_be->fill ? g_be->fill(args) : -1);
    case GPU_FN_BLIT:
      return (void*)(long)(g_be->blit ? g_be->blit(args) : -1);
    case GPU_FN_SYNC:
      return (void*)(long)(g_be->sync ? g_be->sync(args) : 0);

    /* 低层（阶段 2+，为 Mesa 准备） */
    case GPU_LOW_GET_BO_OFFSET:
      return (void*)(long)(g_be->bo_offset ? g_be->bo_offset(args) : -1);
    case GPU_LOW_BO_CREATE:
      return (void*)(long)(g_be->bo_create ? g_be->bo_create(args) : -1);
    case GPU_LOW_BO_MMAP:
      return (void*)(long)(g_be->bo_mmap ? g_be->bo_mmap(args) : -1);
    case GPU_LOW_BO_DESTROY:
      return (void*)(long)(g_be->bo_destroy ? g_be->bo_destroy(args) : -1);
    case GPU_LOW_SUBMIT_TFU:
      return (void*)(long)(g_be->submit_tfu ? g_be->submit_tfu(args) : -1);
    case GPU_LOW_SUBMIT_CL:
      return (void*)(long)(g_be->submit_cl ? g_be->submit_cl(args) : -1);
    case GPU_LOW_SUBMIT_CSD:
      return (void*)(long)(g_be->submit_csd ? g_be->submit_csd(args) : -1);
    case GPU_LOW_WAIT_BO:
      return (void*)(long)(g_be->wait_bo ? g_be->wait_bo(args) : -1);

    default:
      return (void*)(long)-1;
  }
}

int gpu_service_run(const gpu_backend_t* be, const char* svc_name) {
  if (be == NULL || be->init == NULL || svc_name == NULL) {
    printf("gpu_service: invalid backend/name\n");
    return -1;
  }
  g_be = be;

  /* 1) 硬件初始化（capability 映射 + SMS/PM/MMU/L2C + 屏幕） */
  if (be->init() != 0) {
    printf("gpu_service: backend '%s' init failed\n", be->name);
    return -1;
  }

  /* 2) 注册服务 + 跑循环（和 vfsd 一样） */
  client_t* client = client_regist(svc_name);
  if (client == NULL) {
    printf("gpu_service: client_regist(%s) failed\n", svc_name);
    return -1;
  }
  printf("gpu_service: service '%s' ready (backend=%s)\n", svc_name, be->name);
  client_run(client, gpu_service_fn);
  return 0;
}
