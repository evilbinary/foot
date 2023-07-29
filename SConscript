# coding:utf-8
# *******************************************************************
# * Copyright 2021-2080 evilbinary
# * 作者: evilbinary on 01/01/20
# * 邮箱: rootdebug@163.com
# ********************************************************************
import os
import platform
import copy
from xenv.env import add_libc

Import('appEnv')

env=appEnv

current = Dir('.').srcnode().path

add_libc(env)

returns=[]


if env.get('APP'):
    SConscript(dirs=['driver'], exports='env')
    SConscript(dirs=['service'], exports='env')
else:
    pass

Export('appEnv')

Return('returns')
