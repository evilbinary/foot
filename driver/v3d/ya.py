# coding:utf-8
# *******************************************************************
# * Copyright 2021-present evilbinary
# * 作者: evilbinary on 01/01/20
# * 邮箱: rootdebug@163.com
# ********************************************************************
# raspi5 V3D（VideoCore VII）GPU 驱动 · foot 服务端（v3dd）。
# 用户态守护进程：经 capability 映射 V3D/PM 窗口，跑 CSD 2D 内核，
# 通过 foot 共享页协议给合成器/应用提供 2D 操作。
target("v3dd")
set_type("cli")
add_deps("foot")
add_files(
    'v3dd.c',
    'v3d_hw.c'
)
