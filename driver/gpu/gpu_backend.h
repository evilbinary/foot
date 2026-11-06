/*******************************************************************
 * 通用 GPU 后端接口（gpu_backend_t）。
 *
 * 【定位】foot 的 GPU 驱动是"服务进程 + 后端库"结构（与 vfsd 同构）：
 *   foot/service/gpud.c   独立进程，注册服务名 "gpu"
 *   foot/driver/v3d/      V3D 后端实现（提供 v3d_backend）
 *   foot/driver/gpu/      本头文件：后端抽象（本文件）
 *
 * 应用侧只认服务名 "gpu" 和高层方法号，不感知具体 GPU；gpud 内部按平台
 * 选择后端（第一阶段固定 v3d）。
 *
 * 【两层接口】
 *   高层（GPU_FN_*）：FILL/BLIT/SYNC，所有后端都要实现（sw 用 memcpy）。
 *   低层（GPU_LOW_*）：BO 管理 + 命令流提交 + 同步，为 Mesa3D 接入准备；
 *     后端不支持时对应函数指针置 NULL，gpud 返回 -ENOSYS。
 *
 * 【类型】本头文件只依赖标准 C 类型（unsigned int），不 include 内核的
 * libs/include/types.h —— foot/driver 的纯头文件库没有那条 include 路径。
 ********************************************************************/
#ifndef GPU_BACKEND_H
#define GPU_BACKEND_H

/* ---------------- 高层方法号（所有后端必须实现） ---------------- */
enum gpu_fn {
  GPU_FN_QUERY = 0, /* 返回值：(screen_w << 16) | screen_h */
  GPU_FN_FILL = 1,  /* args = gpu_fill_req* */
  GPU_FN_BLIT = 2,  /* args = gpu_blit_req* */
  GPU_FN_SYNC = 3,  /* args = NULL（等 GPU 空闲） */
  GPU_FN_ALPHA = 4, /* args = gpu_alpha_req*（source-over 混合） */
  GPU_FN_PRESENT = 5, /* args = gpu_present_req*（ARGB8888→屏幕格式并上屏） */
};

/* ---------------- 低层方法号（可选，为 Mesa 准备） ---------------- */
enum gpu_fn_low {
  GPU_LOW_GET_PARAM = 100,
  GPU_LOW_GET_BO_OFFSET = 101,
  GPU_LOW_BO_CREATE = 110,
  GPU_LOW_BO_MMAP = 111,
  GPU_LOW_BO_DESTROY = 112,
  GPU_LOW_SUBMIT_CL = 120,
  GPU_LOW_SUBMIT_CSD = 121,
  GPU_LOW_SUBMIT_TFU = 122,
  GPU_LOW_WAIT_BO = 130,
  GPU_LOW_SCREEN_INFO = 140,
  GPU_LOW_SCREEN_PARAM = 141,
  GPU_LOW_PRESENT = 142, /* 应用像素 BO -> 屏幕（默认 direct，可选 tfu） */
};

/* ---- BO（GPU 内存对象）----
 * 【模型】buffer 由应用/Mesa 自己分配（foot_cap_alloc_frame 拿 pa），
 * 通过 BO_IMPORT 交给 gpud：gpud 把 pa 逐页映射进 GPU VA 空间、记录句柄。
 * 这样 CPU 映射始终在应用侧（应用不需要把 VA 跨进程传），gpud 只管
 * "GPU 侧地址"。
 *
 *   args = gpu_bo_import_req{pa, size, flags}  → 返回值 = handle（>0）
 *   args = gpu_bo_ref{handle}                   → 返回值 = GPU VA（offset）
 *   args = gpu_bo_ref{handle}                   → 返回值 = 0（释放）
 */
typedef struct {
  unsigned int pa;    /* 物理地址（页对齐） */
  unsigned int size;  /* 字节数（页对齐） */
  unsigned int flags; /* GPU_BO_* */
} gpu_bo_import_req;

typedef struct {
  unsigned int handle;
} gpu_bo_ref;

#define GPU_BO_SCANOUT (1u << 0) /* 显示输出 */
#define GPU_BO_CACHED (1u << 1)  /* CPU 频繁读写 */
#define GPU_BO_GPU_ONLY (1u << 2)

/* ---- SUBMIT_TFU：按 BO 句柄做拷贝/填充（后端用 TFU 实现）----
 * handle=0 表示屏幕；offset 是【字节偏移】（屏幕 = y*pitch + x*cpp）。
 * src 和 dst 必须是【同一像素格式】（TFU 不做格式转换），tex 取 dst 的格式。 */
