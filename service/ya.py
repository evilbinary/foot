# coding:utf-8
# *******************************************************************
# * Copyright 2021-present evilbinary
# * 作者: evilbinary on 01/01/20
# * 邮箱: rootdebug@163.com
# ********************************************************************
target("servicea")
set_type("cli")
add_files("service_a.c") 
add_deps("gaga")

target("serviceb")
set_type("cli")
add_files("service_b.c") 
add_deps("gaga")

target("vfsd")
set_type("cli")
add_files("vfsd.c") 
add_deps("gaga")

target("systemd")
set_type("cli")
add_files("systemd.c") 
add_deps("gaga")
