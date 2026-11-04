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

#define TEEC_SUCCESS               0x00000000
#define TEEC_ERROR_BAD_PARAMETERS   0xFFFF0006
#define TEEC_ERROR_OUT_OF_MEMORY   0xFFFF000C

typedef struct {
    void* buffer;
    size_t size;
    uint32_t flags;
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

/* Intercept ioctl calls from libTEECommon to /dev/isee_tee0 (Bionic int __op signature) */
int ioctl(int fd, int request, ...) {
    if (!real_ioctl) {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    }

    va_list args;
    va_start(args, request);
    void* argp = va_arg(args, void*);
    va_end(args);

    int ret = real_ioctl(fd, request, argp);

    /* Log Beanpod TEE ioctl commands (Magic 0xA4) */
    unsigned int u_req = (unsigned int)request;
    if ((u_req & 0xFF00) == 0xA400 || (u_req & 0xFF) == 0xA4) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ioctl(0x%x) ret=%d errno=%d (%s)",
                 u_req, ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "OK");
        klog(msg);
    }
    return ret;
}

static void init_real(void) {
    if (!real_TEEC_AllocateSharedMemory) {
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

TEEC_Result TEEC_AllocateSharedMemory(TEEC_Context* context, TEEC_SharedMemory* sharedMem) {
    init_real();
    if (!sharedMem) return TEEC_ERROR_BAD_PARAMETERS;

    char msg[128];
    snprintf(msg, sizeof(msg), "TEEC_AllocateSharedMemory req: size=%zu, flags=0x%x",
             sharedMem->size, sharedMem->flags);
    klog(msg);

    if (real_TEEC_AllocateSharedMemory) {
        TEEC_Result res = real_TEEC_AllocateSharedMemory(context, sharedMem);
        snprintf(msg, sizeof(msg), "real_TEEC_AllocateSharedMemory res=0x%x buf=%p", res, sharedMem->buffer);
        klog(msg);
        return res;
    }
    return TEEC_ERROR_BAD_PARAMETERS;
}

TEEC_Result TEEC_RegisterSharedMemory(TEEC_Context* context, TEEC_SharedMemory* sharedMem) {
    init_real();
    if (!sharedMem) return TEEC_ERROR_BAD_PARAMETERS;

    char msg[128];
    snprintf(msg, sizeof(msg), "TEEC_RegisterSharedMemory req: buf=%p, size=%zu, flags=0x%x",
             sharedMem->buffer, sharedMem->size, sharedMem->flags);
    klog(msg);

    if (real_TEEC_RegisterSharedMemory) {
        TEEC_Result res = real_TEEC_RegisterSharedMemory(context, sharedMem);
        snprintf(msg, sizeof(msg), "real_TEEC_RegisterSharedMemory res=0x%x", res);
        klog(msg);
        return res;
    }
    return TEEC_ERROR_BAD_PARAMETERS;
}

void TEEC_ReleaseSharedMemory(TEEC_SharedMemory* sharedMem) {
    init_real();
    if (sharedMem && real_TEEC_ReleaseSharedMemory) {
        real_TEEC_ReleaseSharedMemory(sharedMem);
    }
}
