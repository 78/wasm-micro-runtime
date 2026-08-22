/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"

#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    thread_start_routine_t start;
    void *arg;
    unsigned int stack_size;
} thread_wrapper_arg;

static _Thread_local unsigned int current_wamr_thread_stack_size;

static void
log_thread_stack_profile(unsigned int stack_size)
{
    size_t minimum_free = (size_t)uxTaskGetStackHighWaterMark2(NULL);
    size_t maximum_used = stack_size > minimum_free
                              ? stack_size - minimum_free
                              : 0;
    os_printf("WAMR Guest pthread native stack: configured=%u B, "
              "max-used=%u B, minimum-free=%u B, utilization=%u%%\n",
              stack_size, (unsigned int)maximum_used,
              (unsigned int)minimum_free,
              stack_size > 0
                  ? (unsigned int)(maximum_used * 100U / stack_size)
                  : 0);
}

static void *
os_thread_wrapper(void *arg)
{
    thread_wrapper_arg *targ = arg;
    thread_start_routine_t start_func = targ->start;
    void *thread_arg = targ->arg;
    unsigned int stack_size = targ->stack_size;

#if 0
    os_printf("THREAD CREATED %jx\n", (uintmax_t)(uintptr_t)pthread_self());
#endif
    BH_FREE(targ);
    current_wamr_thread_stack_size = stack_size;
    start_func(thread_arg);
    log_thread_stack_profile(stack_size);
    current_wamr_thread_stack_size = 0;
    return NULL;
}

korp_tid
os_self_thread(void)
{
    /* only allowed if this is a thread, xTaskCreate is not enough look at
     * product_mini for how to use this*/
    return pthread_self();
}

int
os_mutex_init(korp_mutex *mutex)
{
    return pthread_mutex_init(mutex, NULL);
}

int
os_recursive_mutex_init(korp_mutex *mutex)
{
    int ret;

    pthread_mutexattr_t mattr;

    assert(mutex);
    ret = pthread_mutexattr_init(&mattr);
    if (ret)
        return BHT_ERROR;

    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);

    return ret == 0 ? BHT_OK : BHT_ERROR;
}

int
os_mutex_destroy(korp_mutex *mutex)
{
    return pthread_mutex_destroy(mutex);
}

int
os_mutex_lock(korp_mutex *mutex)
{
    return pthread_mutex_lock(mutex);
}

int
os_mutex_unlock(korp_mutex *mutex)
{
    return pthread_mutex_unlock(mutex);
}

int
os_thread_create_with_prio(korp_tid *tid, thread_start_routine_t start,
                           void *arg, unsigned int stack_size, int prio)
{
    pthread_attr_t tattr;
    thread_wrapper_arg *targ;
    esp_pthread_cfg_t parent_cfg;
    esp_pthread_cfg_t child_cfg;
    bool parent_cfg_found;

    assert(stack_size > 0);
    assert(tid);
    assert(start);

    pthread_attr_init(&tattr);
    pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_JOINABLE);
    if (pthread_attr_setstacksize(&tattr, stack_size) != 0) {
        os_printf("Invalid thread stack size %u. Min stack size = %u",
                  stack_size, PTHREAD_STACK_MIN);
        pthread_attr_destroy(&tattr);
        return BHT_ERROR;
    }

    targ = (thread_wrapper_arg *)BH_MALLOC(sizeof(*targ));
    if (!targ) {
        pthread_attr_destroy(&tattr);
        return BHT_ERROR;
    }

    targ->start = start;
    targ->arg = arg;
    targ->stack_size = stack_size;

    /*
     * AOT shared-memory atomics compile to Xtensa S32C1I. Guest linear memory
     * lives in cached PSRAM in this port, so sibling Guest threads must stay on
     * the same core for the compare-and-swap loop to be reliable. Keep Host
     * pthreads unrestricted: only WAMR-created children inherit this affinity.
     */
    parent_cfg_found = esp_pthread_get_cfg(&parent_cfg) == ESP_OK;
    child_cfg = parent_cfg_found ? parent_cfg
                                 : esp_pthread_get_default_config();
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    child_cfg.stack_alloc_caps = MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM;
#endif
    child_cfg.pin_to_core = xPortGetCoreID();
    child_cfg.inherit_cfg = true;
    if (esp_pthread_set_cfg(&child_cfg) != ESP_OK) {
        pthread_attr_destroy(&tattr);
        os_free(targ);
        return BHT_ERROR;
    }

    if (pthread_create(tid, &tattr, os_thread_wrapper, targ) != 0) {
        if (parent_cfg_found)
            (void)esp_pthread_set_cfg(&parent_cfg);
        else {
            esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
            (void)esp_pthread_set_cfg(&default_cfg);
        }
        pthread_attr_destroy(&tattr);
        os_free(targ);
        return BHT_ERROR;
    }

    if (parent_cfg_found)
        (void)esp_pthread_set_cfg(&parent_cfg);
    else {
        esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
        (void)esp_pthread_set_cfg(&default_cfg);
    }

    pthread_attr_destroy(&tattr);
    return BHT_OK;
}

