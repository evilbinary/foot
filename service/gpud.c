/*******************************************************************
 * gpud：GPU 服务进程（仿 vfsd）。
 *
 * 【定位】独立进程，注册 foot 服务名 "gpu"。具体逻辑全在通用的
 * gpu_service_run()（foot/driver/gpu/gpu_service.c）里：
 *   - 硬件初始化
 *   - client_regist("gpu")
 *   - client_run(gpu_service_fn)
 * 本文件只负责"选哪个后端"。
 *
 * 【后端选择】
 *   默认 v3d（当前唯一实现的后端）。
 *   定义 GPU_USE_MALI 编译宏时改选 mali（目前是空实现，init 返回 -1，
 *   会打印 "mali: backend not implemented (stub)" 并退出）。
 *   以后接入真正的 Mali 驱动时，按平台宏（如 V3S）或运行时探测切换。
 ********************************************************************/
#include <stdio.h>

#include "client.h"
#include "gpu_backend.h"

#if defined(GPU_USE_MALI)
extern const gpu_backend_t mali_backend;
#define GPU_BACKEND_DEFAULT (&mali_backend)
#else
extern const gpu_backend_t v3d_backend;
#define GPU_BACKEND_DEFAULT (&v3d_backend)
#endif

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  /* 带上构建时间戳：调试期间"输出没变化"经常是因为新二进制没部署，
   * 有了这一行，每次看第一行就能确认跑的是哪一版。 */
  printf("gpud: starting (build %s %s)\n", __DATE__, __TIME__);

  const gpu_backend_t* g_be = GPU_BACKEND_DEFAULT;

  return gpu_service_run(g_be, "gpu") == 0 ? 0 : 1;
}
