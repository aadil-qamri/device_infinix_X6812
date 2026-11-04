#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

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

static void klog(const char* msg) {
    FILE* fp = fopen("/dev/kmsg", "w");
    if (fp) {
        fprintf(fp, "[teec_shim] %s\n", msg);
        fclose(fp);
    }
}

static void init_real(void) {
    if (!real_TEEC_AllocateSharedMemory) {
        void* handle = dlopen("/vendor/lib64/libTEECommon.so", RTLD_NOW | RTLD_GLOBAL);
        if (!handle) handle = dlopen("libTEECommon.so", RTLD_NOW | RTLD_GLOBAL);
        if (handle) {
            real_TEEC_AllocateSharedMemory = dlsym(handle, "TEEC_AllocateSharedMemory");
            real_TEEC_ReleaseSharedMemory = dlsym(handle, "TEEC_ReleaseSharedMemory");
            klog("Hooked real libTEECommon functions");
        } else {
            klog("FAILED to dlopen libTEECommon.so");
        }
    }
}

#define MAX_TRACKED 8
static struct {
    void* orig_buf;
    TEEC_SharedMemory allocated_shm;
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
    char log_buf[128];

    if (!sharedMem || !sharedMem->buffer || sharedMem->size == 0) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    if (!real_TEEC_AllocateSharedMemory) {
        klog("real_TEEC_AllocateSharedMemory is NULL");
        return TEEC_ERROR_OUT_OF_MEMORY;
    }

    void* orig_buffer = sharedMem->buffer;
    size_t orig_size = sharedMem->size;

    TEEC_SharedMemory drv_shm;
    memset(&drv_shm, 0, sizeof(drv_shm));
    drv_shm.size = orig_size;
    drv_shm.flags = sharedMem->flags | TEEC_MEM_INPUT | TEEC_MEM_OUTPUT;

    TEEC_Result res = real_TEEC_AllocateSharedMemory(context, &drv_shm);
    snprintf(log_buf, sizeof(log_buf), "real_TEEC_AllocateSharedMemory returned: 0x%08x, buf=%p", res, drv_shm.buffer);
    klog(log_buf);

    if (res != TEEC_SUCCESS || !drv_shm.buffer) {
        return res != TEEC_SUCCESS ? res : TEEC_ERROR_OUT_OF_MEMORY;
    }

    memcpy(drv_shm.buffer, orig_buffer, orig_size);

    for (int i = 0; i < MAX_TRACKED; i++) {
        if (!g_tracked[i].orig_buf) {
            g_tracked[i].orig_buf = orig_buffer;
            g_tracked[i].allocated_shm = drv_shm;
            break;
        }
    }

    *sharedMem = drv_shm;
    return TEEC_SUCCESS;
}

void TEEC_ReleaseSharedMemory(TEEC_SharedMemory* sharedMem) {
    init_real();
    if (!sharedMem || !sharedMem->buffer) return;

    int idx = -1;
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (g_tracked[i].allocated_shm.buffer == sharedMem->buffer) {
            idx = i;
            break;
        }
    }

    if (idx != -1 && g_tracked[idx].orig_buf) {
        memcpy(g_tracked[idx].orig_buf, sharedMem->buffer, g_tracked[idx].allocated_shm.size);
        if (real_TEEC_ReleaseSharedMemory) {
            real_TEEC_ReleaseSharedMemory(&g_tracked[idx].allocated_shm);
        }
        sharedMem->buffer = g_tracked[idx].orig_buf;
        memset(&g_tracked[idx], 0, sizeof(g_tracked[idx]));
    } else {
        if (real_TEEC_ReleaseSharedMemory) {
            real_TEEC_ReleaseSharedMemory(sharedMem);
        }
    }
}
