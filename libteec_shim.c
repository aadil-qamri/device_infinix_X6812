#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define TEEC_SUCCESS               0x00000000
#define TEEC_ERROR_BAD_PARAMETERS   0xFFFF0006
#define TEEC_ERROR_OUT_OF_MEMORY   0xFFFF000C

typedef struct {
    void* buffer;
    size_t size;
    uint32_t flags;
    int32_t id;
    size_t allocated_size;
    void* shadow_buffer;
    int fd;
} Beanpod_TEEC_SharedMemory;

typedef struct {
    int fd;
} TEEC_Context;

typedef uint32_t TEEC_Result;

static int (*real_ioctl)(int, int, ...) = NULL;

int ioctl(int fd, int request, ...) {
    if (!real_ioctl) {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    }

    va_list args;
    va_start(args, request);
    void* argp = va_arg(args, void*);
    va_end(args);

    unsigned int u_req = (unsigned int)request;

    /* Synthesize success for TEE_IOC_SHM_ALLOC since MediaTek driver lacks teedev->pool */
    if (u_req == 0xc010a401 && argp) {
        uint32_t *p32 = (uint32_t *)argp;
        p32[3] = 1;
        return 0;
    }

    return real_ioctl(fd, request, argp);
}

TEEC_Result TEEC_RegisterSharedMemory(TEEC_Context* context, Beanpod_TEEC_SharedMemory* sharedMem) {
    if (!context || !sharedMem || !sharedMem->buffer || sharedMem->size == 0) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    int fd = context->fd;
    size_t size = sharedMem->size;
    if (size == 0) size = 8;

    void* shadow = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shadow == MAP_FAILED) {
        shadow = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shadow == MAP_FAILED) {
            return TEEC_ERROR_OUT_OF_MEMORY;
        }
    }

    memcpy(shadow, sharedMem->buffer, sharedMem->size);

    if (!real_ioctl) {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    }
    int ret = real_ioctl(fd, (int)0x8010a40d, shadow);

    sharedMem->shadow_buffer = shadow;
    sharedMem->allocated_size = size;
    sharedMem->fd = fd;
    sharedMem->id = (ret >= 0) ? ret : 0;

    return TEEC_SUCCESS;
}

void TEEC_ReleaseSharedMemory(Beanpod_TEEC_SharedMemory* sharedMem) {
    if (!sharedMem) return;

    if (sharedMem->shadow_buffer && sharedMem->shadow_buffer != MAP_FAILED) {
        if (sharedMem->buffer) {
            memcpy(sharedMem->buffer, sharedMem->shadow_buffer, sharedMem->size);
        }
        munmap(sharedMem->shadow_buffer, sharedMem->allocated_size);
        sharedMem->shadow_buffer = NULL;
    }
}
