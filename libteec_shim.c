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
static int (*real_ioctl)(int, int, ...) = NULL;

static void klog(const char* msg) {
    FILE* fp = fopen("/dev/kmsg", "w");
    if (fp) {
        fprintf(fp, "[teec_shim] %s\n", msg);
        fclose(fp);
    }
}

int ioctl(int fd, int request, ...) {
    if (!real_ioctl) {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    }

    va_list args;
    va_start(args, request);
    void* argp = va_arg(args, void*);
    va_end(args);

    unsigned int u_req = (unsigned int)request;

    /* Intercept TEE_IOC_SHM_ALLOC (0xc010a401) */
    if (u_req == 0xc010a401 && argp) {
        uint64_t *p64 = (uint64_t *)argp;
        uint32_t *p32 = (uint32_t *)argp;

        char msg[160];
        snprintf(msg, sizeof(msg), "SHM_ALLOC in: size=%llu flags=0x%x id=%d",
                 (unsigned long long)p64[0], p32[2], p32[3]);
        klog(msg);

        /* Kernel 4.14 tee_ioctl_shm_alloc rejects non-zero flags with -EINVAL */
        if (p32[2] != 0) {
            snprintf(msg, sizeof(msg), "Zeroing flags 0x%x -> 0", p32[2]);
            klog(msg);
            p32[2] = 0;
        }

        int ret = real_ioctl(fd, request, argp);
        snprintf(msg, sizeof(msg), "ioctl(0x%x) ret=%d errno=%d (%s) res_id=%d",
                 u_req, ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "OK", p32[3]);
        klog(msg);
        return ret;
    }

    int ret = real_ioctl(fd, request, argp);

    if ((u_req & 0xFF00) == 0xA400 || (u_req & 0xFF) == 0xA4) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ioctl(0x%x) ret=%d errno=%d (%s)",
                 u_req, ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "OK");
        klog(msg);
    }
    return ret;
}

static void init_real(void) {
    if (!real_TEEC_RegisterSharedMemory) {
        void* handle = dlopen("/vendor/lib64/libTEECommon.so", RTLD_NOW | RTLD_GLOBAL);
        if (!handle) handle = dlopen("libTEECommon.so", RTLD_NOW | RTLD_GLOBAL);
        if (handle) {
            real_TEEC_AllocateSharedMemory = dlsym(handle, "TEEC_AllocateSharedMemory");
            real_TEEC_ReleaseSharedMemory = dlsym(handle, "TEEC_ReleaseSharedMemory");
            real_TEEC_RegisterSharedMemory = dlsym(handle, "TEEC_RegisterSharedMemory");
            klog("Hooked real libTEECommon functions");
        } else {
            klog("FAILED to dlopen libTEECommon.so");
        }
    }
}

#define MAX_TRACKED 8
static struct {
    void* orig_buf;
    void* clean_buf;
    size_t size;
    size_t alloc_size;
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

    uintptr_t raw_addr = (uintptr_t)sharedMem->buffer;
    int is_tagged = ((raw_addr >> 56) != 0);
    int is_unaligned = ((raw_addr & 4095) != 0);

    if (!is_tagged && !is_unaligned) {
        if (real_TEEC_RegisterSharedMemory) {
            return real_TEEC_RegisterSharedMemory(context, sharedMem);
        }
    }

    size_t alloc_size = (sharedMem->size + 4095) & ~4095;
    void* clean_buf = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (clean_buf == MAP_FAILED) {
        klog("mmap failed");
        return TEEC_ERROR_OUT_OF_MEMORY;
    }

    clean_buf = (void*)((uintptr_t)clean_buf & 0x00FFFFFFFFFFFFFFULL);

    void* orig_buf = sharedMem->buffer;
    memcpy(clean_buf, orig_buf, sharedMem->size);

    char msg[128];
    snprintf(msg, sizeof(msg), "Substituted tagged %p with clean %p", orig_buf, clean_buf);
    klog(msg);

    sharedMem->buffer = clean_buf;
    TEEC_Result res = TEEC_ERROR_BAD_PARAMETERS;
    if (real_TEEC_RegisterSharedMemory) {
        res = real_TEEC_RegisterSharedMemory(context, sharedMem);
    }

    if (res == TEEC_SUCCESS) {
        for (int i = 0; i < MAX_TRACKED; i++) {
            if (!g_tracked[i].clean_buf) {
                g_tracked[i].orig_buf = orig_buf;
                g_tracked[i].clean_buf = clean_buf;
                g_tracked[i].size = sharedMem->size;
                g_tracked[i].alloc_size = alloc_size;
                g_tracked[i].flags = sharedMem->flags;
                break;
            }
        }
    } else {
        sharedMem->buffer = orig_buf;
        munmap(clean_buf, alloc_size);
    }

    return res;
}

void TEEC_ReleaseSharedMemory(TEEC_SharedMemory* sharedMem) {
    init_real();
    if (!sharedMem || !sharedMem->buffer) return;

    void* current_buf = sharedMem->buffer;
    int idx = -1;

    for (int i = 0; i < MAX_TRACKED; i++) {
        if (g_tracked[i].clean_buf == current_buf) {
            idx = i;
            break;
        }
    }

    if (real_TEEC_ReleaseSharedMemory) {
        real_TEEC_ReleaseSharedMemory(sharedMem);
    }

    if (idx != -1) {
        if (g_tracked[idx].flags & TEEC_MEM_OUTPUT) {
            memcpy(g_tracked[idx].orig_buf, current_buf, g_tracked[idx].size);
        }
        sharedMem->buffer = g_tracked[idx].orig_buf;
        munmap(current_buf, g_tracked[idx].alloc_size);
        memset(&g_tracked[idx], 0, sizeof(g_tracked[idx]));
    }
}
