/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "esp_heap_caps.h"
#if (WASM_MEM_DUAL_BUS_MIRROR != 0) \
    || (WASM_MEM_EXEC_IN_PSRAM != 0) \
    || (WASM_MEM_INTERNAL_DUAL_BUS_MIRROR != 0)
#include "esp_log.h"
#include "esp_memory_utils.h"
#endif
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_ipc.h"
#include "esp_intr_alloc.h"
#include "soc/mmu.h"
#include "rom/cache.h"

#define MEM_DUAL_BUS_OFFSET (SOC_IROM_LOW - SOC_DROM_LOW)

#define in_ibus_ext(addr) \
    (((uint32)addr >= SOC_IROM_LOW) && ((uint32)addr < SOC_IROM_HIGH))

static portMUX_TYPE s_spinlock = portMUX_INITIALIZER_UNLOCKED;
#if defined(CONFIG_ESP_IPC_ENABLE) \
    && (CONFIG_FREERTOS_NUMBER_OF_CORES > 1)
static portMUX_TYPE s_other_core_spinlock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_other_core_parked;
static volatile bool s_release_other_core;

static void IRAM_ATTR
park_other_core_for_cache_maintenance(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_other_core_spinlock);
    esp_intr_noniram_disable();
    s_other_core_parked = true;
    __asm__ __volatile__("memw" ::: "memory");
    while (!s_release_other_core) {
        __asm__ __volatile__("memw\n\tnop" ::: "memory");
    }
    s_other_core_parked = false;
    __asm__ __volatile__("memw" ::: "memory");
    esp_intr_noniram_enable();
    portEXIT_CRITICAL(&s_other_core_spinlock);
}

static void
park_other_core(void)
{
    s_other_core_parked = false;
    s_release_other_core = false;
    __asm__ __volatile__("memw" ::: "memory");
    const uint32_t other_core = xPortGetCoreID() == 0 ? 1U : 0U;
    /* Continuing without the peer parked would make the range invalidate
     * unsafe. IPC is a required part of the dual-core executable-PSRAM
     * contract, so fail closed instead of silently degrading. */
    ESP_ERROR_CHECK(
        esp_ipc_call(other_core, park_other_core_for_cache_maintenance, NULL));
    while (!s_other_core_parked) {
        __asm__ __volatile__("memw\n\tnop" ::: "memory");
    }
}

static void
release_other_core(void)
{
    s_release_other_core = true;
    __asm__ __volatile__("memw" ::: "memory");
    while (s_other_core_parked) {
        __asm__ __volatile__("memw\n\tnop" ::: "memory");
    }
}
#endif

static void IRAM_ATTR
begin_cache_exclusive(void)
{
#if defined(CONFIG_ESP_IPC_ENABLE) \
    && (CONFIG_FREERTOS_NUMBER_OF_CORES > 1)
    park_other_core();
#endif
    portENTER_CRITICAL(&s_spinlock);
    esp_intr_noniram_disable();
}

static void IRAM_ATTR
end_cache_exclusive(void)
{
    esp_intr_noniram_enable();
    portEXIT_CRITICAL(&s_spinlock);
#if defined(CONFIG_ESP_IPC_ENABLE) \
    && (CONFIG_FREERTOS_NUMBER_OF_CORES > 1)
    release_other_core();
#endif
}
#endif

void *
os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    if (prot & MMAP_PROT_EXEC) {
#if (WASM_MEM_DUAL_BUS_MIRROR != 0) \
    || (WASM_MEM_EXEC_IN_PSRAM != 0)
        uint32_t mem_caps = MALLOC_CAP_SPIRAM;
#else
        uint32_t mem_caps = MALLOC_CAP_EXEC;
#endif

        // Memory allocation with MALLOC_CAP_EXEC will return 4-byte aligned
        // Reserve extra 4 byte to fixup alignment and size for the pointer to
        // the originally allocated address
        void *buf_origin =
            heap_caps_malloc(size + 4 + sizeof(uintptr_t), mem_caps);
        if (!buf_origin) {
            return NULL;
        }
        void *buf_fixed = buf_origin + sizeof(void *);
        if ((uintptr_t)buf_fixed & (uintptr_t)0x7) {
            buf_fixed = (void *)((uintptr_t)(buf_fixed + 4) & (~(uintptr_t)7));
        }

        uintptr_t *addr_field = buf_fixed - sizeof(uintptr_t);
        *addr_field = (uintptr_t)buf_origin;
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
        void *exec_ptr = buf_fixed + MEM_DUAL_BUS_OFFSET;
        memset(buf_fixed, 0, size);
        ESP_LOGI("wamr_memmap",
                 "AOT executable allocation: exec=%p write=%p size=%u PSRAM=%s",
                 exec_ptr, buf_fixed, (unsigned)size,
                 esp_ptr_external_ram(buf_fixed) ? "yes" : "no");
        return exec_ptr;
#else
        memset(buf_fixed, 0, size);
#if (WASM_MEM_EXEC_IN_PSRAM != 0)
        ESP_LOGI("wamr_memmap",
                 "AOT executable allocation: ptr=%p size=%u PSRAM=%s executable=%s",
                 buf_fixed, (unsigned)size,
                 esp_ptr_external_ram(buf_fixed) ? "yes" : "no",
                 esp_ptr_executable(buf_fixed) ? "yes" : "no");
#endif
        return buf_fixed;
#endif
    }
    else {
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
        uint32_t mem_caps = MALLOC_CAP_SPIRAM;
#elif CONFIG_WAMR_LINEAR_MEMORY_IN_PSRAM
        /* Non-executable mmap backs Wasm linear memory. The host-managed
         * guest heap is embedded in this same allocation. */
        uint32_t mem_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
        uint32_t mem_caps = MALLOC_CAP_8BIT;
#endif
        void *buf_origin =
            heap_caps_malloc(size + 4 + sizeof(uintptr_t), mem_caps);
        if (!buf_origin) {
            return NULL;
        }

        // Memory allocation with MALLOC_CAP_SPIRAM or MALLOC_CAP_8BIT will
        // return 4-byte aligned Reserve extra 4 byte to fixup alignment and
        // size for the pointer to the originally allocated address
        void *buf_fixed = buf_origin + sizeof(void *);
        if ((uintptr_t)buf_fixed & (uintptr_t)0x7) {
            buf_fixed = (void *)((uintptr_t)(buf_fixed + 4) & (~(uintptr_t)7));
        }

        uintptr_t *addr_field = buf_fixed - sizeof(uintptr_t);
        *addr_field = (uintptr_t)buf_origin;

        memset(buf_fixed, 0, size);
        return buf_fixed;
    }
}

