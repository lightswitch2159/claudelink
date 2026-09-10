# SPDX-License-Identifier: GPL-2.0-only
#
# STATUS: scaffolding. src/ is empty — Phase 0 produced no application code.
# See docs/FEASIBILITY_REPORT.md; Phase 1 must not start before blockers B1-B3
# clear.
#
# NOTE ON LICENSE: the SPDX line above reflects the upstream application headers
# (GPL v2 only). Whether a GPL-2.0-only application may be combined and
# distributed with Apache-2.0 NCS/Zephyr code is an unresolved legal blocker —
# FEASIBILITY_REPORT.md §B2. Do not treat this header as a settled decision.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(orangelink LANGUAGES C)

target_sources(app PRIVATE
  # src/main.c
)

# Phase 3/4 will add the ported trees. Kept here as a map of what is coming:
#
#   src/ble/          <- legacy/lib/pump/ble_services/ble_ips  (BT_GATT_SERVICE_DEFINE)
#   src/aps/          <- legacy/project/app/src/app_aps.c
#   src/subg/         <- legacy/project/app/src/app_subg.c     (dedicated thread)
#   src/drivers/rf69/ <- legacy/periph/rf69                    (Zephyr SPI + DIO interrupts)
#   src/encoding/     <- legacy/lib/pump/encrypt/{4b6b,manchester}.c  (port verbatim)
#   src/config/       <- legacy/project/app/src/app_config.c   (Zephyr Settings)
#
# Do NOT port legacy/userKit/heap — it is dead code in the shipping image
# (zero references in app.map). See docs/legacy-flash-budget.md.
