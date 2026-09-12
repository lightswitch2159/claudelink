# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(orangelink LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/ble/ips.c
  src/encoding/4b6b.c
  src/encoding/manchester.c
  src/drivers/rf69/rf69.c
  src/aps/aps.c
  src/subg/subg.c
)

if(CONFIG_ORANGELINK_SX1276)
  target_sources(app PRIVATE src/drivers/sx1276/sx1276.c)
endif()

if(CONFIG_ORANGELINK_LED)
  target_sources(app PRIVATE src/indication/led.c)
endif()

if(CONFIG_ORANGELINK_BATTERY)
  target_sources(app PRIVATE src/battery/battery.c)
endif()

target_include_directories(app PRIVATE src src/encoding src/drivers/rf69 src/aps src/subg
                           src/indication src/battery src/drivers/sx1276)

# Phase 3/4 will add:
#   src/aps/       <- legacy/project/app/src/app_aps.c
#   src/subg/      <- legacy/project/app/src/app_subg.c   (dedicated thread)
#   src/drivers/rf69/  <- legacy/periph/rf69              (SPI + DIO0 interrupts)
#   src/config/    <- legacy/project/app/src/app_config.c (Zephyr Settings)
#
# Do NOT port legacy/userKit/heap -- dead code in the shipping image.
