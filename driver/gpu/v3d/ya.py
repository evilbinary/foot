# coding:utf-8
# *******************************************************************
# Copyright 2021-present evilbinary
# 作者: evilbinary on 01/01/20
# 邮箱: rootdebug@163.com
# ********************************************************************
# raspi5 V3D（VideoCore VII）GPU 驱动 —— 后端【库】（不再是独立进程）。
# 提供 gpu_backend_t v3d_backend，由 foot/service/gpud 链接。
# 服务注册与 client_run 在 gpud.c。
target("v3d")
set_type("lib")
add_deps("foot", "gpu")
add_files(
    'v3d_backend.c',
    'v3d_hw.c',
    'v3d_mmu.c',
    'v3d_csd.c',
    'v3d_tfu.c',
    'v3d_g2d.c'
)
