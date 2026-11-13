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

/* Beanpod's internal TEEC_SharedMemory layout discovered from disassembly */
typedef struct {
    void* buffer;         /* offset 0 */
    size_t size;          /* offset 8 */
    uint32_t flags;       /* offset 16 */
    int32_t id;           /* offset 20 */
    size_t allocated_size;/* offset 24 */
    void* shadow_buffer;  /* offset 32 */
    int fd;               /* offset 40 */
} Beanpod_TEEC_SharedMemory;

typedef struct {
    int fd;
} TEEC_Context;

typedef uint32_t TEEC_Result;

static int (*real_ioctl)(int, int, ...) = NULL;

static void klog(const char* msg) {
    FILE* fp = fopen("/dev/kmsg", "w");
    if (fp) {
        fprintf(fp, "[teec_shim] %s\n", msg);
        fclose(fp);
    }
}

/* 
 * Intercept ioctl:
 * When generic TEE_IOC_SHM_ALLOC (0xc010a401) is called, synthesize success
 * because MediaTek's driver lacks teedev->pool.
 */
int ioctl(int fd, int request, ...) {
    if (!real_ioctl) {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    }

    va_list args;
    va_start(args, request);
    void* argp = va_arg(args, void*);
    va_end(args);

    unsigned int u_req = (unsigned int)request;

    if (u_req == 0xc010a401 && argp) {
        uint64_t *p64 = (uint64_t *)argp;
        uint32_t *p32 = (uint32_t *)argp;
        char msg[128];
        snprintf(msg, sizeof(msg), "Bypassing TEE_IOC_SHM_ALLOC: size=%llu, assigning id=1", (unsigned long long)p64[0]);
        klog(msg);
        p32[3] = 1; /* fake buffer id */
        return 0;   /* return success (0) */
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

/* 
 * Intercept TEEC_RegisterSharedMemory according to Beanpod's exact protocol 
 */
TEEC_Result TEEC_RegisterSharedMemory(TEEC_Context* context, Beanpod_TEEC_SharedMemory* sharedMem) {
    if (!context || !sharedMem) return TEEC_ERROR_BAD_PARAMETERS;
    if (!sharedMem->buffer || sharedMem->size == 0) return TEEC_ERROR_BAD_PARAMETERS;

    int fd = context->fd;
    size_t size = sharedMem->size;
    if (size == 0) size = 8;

    char msg[160];
    snprintf(msg, sizeof(msg), "RegisterSharedMemory: fd=%d, buf=%p, size=%zu, flags=0x%x",
             fd, sharedMem->buffer, size, sharedMem->flags);
    klog(msg);

    /* Allocate driver shadow buffer on /dev/isee_tee0 */
    void* shadow = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shadow == MAP_FAILED) {
        snprintf(msg, sizeof(msg), "mmap on isee_tee0 failed: errno=%d (%s), trying anonymous fallback",
                 errno, strerror(errno));
        klog(msg);
        shadow = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shadow == MAP_FAILED) {
            return TEEC_ERROR_OUT_OF_MEMORY;
        }
    }

    /* Copy caller's payload into the shadow buffer */
    memcpy(shadow, sharedMem->buffer, sharedMem->size);

    /* Call Beanpod's registration ioctl (0x8010a40d) */
    if (!real_ioctl) {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    }
    int ret = real_ioctl(fd, (int)0x8010a40d, shadow);
    snprintf(msg, sizeof(msg), "ioctl(0x8010a40d, shadow=%p) ret=%d errno=%d",
             shadow, ret, ret < 0 ? errno : 0);
    klog(msg);

    sharedMem->shadow_buffer = shadow;
    sharedMem->allocated_size = size;
    sharedMem->fd = fd;
    sharedMem->id = (ret >= 0) ? ret : 0;

    return TEEC_SUCCESS;
}

/* 
 * Intercept TEEC_ReleaseSharedMemory:
 * Copy verified output token back to original buffer, then munmap shadow buffer.
 */
void TEEC_ReleaseSharedMemory(Beanpod_TEEC_SharedMemory* sharedMem) {
    if (!sharedMem) return;

    if (sharedMem->shadow_buffer && sharedMem->shadow_buffer != MAP_FAILED) {
        klog("ReleaseSharedMemory: copying back output and cleaning shadow");
        if (sharedMem->buffer) {
            memcpy(sharedMem->buffer, sharedMem->shadow_buffer, sharedMem->size);
        }
        munmap(sharedMem->shadow_buffer, sharedMem->allocated_size);
        sharedMem->shadow_buffer = NULL;
    }
}
