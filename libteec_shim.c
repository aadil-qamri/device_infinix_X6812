#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

#define TEEC_SUCCESS               0x00000000
#define TEEC_ERROR_BAD_PARAMETERS   0xFFFF0006
#define TEEC_ERROR_OUT_OF_MEMORY   0xFFFF000C
#define TEEC_MEM_INPUT             0x00000001
#define TEEC_MEM_OUTPUT            0x00000002

typedef struct {
    void* buffer;
    size_t size;
    uint32_t flags;
    uint32_t ops;
    uint64_t dummy;
} TEEC_SharedMemory;

typedef struct {
    int fd;
} TEEC_Context;

typedef uint32_t TEEC_Result;

static TEEC_Result (*real_TEEC_AllocateSharedMemory)(TEEC_Context*, TEEC_SharedMemory*) = NULL;
static void (*real_TEEC_ReleaseSharedMemory)(TEEC_SharedMemory*) = NULL;
static TEEC_Result (*real_TEEC_RegisterSharedMemory)(TEEC_Context*, TEEC_SharedMemory*) = NULL;

static void init_real(void) {
    if (!real_TEEC_RegisterSharedMemory) {
        void* handle = dlopen("/vendor/lib64/libTEECommon.so", RTLD_NOW | RTLD_GLOBAL);
        if (!handle) handle = dlopen("libTEECommon.so", RTLD_NOW | RTLD_GLOBAL);
        if (handle) {
            real_TEEC_AllocateSharedMemory = dlsym(handle, "TEEC_AllocateSharedMemory");
            real_TEEC_ReleaseSharedMemory = dlsym(handle, "TEEC_ReleaseSharedMemory");
            real_TEEC_RegisterSharedMemory = dlsym(handle, "TEEC_RegisterSharedMemory");
        }
    }
}

#define MAX_TRACKED 16
static struct {
    void* orig_buf;
    void* aligned_buf;
    size_t size;
    uint32_t flags;
} g_tracked[MAX_TRACKED];

TEEC_Result TEEC_AllocateSharedMemory(TEEC_Context* context, TEEC_SharedMemory* sharedMem) {
    init_real();
    if (real_TEEC_AllocateSharedMemory) {
        return real_TEEC_AllocateSharedMemory(context, sharedMem);
    }
    return TEEC_ERROR_BAD_PARAMETERS;
}

TEEC_Result TEEC_RegisterSharedMemory(TEEC_Context* context, TEEC_SharedMemory* sharedMem) {
    init_real();
    if (!sharedMem || !sharedMem->buffer || sharedMem->size == 0) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    if (((uintptr_t)sharedMem->buffer & 4095) == 0) {
        if (real_TEEC_RegisterSharedMemory) {
            return real_TEEC_RegisterSharedMemory(context, sharedMem);
        }
    }

    size_t aligned_size = (sharedMem->size + 4095) & ~4095;
    void* aligned_ptr = NULL;
    if (posix_memalign(&aligned_ptr, 4096, aligned_size) != 0 || !aligned_ptr) {
        return TEEC_ERROR_OUT_OF_MEMORY;
    }

    void* orig_ptr = sharedMem->buffer;
    memcpy(aligned_ptr, orig_ptr, sharedMem->size);

    sharedMem->buffer = aligned_ptr;
    TEEC_Result res = TEEC_ERROR_BAD_PARAMETERS;
    if (real_TEEC_RegisterSharedMemory) {
        res = real_TEEC_RegisterSharedMemory(context, sharedMem);
    }

    if (res == TEEC_SUCCESS) {
        for (int i = 0; i < MAX_TRACKED; i++) {
            if (!g_tracked[i].aligned_buf) {
                g_tracked[i].orig_buf = orig_ptr;
                g_tracked[i].aligned_buf = aligned_ptr;
                g_tracked[i].size = sharedMem->size;
                g_tracked[i].flags = sharedMem->flags;
                break;
            }
        }
    } else {
        sharedMem->buffer = orig_ptr;
        free(aligned_ptr);
    }
    return res;
}

void TEEC_ReleaseSharedMemory(TEEC_SharedMemory* sharedMem) {
    init_real();
    if (!sharedMem) return;

    void* current_buf = sharedMem->buffer;
    void* orig_buf = NULL;
    int idx = -1;

    for (int i = 0; i < MAX_TRACKED; i++) {
        if (g_tracked[i].aligned_buf == current_buf) {
            orig_buf = g_tracked[i].orig_buf;
            idx = i;
            break;
        }
    }

    if (real_TEEC_ReleaseSharedMemory) {
        real_TEEC_ReleaseSharedMemory(sharedMem);
    }

    if (idx != -1 && orig_buf) {
        if (g_tracked[idx].flags & TEEC_MEM_OUTPUT) {
            memcpy(orig_buf, current_buf, g_tracked[idx].size);
        }
        free(current_buf);
        sharedMem->buffer = orig_buf;
        memset(&g_tracked[idx], 0, sizeof(g_tracked[idx]));
    }
}
