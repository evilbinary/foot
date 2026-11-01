# coding:utf-8
# *******************************************************************
# * Copyright 2021-present evilbinary
# * 作者: evilbinary on 01/01/20
# * 邮箱: rootdebug@163.com
# ********************************************************************
target("servicea")
set_type("cli")
add_files("service_a.c") 
add_deps("foot")

target("serviceb")
set_type("cli")
add_files("service_b.c") 
add_deps("foot")

target("vfsd")
set_type("cli")
add_files("vfsd.c") 
add_deps("foot")

target("systemd")
set_type("cli")
add_files("systemd.c") 
add_deps("foot")

# GPU 服务进程（仿 vfsd）：注册服务名 "gpu"，内部链接 GPU 后端库。
# 默认 v3d 后端；mali 目前是空实现（占位），定义 GPU_USE_MALI 才启用。
target("gpud")
set_type("cli")
add_files("gpud.c")
add_deps("v3d", "mali", "foot")
