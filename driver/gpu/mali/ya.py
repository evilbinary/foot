# coding:utf-8
# *******************************************************************
# Copyright 2021-present evilbinary
# 作者: evilbinary on 01/01/20
# 邮箱: rootdebug@163.com
# ********************************************************************
# Mali GPU 后端（占位，尚未实现）。实现 gpu_backend_t，由 gpud 按需选用。
target("mali")
set_type("lib")
add_deps("foot", "gpu")
add_files("mali_backend.c")