void *
os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    if (old_addr && new_size > old_size) {
        /* os_mmap records the heap allocation origin just below the aligned
         * pointer. When that allocation was reserved larger than the mapped
         * size (WASM_LINEAR_MEMORY_RESERVE_MAX), grow in place so the base
         * address stays stable. */
        uintptr_t *addr_field = (uintptr_t *)old_addr - 1;
        void *buf_origin = (void *)*addr_field;
        size_t offset = (size_t)((char *)old_addr - (char *)buf_origin);
        size_t capacity = heap_caps_get_allocated_size(buf_origin);
        if (capacity >= offset + new_size) {
            memset((char *)old_addr + old_size, 0, new_size - old_size);
            return old_addr;
        }
    }
    return os_mremap_slow(old_addr, old_size, new_size);
}

void
os_munmap(void *addr, size_t size)
{
    char *ptr = (char *)addr;

#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    if (in_ibus_ext(ptr)) {
        ptr -= MEM_DUAL_BUS_OFFSET;
    }
#endif
    // We don't need special handling of the executable allocations
    // here, free() of esp-idf handles it properly
    return os_free(ptr);
}

int
os_mprotect(void *addr, size_t size, int prot)
{
    return 0;
}

void
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    IRAM_ATTR
#endif
os_dcache_flush()
{
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    extern void Cache_WriteBack_All(void);

    begin_cache_exclusive();
    Cache_WriteBack_All();
    end_cache_exclusive();
#endif
}

void
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    IRAM_ATTR
#endif
os_icache_flush(void *start, size_t len)
{
    /* AOT text and relocations are written through the data path. This is
     * required for internal executable D/IRAM as well as executable PSRAM so
     * that instruction fetch observes the final code bytes. */
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    /* ESP32-S2/S3 executable PSRAM is written through its D-bus alias and
     * executed through its I-bus alias. The data writeback and instruction
     * invalidation must both run inside the same kind of cross-core exclusion:
     * range invalidation alone does not stop the other CPU from reading PSRAM
     * concurrently. */
    const size_t line_size = Cache_Get_ICache_Line_Size();
    if (line_size == 0U) {
        return;
    }
    const uintptr_t first = (uintptr_t)start & ~(uintptr_t)(line_size - 1U);
    const uintptr_t last = ((uintptr_t)start + len + line_size - 1U) & ~(uintptr_t)(line_size - 1U);
    begin_cache_exclusive();
    (void)esp_cache_msync((void *)first, last - first,
                          ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    end_cache_exclusive();
#elif defined(BUILD_TARGET_XTENSA)
    __asm__ __volatile__("memw\n\tisync" ::: "memory");
    (void)start;
    (void)len;
#else
    __builtin___clear_cache((char *)start, (char *)start + len);
#endif
}

#if (WASM_MEM_DUAL_BUS_MIRROR != 0) \
    || (WASM_MEM_INTERNAL_DUAL_BUS_MIRROR != 0)
void *
os_get_dbus_mirror(void *ibus)
{
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    if (in_ibus_ext(ibus)) {
        return (void *)((char *)ibus - MEM_DUAL_BUS_OFFSET);
    }
#endif
#if (WASM_MEM_INTERNAL_DUAL_BUS_MIRROR != 0)
    if (esp_ptr_in_diram_iram(ibus)) {
        return esp_ptr_diram_iram_to_dram(ibus);
    }
#endif
    return ibus;
}
#endif
