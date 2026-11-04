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

static void klog(const char* msg) {
    FILE* fp = fopen("/dev/kmsg", "w");
    if (fp) {
        fprintf(fp, "[teec_shim] %s\n", msg);
        fclose(fp);
    }
}

static void* (*real_mmap)(void*, size_t, int, int, int, off_t) = NULL;
static int (*real_ioctl)(int, int, ...) = NULL;

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!real_mmap) {
        real_mmap = dlsym(RTLD_NEXT, "mmap");
    }

    void* res = real_mmap(addr, length, prot, flags, fd, offset);

    /* Trace calls related to device file descriptors */
    if (fd >= 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "mmap(fd=%d, len=%zu, prot=0x%x, flags=0x%x) -> %p (errno=%d %s)",
                 fd, length, prot, flags, res,
                 res == MAP_FAILED ? errno : 0,
                 res == MAP_FAILED ? strerror(errno) : "OK");
        klog(msg);

        /* If /dev/isee_tee0 rejects direct device mmap, fallback to anonymous shared memory */
        if (res == MAP_FAILED && (flags & MAP_SHARED)) {
            void* anon = real_mmap(NULL, length, prot, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            snprintf(msg, sizeof(msg), "Fallback mmap(MAP_ANONYMOUS) -> %p", anon);
            klog(msg);
            if (anon != MAP_FAILED) {
                return anon;
            }
        }
    }

    return res;
}

int ioctl(int fd, int request, ...) {
    if (!real_ioctl) {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    }

    va_list args;
    va_start(args, request);
    void* argp = va_arg(args, void*);
    va_end(args);

    int ret = real_ioctl(fd, request, argp);

    unsigned int u_req = (unsigned int)request;
    if ((u_req & 0xFF00) == 0xA400 || (u_req & 0xFF) == 0xA4) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ioctl(0x%x) ret=%d errno=%d (%s)",
                 u_req, ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "OK");
        klog(msg);
    }
    return ret;
}
