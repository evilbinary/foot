# coding:utf-8
# *******************************************************************
# Copyright 2021-present evilbinary
# 作者: evilbinary on 01/01/20
# 邮箱: rootdebug@163.com
# ********************************************************************
# GPU 驱动根目录：
#   gpu_backend.h  后端接口（各 GPU 实现、gpud 使用）
#   gpu_service.c  通用服务壳（方法号分发 + client_regist + client_run）
#   v3d/           V3D 后端（VideoCore VII）
#   mali/          以后的 Mali 后端（同样实现 gpu_backend_t）
target("gpu")
set_type("lib")
add_deps("foot")
add_files("gpu_service.c")
add_includedirs(".", public = true)

add_subs('./*/ya.py')
