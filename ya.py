# coding:utf-8
# *******************************************************************
# * Copyright 2021-present evilbinary
# * 作者: evilbinary on 01/01/20
# * 邮箱: rootdebug@163.com
# ********************************************************************
foot =get_config('foot')

for f in foot:
    add_subs('./'+f+'/ya.py')
