# coding:utf-8
# *******************************************************************
# * Copyright 2021-present evilbinary
# * 作者: evilbinary on 01/01/20
# * 邮箱: rootdebug@163.com
# ********************************************************************
target("usbd")
set_type("cli")
add_deps("gaga")

cflags = '  '

source = []
include = [
    'eggs/libgaga',
    'include',
    '.',
    'src',
    'include/device',
    'include/device/hid',
    'include/hcd',
    'include/hcd/dwc',

    'include/platform',
    'include/platform/arm',
    'include/usbd',
    '../../service'

]

source += [
    'source/device/hub.c',
    'source/device/hid/*.c',

    'source/hcd/dwc/*.c',

    'source/platform/arm/*.c',
    'source/platform/platform.c',

    'source/usbd/usbd.c',

    'source/configuration.c',

    'main.c'
    
]

if get_plat()=='raspi2':

    cflags+='-DTARGET_RPI -DDEBUG -DTYPE_LOWLEVEL -DLIB_BCM2835 -DLIB_ARM_V6 -DLIB_HID -DLIB_KBD -DLIB_MOUSE '

    add_files(source) 
    add_includedirs(include)
    add_cflags(cflags)

else:
    print('csudusb todo support')
    pass