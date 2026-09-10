# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(orangelink LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/ble/ips.c
  src/encoding/4b6b.c
  src/encoding/manchester.c
)

target_include_directories(app PRIVATE src src/encoding)

# Phase 3/4 will add:
#   src/aps/       <- legacy/project/app/src/app_aps.c
#   src/subg/      <- legacy/project/app/src/app_subg.c   (dedicated thread)
#   src/drivers/rf69/  <- legacy/periph/rf69              (SPI + DIO0 interrupts)
#   src/config/    <- legacy/project/app/src/app_config.c (Zephyr Settings)
#
# Do NOT port legacy/userKit/heap -- dead code in the shipping image.
