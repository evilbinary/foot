# coding:utf-8
# *******************************************************************
# * Copyright 2021-present evilbinary
# * 作者: evilbinary on 01/01/20
# * 邮箱: rootdebug@163.com
# ********************************************************************
target("tinyusb")
set_type("cli")

add_deps("gaga")
cflags = '  '

source = []
include = [
    'eggs/libgaga',
    'include',
    '.',
    'src',
    'hw',
    'hw/bsp'
]

source += [
    # common
    'src/tusb.c',
    'src/common/tusb_fifo.c',
    # device
    'src/device/usbd.c',
    'src/device/usbd_control.c',
    'src/class/audio/audio_device.c',
    'src/class/cdc/cdc_device.c',
    'src/class/dfu/dfu_device.c',
    'src/class/dfu/dfu_rt_device.c',
    'src/class/hid/hid_device.c',
    'src/class/midi/midi_device.c',
    'src/class/msc/msc_device.c',
    'src/class/net/ecm_rndis_device.c',
    'src/class/net/ncm_device.c',
    'src/class/usbtmc/usbtmc_device.c',
    'src/class/vendor/vendor_device.c',
    'src/class/video/video_device.c',
    # host
    'src/host/usbh.c',
    'src/host/hub.c',
    'src/class/cdc/cdc_host.c',
    'src/class/hid/hid_host.c',
    'src/class/msc/msc_host.c',
    'src/class/vendor/vendor_host.c',
    # typec
    'src/typec/usbc.c',
]

include+=[
    'examples/dual/host_hid_to_device_cdc/src/'
]


if get_plat()=='raspi2':

    include+=[
        'hw/bsp/broadcom_32bit/boards',
        'hw/bsp/broadcom_32bit',
        'hw/bsp/broadcom'
    ]

    source += [
        'examples/dual/host_hid_to_device_cdc/src/main.c',
        'examples/dual/host_hid_to_device_cdc/src/usb_descriptors.c'
    ]

    source += [
        'hw/bsp/broadcom/*.c',
        'hw/bsp/broadcom/gen/*.c',
        'hw/bsp/broadcom_32bit/*.c',
        'src/portable/raspberrypi/rp2/*.c'
    ]

    cflags +=' -std=c99 -DCFG_TUSB_MCU=OPT_MCU_BCM2835 -DBCM_VERSION=2835 -D__ARM_ARCH -D__ARM_ARCH=6 -DCFG_TUD_LOG_LEVEL=0'
    
    add_files(source) 
    add_includedirs(include)
    add_cflags(cflags)

else:
    print('tinyusb todo support')
    pass