int
os_thread_create(korp_tid *tid, thread_start_routine_t start, void *arg,
                 unsigned int stack_size)
{
    return os_thread_create_with_prio(tid, start, arg, stack_size,
                                      BH_THREAD_DEFAULT_PRIORITY);
}

int
os_thread_join(korp_tid thread, void **retval)
{
    return pthread_join(thread, retval);
}

int
os_thread_detach(korp_tid tid)
{
    return pthread_detach(tid);
}

void
os_thread_exit(void *retval)
{
    if (current_wamr_thread_stack_size > 0) {
        log_thread_stack_profile(current_wamr_thread_stack_size);
        current_wamr_thread_stack_size = 0;
    }
    pthread_exit(retval);
}

int
os_cond_init(korp_cond *cond)
{
    return pthread_cond_init(cond, NULL);
}

int
os_cond_destroy(korp_cond *cond)
{
    return pthread_cond_destroy(cond);
}

int
os_cond_wait(korp_cond *cond, korp_mutex *mutex)
{
    return pthread_cond_wait(cond, mutex);
}

static void
msec_nsec_to_abstime(struct timespec *ts, uint64 usec)
{
    struct timeval tv;
    time_t tv_sec_new;
    long int tv_nsec_new;

    gettimeofday(&tv, NULL);

    tv_sec_new = (time_t)(tv.tv_sec + usec / 1000000);
    if (tv_sec_new >= tv.tv_sec) {
        ts->tv_sec = tv_sec_new;
    }
    else {
        /* integer overflow */
        ts->tv_sec = BH_TIME_T_MAX;
        os_printf("Warning: os_cond_reltimedwait exceeds limit, "
                  "set to max timeout instead\n");
    }

    tv_nsec_new = (long int)(tv.tv_usec * 1000 + (usec % 1000000) * 1000);
    if (tv.tv_usec * 1000 >= tv.tv_usec && tv_nsec_new >= tv.tv_usec * 1000) {
        ts->tv_nsec = tv_nsec_new;
    }
    else {
        /* integer overflow */
        ts->tv_nsec = LONG_MAX;
        os_printf("Warning: os_cond_reltimedwait exceeds limit, "
                  "set to max timeout instead\n");
    }

    if (ts->tv_nsec >= 1000000000L && ts->tv_sec < BH_TIME_T_MAX) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

int
os_cond_reltimedwait(korp_cond *cond, korp_mutex *mutex, uint64 useconds)
{
    int ret;
    struct timespec abstime;

    if (useconds == BHT_WAIT_FOREVER)
        ret = pthread_cond_wait(cond, mutex);
    else {
        msec_nsec_to_abstime(&abstime, useconds);
        ret = pthread_cond_timedwait(cond, mutex, &abstime);
    }

    if (ret != BHT_OK && ret != ETIMEDOUT)
        return BHT_ERROR;

    return ret;
}

int
os_cond_signal(korp_cond *cond)
{
    return pthread_cond_signal(cond);
}

int
os_cond_broadcast(korp_cond *cond)
{
    return pthread_cond_broadcast(cond);
}

int
os_rwlock_init(korp_rwlock *lock)
{
    assert(lock);

    if (pthread_rwlock_init(lock, NULL) != BHT_OK)
        return BHT_ERROR;

    return BHT_OK;
}

int
os_rwlock_rdlock(korp_rwlock *lock)
{
    assert(lock);

    if (pthread_rwlock_rdlock(lock) != BHT_OK)
        return BHT_ERROR;

    return BHT_OK;
}

int
os_rwlock_wrlock(korp_rwlock *lock)
{
    assert(lock);

    if (pthread_rwlock_wrlock(lock) != BHT_OK)
        return BHT_ERROR;

    return BHT_OK;
}

int
os_rwlock_unlock(korp_rwlock *lock)
{
    assert(lock);

    if (pthread_rwlock_unlock(lock) != BHT_OK)
        return BHT_ERROR;

    return BHT_OK;
}

int
os_rwlock_destroy(korp_rwlock *lock)
{
    assert(lock);

    if (pthread_rwlock_destroy(lock) != BHT_OK)
        return BHT_ERROR;

    return BHT_OK;
}