typedef struct {
  unsigned int src_handle, src_offset;
  unsigned int dst_handle, dst_offset;
  unsigned int w, h;
  unsigned int src_fmt, dst_fmt; /* GPU_FMT_*（0 = 按屏幕/后端默认） */
  unsigned int flags;            /* GPU_SUBMIT_ASYNC */
} gpu_submit_tfu_req;

#define GPU_SUBMIT_ASYNC (1u << 0)

/* ---------------- 后端标识 ---------------- */
#define GPU_BACKEND_NONE 0
#define GPU_BACKEND_V3D 1
#define GPU_BACKEND_MALI 2
#define GPU_BACKEND_SW 3

/* ---------------- 能力位 ---------------- */
#define GPU_CAP_FILL (1u << 0)
#define GPU_CAP_BLIT (1u << 1)
#define GPU_CAP_ALPHA (1u << 2)
#define GPU_CAP_SCALE (1u << 3)
#define GPU_CAP_COMPUTE (1u << 4)
#define GPU_CAP_3D (1u << 5)

/* ---------------- 通用像素格式 ---------------- */
#define GPU_FMT_RGB565 1
#define GPU_FMT_ARGB8888 2
#define GPU_FMT_RGBA8888 3
#define GPU_FMT_NV12 4

/* ---------------- 高层参数 ---------------- */
typedef struct {
  unsigned int api_version; /* 当前 1 */
  unsigned int backend_id;  /* GPU_BACKEND_* */
  unsigned int flags;       /* GPU_CAP_* */
  unsigned int max_width, max_height;
  unsigned int fmt_mask; /* GPU_FMT_* 位图 */
  unsigned int screen_w, screen_h, screen_fmt;
} gpu_info_t;

typedef struct {
  unsigned int x, y, w, h; /* 目标矩形 */
  unsigned int color;      /* 颜色：阶段 1 按屏幕格式解释（RGB565 用低 16 位；
                            * ARGB8888 用 32 位），阶段 2 统一 0xAARRGGBB */
} gpu_fill_req;

typedef struct {
  unsigned int src; /* 源 buffer 句柄（0 = 屏幕） */
  unsigned int dst; /* 目标 buffer 句柄（0 = 屏幕） */
  unsigned int sx, sy; /* 源坐标 */
  unsigned int dx, dy; /* 目标坐标 */
  unsigned int w, h;   /* 尺寸 */
  unsigned int flags;  /* GPU_BLIT_* */
} gpu_blit_req;

#define GPU_BLIT_ASYNC (1u << 0)

/* 【alpha 混合】dst = src*a + dst*(1-a)，逐通道。
 * alpha 取值 0..256（256 = 完全不透明）。src/dst 句柄 0 表示屏幕。 */
typedef struct {
  unsigned int src;
  unsigned int dst;
  unsigned int sx, sy; /* 源坐标 */
  unsigned int dx, dy; /* 目标坐标 */
  unsigned int w, h;
  unsigned int alpha; /* 0..256 */
  unsigned int flags;
} gpu_alpha_req;

#define GPU_ALPHA_ASYNC (1u << 0)

/* 【上屏】把一块 ARGB8888（或指定格式）的 buffer 贴到屏幕。
 * 内部做格式转换（ARGB8888 -> RGB565），因为屏幕是 16bpp。 */
typedef struct {
  unsigned int src;      /* 源句柄（0 表示屏幕自身，非法） */
  unsigned int sx, sy;
  unsigned int dx, dy;   /* 屏幕坐标 */
  unsigned int w, h;
  unsigned int src_fmt;  /* GPU_FMT_*，0 = ARGB8888 */
  unsigned int flags;
} gpu_present_req;

/* 低层上屏：应用已经把自己的像素放进一个 GPU BO，gpud 负责贴到屏幕上。
 * mode: 0/default = direct，1 = tfu。src_fmt: GPU_FMT_RGB565 时走
 * 直拷（TFU 直接写屏幕）；其它格式走转换路径。 */
typedef struct {
  unsigned int src_handle; /* BO_CREATE 返回的句柄 */
  unsigned int src_offset; /* BO 内字节偏移 */
  unsigned int w, h;
  unsigned int src_fmt;    /* GPU_FMT_* */
  unsigned int dst_x, dst_y;
  unsigned int mode;       /* gpu_present_mode_t */
  unsigned int flags;
} gpu_present_frame_req;

/* ---- CL 提交（Mesa 桥接用，对齐 V3D_SUBMIT_CL 的最小集）----
 * CL 由应用侧（Mesa）生成，放在 BO 里；handle==0 表示用 gpud 内部
 * 自建的测试 CL（此时 start/end 是相对内部 CL 缓冲的偏移）。 */
