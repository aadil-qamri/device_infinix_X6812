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

static void init_real(void) {
    if (!real_TEEC_AllocateSharedMemory) {
        void* handle = dlopen("/vendor/lib64/libTEECommon.so", RTLD_NOW | RTLD_GLOBAL);
        if (!handle) handle = dlopen("libTEECommon.so", RTLD_NOW | RTLD_GLOBAL);
        if (handle) {
            real_TEEC_AllocateSharedMemory = dlsym(handle, "TEEC_AllocateSharedMemory");
            real_TEEC_ReleaseSharedMemory = dlsym(handle, "TEEC_ReleaseSharedMemory");
        }
    }
}

#define MAX_TRACKED 8
static struct {
    void* orig_buf;
    TEEC_SharedMemory allocated_shm;
} g_tracked[MAX_TRACKED];

/* 
 * Intercept AllocateSharedMemory: delegate directly to real vendor 
 */
TEEC_Result TEEC_AllocateSharedMemory(TEEC_Context* context, TEEC_SharedMemory* sharedMem) {
    init_real();
    if (real_TEEC_AllocateSharedMemory) {
        return real_TEEC_AllocateSharedMemory(context, sharedMem);
    }
    return TEEC_ERROR_BAD_PARAMETERS;
}

/* 
 * Intercept RegisterSharedMemory:
 * When microgatekeeper tries to register an unmappable heap buffer,
 * allocate a true TEE driver-registered shared buffer via AllocateSharedMemory.
 */
TEEC_Result TEEC_RegisterSharedMemory(TEEC_Context* context, TEEC_SharedMemory* sharedMem) {
    init_real();
    if (!sharedMem || !sharedMem->buffer || sharedMem->size == 0) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    if (!real_TEEC_AllocateSharedMemory) {
        return TEEC_ERROR_OUT_OF_MEMORY;
    }

    void* orig_buffer = sharedMem->buffer;
    size_t orig_size = sharedMem->size;
    uint32_t orig_flags = sharedMem->flags;

    /* Create driver-backed TEE shared memory */
    TEEC_SharedMemory drv_shm;
    memset(&drv_shm, 0, sizeof(drv_shm));
    drv_shm.size = orig_size;
    drv_shm.flags = orig_flags | TEEC_MEM_INPUT | TEEC_MEM_OUTPUT;

    TEEC_Result res = real_TEEC_AllocateSharedMemory(context, &drv_shm);
    if (res != TEEC_SUCCESS || !drv_shm.buffer) {
        return TEEC_ERROR_OUT_OF_MEMORY;
    }

    /* Copy input data (handle, pattern hash) to the TEE shared buffer */
    memcpy(drv_shm.buffer, orig_buffer, orig_size);

    /* Track the association so we can copy output back on release */
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (!g_tracked[i].orig_buf) {
            g_tracked[i].orig_buf = orig_buffer;
            g_tracked[i].allocated_shm = drv_shm;
            break;
        }
    }

    /* Pass the valid driver-backed shared memory to Gatekeeper */
    *sharedMem = drv_shm;
    return TEEC_SUCCESS;
}

/* 
 * Intercept ReleaseSharedMemory:
 * Copy verified auth token back to Gatekeeper's buffer, then free driver memory.
 */
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
        /* Copy output results (auth token) back to Gatekeeper */
        memcpy(g_tracked[idx].orig_buf, sharedMem->buffer, g_tracked[idx].allocated_shm.size);
        
        /* Free through real vendor TEE allocator */
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
