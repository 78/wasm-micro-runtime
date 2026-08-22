/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#if (WASM_MEM_EXEC_IN_PSRAM != 0)
#include "esp_log.h"
#include "esp_memory_utils.h"
#endif
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
#include "soc/mmu.h"
#include "rom/cache.h"

#define MEM_DUAL_BUS_OFFSET (SOC_IROM_LOW - SOC_IROM_HIGH)

#define in_ibus_ext(addr) \
    (((uint32)addr >= SOC_IROM_LOW) && ((uint32)addr < SOC_IROM_HIGH))

static portMUX_TYPE s_spinlock = portMUX_INITIALIZER_UNLOCKED;
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
        memset(buf_fixed + MEM_DUAL_BUS_OFFSET, 0, size);
        return buf_fixed + MEM_DUAL_BUS_OFFSET;
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
    uint32_t preload;
    extern void Cache_WriteBack_All(void);

    portENTER_CRITICAL(&s_spinlock);

    Cache_WriteBack_All();
    preload = Cache_Disable_ICache();
    Cache_Enable_ICache(preload);

    portEXIT_CRITICAL(&s_spinlock);
#endif
}

void
os_icache_flush(void *start, size_t len)
{
#if (WASM_MEM_EXEC_IN_PSRAM != 0)
    /* P4 has a unified PSRAM address range, but instruction fetch still must
     * observe the code bytes and relocations written through the data path. */
    __builtin___clear_cache((char *)start, (char *)start + len);
#else
    (void)start;
    (void)len;
#endif
}

#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
void *
os_get_dbus_mirror(void *ibus)
{
    if (in_ibus_ext(ibus)) {
        return (void *)((char *)ibus - MEM_DUAL_BUS_OFFSET);
    }
    else {
        return ibus;
    }
}
#endif