typedef struct {
  unsigned int bcl_handle; /* bin CL 所在 BO 句柄，0 = 内部测试 CL */
  unsigned int bcl_start;  /* BO 内偏移（字节） */
  unsigned int bcl_end;    /* 结束偏移（exclusive） */
  unsigned int rcl_handle; /* render CL 所在 BO 句柄，0 = 内部 */
  unsigned int rcl_start;
  unsigned int rcl_end;
  /* Mesa 路径：qma/qms/qts 是 Mesa 自己的 tile_alloc / tile_state BO 的
   * GPU VA（不是 gpud 内部的 _pool_p/_tsda_p）。0 表示用内部缓冲。 */
  unsigned int qma;
  unsigned int qms;
  unsigned int qts;
  unsigned int w, h;       /* 渲染目标尺寸（tile 配置用） */
  unsigned int flags;      /* GPU_SUBMIT_CL_* */
} gpu_submit_cl_req;

#define GPU_SUBMIT_CL_ABS_VA (1u << 0) /* bcl/rcl start/end 是绝对 GPU VA */


/* 屏幕 framebuffer 信息：direct scanout 模式下应用要把这块已有显存
 * 包成 Mesa pipe_resource，GPU 直接渲染进去，显示控制器扫同一块内存。 */
typedef struct {
  unsigned int gva;    /* 屏幕在 GPU MMU 里的虚拟地址 */
  unsigned int pa;     /* 物理地址 */
  unsigned int width;  /* 1920 */
  unsigned int height; /* 1080 */
  unsigned int pitch;  /* 字节/行 */
  unsigned int cpp;    /* 2 (RGB565) */
  unsigned int format; /* V3D_TEX_FORMAT_RGB565 = 6 */
  unsigned int size;   /* pitch * height */
} gpu_screen_info;

/* 屏幕信息逐字段查询（client_call 只能回传一个整数） */
enum {
  GPU_SCREEN_PARAM_GVA = 0,
  GPU_SCREEN_PARAM_PA = 1,
  GPU_SCREEN_PARAM_WIDTH = 2,
  GPU_SCREEN_PARAM_HEIGHT = 3,
  GPU_SCREEN_PARAM_PITCH = 4,
  GPU_SCREEN_PARAM_CPP = 5,
  GPU_SCREEN_PARAM_FORMAT = 6,
  GPU_SCREEN_PARAM_SIZE = 7,
};

/* present 模式参数（应用侧选择） */
typedef enum {
  GPU_PRESENT_MODE_TFU = 0,    /* 离屏 BO -> TFU -> 屏幕 */
  GPU_PRESENT_MODE_DIRECT = 1  /* 直接渲染到屏幕 framebuffer */
} gpu_present_mode_t;

/* ---------------- 后端抽象 ---------------- */
typedef struct gpu_backend {
  const char* name;     /* "v3d" */
  unsigned int backend_id; /* GPU_BACKEND_V3D */

  int (*init)(void);       /* 硬件初始化（SMS/PM/MMU/L2C/屏幕） */
  int (*query)(void* out); /* 返回 (w<<16)|h */

  /* 高层：所有后端都实现 */
  int (*fill)(void* args); /* args = gpu_fill_req* */
  int (*blit)(void* args); /* args = gpu_blit_req* */
  int (*sync)(void* args); /* 等 GPU 空闲 */
  int (*alpha)(void* args);   /* args = gpu_alpha_req* */
  int (*present)(void* args); /* args = gpu_present_req* */
  int (*present_frame)(void* args); /* args = gpu_present_frame_req* */

  /* 低层：不支持则置 NULL */
  int (*bo_create)(void* args); /* 导入已有 pa，返回 handle */
  int (*bo_mmap)(void* args);   /* 未用（CPU 映射在应用侧） */
  int (*bo_destroy)(void* args);
  int (*bo_offset)(void* args); /* handle → GPU VA */
  int (*submit_cl)(void* args);
  int (*submit_csd)(void* args);
  int (*screen_info)(void* args); /* gpu_screen_info* */
  int (*screen_param)(unsigned int param);
  int (*submit_tfu)(void* args);
  int (*wait_bo)(void* args);
} gpu_backend_t;

/* 通用服务壳（gpu_service.c）：初始化后端 → 注册 svc_name → client_run。
 * 方法号分发到 be->xxx()。返回 0 成功，-1 初始化/注册失败。 */
int gpu_service_run(const gpu_backend_t* be, const char* svc_name);

#endif /* GPU_BACKEND_H */
