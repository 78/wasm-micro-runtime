# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set (PLATFORM_SHARED_DIR ${CMAKE_CURRENT_LIST_DIR})

add_definitions(-DBH_PLATFORM_ESP_IDF)

include_directories(${PLATFORM_SHARED_DIR})
include_directories(${PLATFORM_SHARED_DIR}/../include)

file (GLOB_RECURSE source_all ${PLATFORM_SHARED_DIR}/*.c)

include (${CMAKE_CURRENT_LIST_DIR}/../common/libc-util/platform_common_libc_util.cmake)
set (source_all ${source_all} ${PLATFORM_COMMON_LIBC_UTIL_SOURCE})

set (PLATFORM_SHARED_SOURCE ${source_all} ${PLATFORM_COMMON_MATH_SOURCE})

# Executable PSRAM is independent from placing guest linear memory in PSRAM.
# Keep AOT code in internal executable SRAM unless explicitly requested.
if(CONFIG_WAMR_AOT_CODE_IN_PSRAM)
    if(CONFIG_IDF_TARGET_ESP32P4)
        # ESP32-P4 exposes PSRAM in a unified executable address range.  Do
        # not apply the Xtensa I-bus/D-bus address mirror used by S2/S3.
        add_definitions(-DWASM_MEM_EXEC_IN_PSRAM=1)
    elseif(CONFIG_IDF_TARGET_ESP32S2 OR CONFIG_IDF_TARGET_ESP32S3)
        add_definitions(-DWASM_MEM_DUAL_BUS_MIRROR=1)
    else()
        message(FATAL_ERROR "WAMR executable PSRAM is not implemented for ${IDF_TARGET}")
    endif()
endif()
