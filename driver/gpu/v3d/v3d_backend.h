/*******************************************************************
 * V3D 后端：实现 gpu_backend_t，由 foot/service/gpud 链接。
 *
 * gpud 的 main 里：
 *   extern const gpu_backend_t v3d_backend;
 *   g_be = &v3d_backend;
 *   g_be->init();
 ********************************************************************/
#ifndef V3D_BACKEND_H
#define V3D_BACKEND_H

#include "gpu_backend.h"

extern const gpu_backend_t v3d_backend;

#endif /* V3D_BACKEND_H */
