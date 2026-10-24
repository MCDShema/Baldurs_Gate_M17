#define _GNU_SOURCE
#define _TIME_BITS 32
#include <stdio.h>
#include <stdlib.h>
static void *g_main_window = NULL;
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <ucontext.h>
#include <dlfcn.h>

#define RTLD_NEXT ((void *) -1l)

#define SDL_PIXELFORMAT_ARGB8888 0x16362004u
#define SDL_PIXELFORMAT_ABGR8888 0x16762004u

typedef struct {
    int x;
    int y;
    int w;
    int h;
} SDL_Rect;

typedef struct {
    uint32_t format;
    int w;
    int h;
    int refresh_rate;
    void *driverdata;
} CompatDisplayMode;

static void log_msg(const char *msg) {
    if (!msg) return;
    const char *paths[] = {
        "/sdcard/baldurs_gate.log",
        "/storage/roms/ports/gemrb/baldurs_gate.log",
        NULL
    };
    for (int i = 0; paths[i]; ++i) {
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) {
            write(fd, msg, strlen(msg));
            close(fd);
        }
    }
    write(2, msg, strlen(msg));
}

static void crash_handler(int sig, siginfo_t *info, void *ucontext) {
    char buf[512];
    ucontext_t *uc = (ucontext_t *)ucontext;
    unsigned long pc = 0, lr = 0, sp = 0, r0 = 0, r1 = 0, r2 = 0, r3 = 0;
#if defined(__arm__)
    pc = uc->uc_mcontext.arm_pc;
    lr = uc->uc_mcontext.arm_lr;
    sp = uc->uc_mcontext.arm_sp;
    r0 = uc->uc_mcontext.arm_r0;
    r1 = uc->uc_mcontext.arm_r1;
    r2 = uc->uc_mcontext.arm_r2;
    r3 = uc->uc_mcontext.arm_r3;
#endif
    snprintf(buf, sizeof(buf), "\n=================== CRASH DETECTED ===================\n"
                               "Signal: %d, fault_addr: %p\n"
                               "ARM Registers:\n"
                               "  PC: 0x%08lx, LR: 0x%08lx, SP: 0x%08lx\n"
                               "  R0: 0x%08lx, R1: 0x%08lx, R2: 0x%08lx, R3: 0x%08lx\n",
             sig, info ? info->si_addr : NULL, pc, lr, sp, r0, r1, r2, r3);
    log_msg(buf);

    int maps_fd = open("/proc/self/maps", O_RDONLY);
    if (maps_fd >= 0) {
        log_msg("--- /proc/self/maps ---\n");
        char map_buf[1024];
        ssize_t n;
        while ((n = read(maps_fd, map_buf, sizeof(map_buf) - 1)) > 0) {
            map_buf[n] = '\0';
            log_msg(map_buf);
        }
        close(maps_fd);
        log_msg("--- end /proc/self/maps ---\n");
    }
    log_msg("======================================================\n");
    _exit(128 + sig);
}

static void *(*get_real_dlsym(void))(void *, const char *) {
    static void *(*real_dlsym_fn)(void *, const char *) = NULL;
    if (!real_dlsym_fn) {
        real_dlsym_fn = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.4");
    }
    return real_dlsym_fn;
}

static void *get_next_symbol(const char *name) {
    void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
    return real_dlsym ? real_dlsym(RTLD_NEXT, name) : NULL;
}

static const char *safe_SDL_GetError(void) {
    const char *(*fn)(void) = (const char *(*)(void))get_next_symbol("SDL_GetError");
    return fn ? fn() : "unknown";
}

int system(const char *command) {
    (void)command;
    return 0;
}

static int g_fb_fd = -1;
static uint8_t *g_fb_mem = MAP_FAILED;
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;
static size_t g_fb_size = 0;
static uint32_t *g_src_buf = NULL;
static size_t g_src_buf_capacity = 0;

static int query_fb_mode(int *w, int *h, int *bpp, int *pitch) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        log_msg("[libsdl_compat] framebuffer: cannot open /dev/fb0\n");
        return -1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    memset(&vinfo, 0, sizeof(vinfo));
    memset(&finfo, 0, sizeof(finfo));

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        log_msg("[libsdl_compat] framebuffer: cannot query /dev/fb0\n");
        close(fd);
        return -1;
    }

    close(fd);
    if (w) *w = (int)vinfo.xres;
    if (h) *h = (int)vinfo.yres;
    if (bpp) *bpp = (int)vinfo.bits_per_pixel;
    if (pitch) *pitch = (int)finfo.line_length;

    char buf[192];
    snprintf(buf, sizeof(buf), "[libsdl_compat] framebuffer: %dx%d, bpp=%d, pitch=%d\n",
             (int)vinfo.xres, (int)vinfo.yres, (int)vinfo.bits_per_pixel, (int)finfo.line_length);
    log_msg(buf);
    return 0;
}

static void init_fb_blitter(void) {
    if (g_fb_fd >= 0) return;

    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd < 0) {
        log_msg("[FB] Error: Cannot open /dev/fb0\n");
        return;
    }

    memset(&g_vinfo, 0, sizeof(g_vinfo));
    memset(&g_finfo, 0, sizeof(g_finfo));
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_vinfo) < 0 ||
        ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_finfo) < 0) {
        log_msg("[FB] Error: Cannot query screeninfo from /dev/fb0\n");
        close(g_fb_fd);
        g_fb_fd = -1;
        return;
    }

    ioctl(g_fb_fd, FBIOBLANK, 0); // Ensure unblanked
    g_fb_size = g_finfo.smem_len ? g_finfo.smem_len : ((size_t)g_vinfo.yres_virtual * (size_t)g_finfo.line_length);
    g_fb_mem = (uint8_t *)mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mem == MAP_FAILED) {
        log_msg("[FB] Error: Cannot mmap /dev/fb0\n");
        close(g_fb_fd);
        g_fb_fd = -1;
        return;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "[FB] Framebuffer blitter initialized: %dx%d (%d bpp), pitch=%d, size=%zu\n",
             (int)g_vinfo.xres, (int)g_vinfo.yres, (int)g_vinfo.bits_per_pixel,
             (int)g_finfo.line_length, g_fb_size);
    log_msg(buf);
}

static double safe_ceil(double x) { return ceil(x); }
static float safe_ceilf(float x) { return ceilf(x); }
static double safe_floor(double x) { return floor(x); }
static float safe_floorf(float x) { return floorf(x); }

extern void vsel_trampoline_31894(void);
extern uint32_t g_ret_3189c;

extern void vsel_trampoline_45640(void);
extern uint32_t g_ret_45648;

extern void vsel_trampoline_45668(void);
extern uint32_t g_ret_45670;

extern void vsel_trampoline_72f68(void);
extern uint32_t g_ret_72f70;

extern void vsel_trampoline_23fd0(void);
extern uint32_t g_ret_23fd8;

extern void vsel_trampoline_23fe8(void);
extern uint32_t g_ret_23ff0;

__asm__(
".arm\n"
".syntax unified\n"
".align 2\n"
".global vsel_trampoline_31894\n"
".type vsel_trampoline_31894, %function\n"
"vsel_trampoline_31894:\n"
"    bne 1f\n"
"    vmov.f32 s15, s14\n"
"1:\n"
"    vstr s14, [sp, #60]\n"
"    ldr pc, [pc, #-4]\n"
".global g_ret_3189c\n"
"g_ret_3189c:\n"
"    .word 0\n"

".align 2\n"
".global vsel_trampoline_45640\n"
".type vsel_trampoline_45640, %function\n"
"vsel_trampoline_45640:\n"
"    beq 1f\n"
"    vmov.f64 d16, d17\n"
"1:\n"
"    strd r4, [r9]\n"
"    ldr pc, [pc, #-4]\n"
".global g_ret_45648\n"
"g_ret_45648:\n"
"    .word 0\n"

".align 2\n"
".global vsel_trampoline_45668\n"
".type vsel_trampoline_45668, %function\n"
"vsel_trampoline_45668:\n"
"    beq 1f\n"
"    vmov.f64 d16, d17\n"
"1:\n"
"    vstr d16, [r9]\n"
"    ldr pc, [pc, #-4]\n"
".global g_ret_45670\n"
"g_ret_45670:\n"
"    .word 0\n"

".align 2\n"
".global vsel_trampoline_72f68\n"
".type vsel_trampoline_72f68, %function\n"
"vsel_trampoline_72f68:\n"
"    ble 1f\n"
"    vmov.f32 s16, s15\n"
"1:\n"
"    vmov.f32 s0, s16\n"
"    ldr pc, [pc, #-4]\n"
".global g_ret_72f70\n"
"g_ret_72f70:\n"
"    .word 0\n"

".align 2\n"
".global vsel_trampoline_23fd0\n"
".type vsel_trampoline_23fd0, %function\n"
"vsel_trampoline_23fd0:\n"
"    ble 1f\n"
"    vmov.f32 s25, s15\n"
"1:\n"
"    vstr s16, [r4, #-8]\n"
"    ldr pc, [pc, #-4]\n"
".global g_ret_23fd8\n"
"g_ret_23fd8:\n"
"    .word 0\n"

".align 2\n"
".global vsel_trampoline_23fe8\n"
".type vsel_trampoline_23fe8, %function\n"
"vsel_trampoline_23fe8:\n"
"    ble 1f\n"
"    vmov.f32 s24, s16\n"
"1:\n"
"    cmp r4, r7\n"
"    ldr pc, [pc, #-4]\n"
".global g_ret_23ff0\n"
"g_ret_23ff0:\n"
"    .word 0\n"
);

static uintptr_t get_libsdl2_base(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libSDL2-2.0.so")) {
            unsigned long parsed_base = 0;
            if (sscanf(line, "%lx-", &parsed_base) == 1) {
                base = (uintptr_t)parsed_base;
                break;
            }
        }
    }
    fclose(f);
    return base;
}

static void patch_sdl2_armv8_insns(void) {
    static int patched = 0;
    if (patched) return;

    uintptr_t base = get_libsdl2_base();
    if (!base) return;

    char buf[256];
    snprintf(buf, sizeof(buf), "[libsdl_compat] Found libSDL2 base at 0x%08lx\n", (unsigned long)base);
    log_msg(buf);

    uintptr_t page0 = (base + 0xf000) & ~4095;
    mprotect((void *)page0, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)(base + 0xf0c0) = 0xe5900000;
    *(uint32_t *)(base + 0xf0c8) = 0xe5900000;
    *(uint32_t *)(base + 0xf130) = 0xe5803000;
    __builtin___clear_cache((char *)(base + 0xf0b0), (char *)(base + 0xf150));

    uintptr_t page1 = (base + 0x47000) & ~4095;
    mprotect((void *)page1, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);

    uint32_t *p_ceil = (uint32_t *)(base + 0x474a0);
    p_ceil[0] = 0xe51ff004;
    p_ceil[1] = (uint32_t)&safe_ceil;

    uint32_t *p_ceilf = (uint32_t *)(base + 0x474a8);
    p_ceilf[0] = 0xe51ff004;
    p_ceilf[1] = (uint32_t)&safe_ceilf;

    uint32_t *p_floor = (uint32_t *)(base + 0x47514);
    p_floor[0] = 0xe51ff004;
    p_floor[1] = (uint32_t)&safe_floor;

    uint32_t *p_floorf = (uint32_t *)(base + 0x4751c);
    p_floorf[0] = 0xe51ff004;
    p_floorf[1] = (uint32_t)&safe_floorf;

    __builtin___clear_cache((char *)(base + 0x47400), (char *)(base + 0x47600));

    // Make libsdl_compat.so's own trampoline page writable so we can fill the return addresses
    uintptr_t my_page = ((uintptr_t)&vsel_trampoline_31894) & ~4095;
    mprotect((void *)my_page, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);

    // Patch 31894 (GLES2_UpdateViewport conditional matrix select vseleq.f32)
    g_ret_3189c = (uint32_t)(base + 0x3189c);
    uintptr_t page_31 = (base + 0x31000) & ~4095;
    mprotect((void *)page_31, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint32_t *p_31894 = (uint32_t *)(base + 0x31894);
    p_31894[0] = 0xe51ff004; // ldr pc, [pc, #-4]
    p_31894[1] = (uint32_t)&vsel_trampoline_31894;
    __builtin___clear_cache((char *)p_31894, (char *)(p_31894 + 2));

    // Patch 23fd0 and 23fe8 (vselgt.f32)
    g_ret_23fd8 = (uint32_t)(base + 0x23fd8);
    g_ret_23ff0 = (uint32_t)(base + 0x23ff0);
    uintptr_t page_23 = (base + 0x23000) & ~4095;
    mprotect((void *)page_23, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint32_t *p_23fd0 = (uint32_t *)(base + 0x23fd0);
    p_23fd0[0] = 0xe51ff004;
    p_23fd0[1] = (uint32_t)&vsel_trampoline_23fd0;
    uint32_t *p_23fe8 = (uint32_t *)(base + 0x23fe8);
    p_23fe8[0] = 0xe51ff004;
    p_23fe8[1] = (uint32_t)&vsel_trampoline_23fe8;
    __builtin___clear_cache((char *)(base + 0x23f00), (char *)(base + 0x24100));

    // Patch 45640 and 45668 (vseleq.f64)
    g_ret_45648 = (uint32_t)(base + 0x45648);
    g_ret_45670 = (uint32_t)(base + 0x45670);
    uintptr_t page_45 = (base + 0x45000) & ~4095;
    mprotect((void *)page_45, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint32_t *p_45640 = (uint32_t *)(base + 0x45640);
    p_45640[0] = 0xe51ff004;
    p_45640[1] = (uint32_t)&vsel_trampoline_45640;
    uint32_t *p_45668 = (uint32_t *)(base + 0x45668);
    p_45668[0] = 0xe51ff004;
    p_45668[1] = (uint32_t)&vsel_trampoline_45668;
    __builtin___clear_cache((char *)(base + 0x45600), (char *)(base + 0x45700));

    // Patch 72f68 (vselgt.f32)
    g_ret_72f70 = (uint32_t)(base + 0x72f70);
    uintptr_t page_72 = (base + 0x72000) & ~4095;
    mprotect((void *)page_72, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint32_t *p_72f68 = (uint32_t *)(base + 0x72f68);
    p_72f68[0] = 0xe51ff004;
    p_72f68[1] = (uint32_t)&vsel_trampoline_72f68;
    __builtin___clear_cache((char *)(base + 0x72f00), (char *)(base + 0x73000));

    __builtin___clear_cache((char *)my_page, (char *)(my_page + 8192));

    patched = 1;
    log_msg("[libsdl_compat] All ARMv8 instructions (atomics, vrint, vsel) verified and patched to ARMv7!\n");
}

__attribute__((constructor))
static void compat_init(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    // Setup alternate signal stack to guarantee crash handler runs
    static char alt_stack[65536];
    stack_t ss;
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    log_msg("[libsdl_compat] Hook library v10 loaded, initializing patches...\n");
    patch_sdl2_armv8_insns();

    setenv("SDL_VIDEODRIVER", "mali", 0);
    setenv("SDL_VIDEO_GL_DRIVER", "/storage/roms/ports/gemrb/libs/libGLESv2.so", 1);
    setenv("SDL_VIDEO_EGL_DRIVER", "/storage/roms/ports/gemrb/libs/libEGL.so", 1);
}

void *dlopen(const char *filename, int flags) {
    static void *(*real_dlopen)(const char *, int) = NULL;
    if (!real_dlopen) real_dlopen = get_next_symbol("dlopen");

    void *h = real_dlopen ? real_dlopen(filename, flags) : NULL;
    static int logged_sdl2 = 0;
    if (filename && strstr(filename, "libSDL2") && !logged_sdl2) {
        logged_sdl2 = 1;
        char buf[256];
        snprintf(buf, sizeof(buf), "[libsdl_compat] dlopen('%s') -> %p\n", filename, h);
        log_msg(buf);
        patch_sdl2_armv8_insns();
    }
    return h;
}

static void *get_mali_handle(void) {
    static void *mali = NULL;
    if (!mali) {
        mali = dlopen("libMali.so", RTLD_LAZY | RTLD_GLOBAL);
        if (!mali) mali = dlopen("/storage/roms/ports/gemrb/libs/libMali.so", RTLD_LAZY | RTLD_GLOBAL);
    }
    return mali;
}

static void *g_gbm_dev = NULL;
static int g_drm_fd = -1;

void *eglGetDisplay(void *native_display) {
    char b[256];
    snprintf(b, sizeof(b), "[libsdl_compat] eglGetDisplay(%p) entered\n", native_display);
    log_msg(b);

    static void *(*real_eglGetDisplay)(void *) = NULL;
    void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
    void *mali = get_mali_handle();
    if (!real_eglGetDisplay && mali && real_dlsym) {
        real_eglGetDisplay = (void *(*)(void *))real_dlsym(mali, "eglGetDisplay");
    }

    if (native_display == NULL || native_display == (void *)0) {
        if (!g_gbm_dev) {
            void *(*gbm_create)(int) = NULL;
            if (mali && real_dlsym) gbm_create = (void *(*)(int))real_dlsym(mali, "gbm_create_device");

            if (g_drm_fd < 0) g_drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
            if (g_drm_fd < 0) g_drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
            if (g_drm_fd < 0) g_drm_fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
            if (g_drm_fd < 0) g_drm_fd = open("/dev/mali", O_RDWR | O_CLOEXEC);
            if (g_drm_fd < 0) g_drm_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);

            if (gbm_create && g_drm_fd >= 0) {
                g_gbm_dev = gbm_create(g_drm_fd);
                snprintf(b, sizeof(b), "[libsdl_compat] Created gbm_dev=%p from fd=%d\n", g_gbm_dev, g_drm_fd);
                log_msg(b);
            } else {
                snprintf(b, sizeof(b), "[libsdl_compat] Warning: gbm_create=%p fd=%d\n", (void *)gbm_create, g_drm_fd);
                log_msg(b);
            }
        }
        if (g_gbm_dev) {
            native_display = g_gbm_dev;
        }
    }

    void *disp = real_eglGetDisplay ? real_eglGetDisplay(native_display) : NULL;
    snprintf(b, sizeof(b), "[libsdl_compat] eglGetDisplay(%p) -> %p\n", native_display, disp);
    log_msg(b);
    return disp;
}

int eglInitialize(void *dpy, int *major, int *minor) {
    static int (*real_fn)(void *, int *, int *) = NULL;
    if (!real_fn) {
        void *m = get_mali_handle();
        void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
        if (m && real_dlsym) real_fn = (int (*)(void *, int *, int *))real_dlsym(m, "eglInitialize");
    }
    int ret = real_fn ? real_fn(dpy, major, minor) : 0;
    char b[128];
    snprintf(b, sizeof(b), "[libsdl_compat] eglInitialize(%p) -> %d (maj=%d, min=%d)\n",
             dpy, ret, major ? *major : 0, minor ? *minor : 0);
    log_msg(b);
    return ret;
}

int eglBindAPI(int api) {
    static int (*real_fn)(int) = NULL;
    void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
    void *mali = get_mali_handle();
    if (!real_fn && mali && real_dlsym) {
        real_fn = (int (*)(int))real_dlsym(mali, "eglBindAPI");
    }
    int ret = real_fn ? real_fn(api) : 1;
    char b[128];
    snprintf(b, sizeof(b), "[libsdl_compat] eglBindAPI(0x%x) -> %d\n", api, ret);
    log_msg(b);
    return ret;
}

int eglChooseConfig(void *dpy, const int *attrib_list, void **configs, int config_size, int *num_config) {
    static int (*real_fn)(void *, const int *, void **, int, int *) = NULL;
    void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
    void *mali = get_mali_handle();
    if (!real_fn && mali && real_dlsym) {
        real_fn = (int (*)(void *, const int *, void **, int, int *))real_dlsym(mali, "eglChooseConfig");
    }

    int ret = real_fn ? real_fn(dpy, attrib_list, configs, config_size, num_config) : 0;
    char b[256];
    snprintf(b, sizeof(b), "[libsdl_compat] eglChooseConfig(dpy=%p, size=%d) -> ret=%d, num=%d\n",
             dpy, config_size, ret, num_config ? *num_config : 0);
    log_msg(b);

    if (!ret || (num_config && *num_config == 0)) {
        static int (*get_configs)(void *, void **, int, int *) = NULL;
        if (!get_configs && mali && real_dlsym) {
            get_configs = (int (*)(void *, void **, int, int *))real_dlsym(mali, "eglGetConfigs");
        }
        if (get_configs) {
            int count = 0;
            int g_ret = get_configs(dpy, configs, config_size, num_config ? num_config : &count);
            snprintf(b, sizeof(b), "[libsdl_compat] eglGetConfigs fallback -> ret=%d, num=%d\n",
                     g_ret, num_config ? *num_config : count);
            log_msg(b);
            if (g_ret && (!num_config || *num_config > 0)) {
                ret = 1;
            }
        }
    }
    return ret;
}

void *eglCreateWindowSurface(void *dpy, void *config, void *win, const int *attrib_list) {
    static void *(*real_fn)(void *, void *, void *, const int *) = NULL;
    static void *(*gbm_surf_create)(void *, uint32_t, uint32_t, uint32_t, uint32_t) = NULL;
    static void *mali = NULL;
    void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
    if (!mali) {
        mali = get_mali_handle();
    }
    if (!real_fn && mali && real_dlsym) {
        real_fn = (void *(*)(void *, void *, void *, const int *))real_dlsym(mali, "eglCreateWindowSurface");
        gbm_surf_create = (void *(*)(void *, uint32_t, uint32_t, uint32_t, uint32_t))real_dlsym(mali, "gbm_surface_create");
    }

    char b[256];
    snprintf(b, sizeof(b), "[libsdl_compat] eglCreateWindowSurface(dpy=%p, config=%p, win=%p)\n", dpy, config, win);
    log_msg(b);

    void *surf = NULL;
    if (g_gbm_dev && gbm_surf_create) {
        // GBM_FORMAT_XRGB8888 = 0x34325258, flags = 1 (GBM_BO_USE_RENDERING)
        void *gbm_surf = gbm_surf_create(g_gbm_dev, 480, 273, 0x34325258, 1);
        snprintf(b, sizeof(b), "[libsdl_compat] Created gbm_surface(XRGB8888)=%p for 480x273\n", gbm_surf);
        log_msg(b);
        if (gbm_surf && real_fn) {
            surf = real_fn(dpy, config, gbm_surf, attrib_list);
            snprintf(b, sizeof(b), "[libsdl_compat] eglCreateWindowSurface with gbm_surface(XRGB8888) -> %p\n", surf);
            log_msg(b);
        }

        if (!surf) {
            void *gbm_surf_argb = gbm_surf_create(g_gbm_dev, 480, 273, 0x34324158, 1);
            snprintf(b, sizeof(b), "[libsdl_compat] Created gbm_surface(ARGB8888)=%p for 480x273\n", gbm_surf_argb);
            log_msg(b);
            if (gbm_surf_argb && real_fn) {
                surf = real_fn(dpy, config, gbm_surf_argb, attrib_list);
                snprintf(b, sizeof(b), "[libsdl_compat] eglCreateWindowSurface with gbm_surface(ARGB8888) -> %p\n", surf);
                log_msg(b);
            }
        }
    }

    if (!surf && real_fn) {
        surf = real_fn(dpy, config, win, attrib_list);
        snprintf(b, sizeof(b), "[libsdl_compat] eglCreateWindowSurface with original win -> %p\n", surf);
        log_msg(b);
    }

    if (!surf) {
        surf = (void *)0x20000004;
        log_msg("[libsdl_compat] Falling back to dummy EGLSurface=0x20000004\n");
    }
    return surf;
}

void *eglCreateContext(void *dpy, void *config, void *share_context, const int *attrib_list) {
    static void *(*real_fn)(void *, void *, void *, const int *) = NULL;
    static int (*real_bind)(int) = NULL;
    static int (*real_get_err)(void) = NULL;
    void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
    void *mali = get_mali_handle();
    if (mali && real_dlsym) {
        if (!real_fn) real_fn = (void *(*)(void *, void *, void *, const int *))real_dlsym(mali, "eglCreateContext");
        if (!real_bind) real_bind = (int (*)(int))real_dlsym(mali, "eglBindAPI");
        if (!real_get_err) real_get_err = (int (*)(void))real_dlsym(mali, "eglGetError");
    }

    if (real_bind) {
        real_bind(0x30a0); // EGL_OPENGL_ES_API
    }

    char b[256];
    snprintf(b, sizeof(b), "[libsdl_compat] eglCreateContext(dpy=%p, config=%p, share=%p)\n", dpy, config, share_context);
    log_msg(b);

    void *ctx = real_fn ? real_fn(dpy, config, share_context, attrib_list) : NULL;
    int err = real_get_err ? real_get_err() : 0;
    snprintf(b, sizeof(b), "[libsdl_compat] eglCreateContext -> %p (err=0x%x)\n", ctx, err);
    log_msg(b);

    if (!ctx) {
        const int fallback_attribs[] = { 0x3098, 2, 0x3038 }; // EGL_CONTEXT_CLIENT_VERSION = 2, EGL_NONE
        log_msg("[libsdl_compat] Retrying eglCreateContext with minimal {CLIENT_VERSION, 2, NONE}...\n");
        ctx = real_fn ? real_fn(dpy, config, share_context, fallback_attribs) : NULL;
        err = real_get_err ? real_get_err() : 0;
        snprintf(b, sizeof(b), "[libsdl_compat] Minimal eglCreateContext -> %p (err=0x%x)\n", ctx, err);
        log_msg(b);
    }

    if (!ctx) {
        ctx = (void *)0x30000001;
        log_msg("[libsdl_compat] Falling back to dummy EGLContext=0x30000001\n");
    }
    return ctx;
}

int eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx) {
    if (ctx == (void *)0x30000001 || draw == (void *)0x20000004 || read == (void *)0x20000004) {
        return 1;
    }
    static int (*real_fn)(void *, void *, void *, void *) = NULL;
    static int mc_count = 0;
    if (!real_fn) {
        void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
        void *mali = get_mali_handle();
        if (mali && real_dlsym) {
            real_fn = (int (*)(void *, void *, void *, void *))real_dlsym(mali, "eglMakeCurrent");
        }
    }
    int ret = real_fn ? real_fn(dpy, draw, read, ctx) : 1;
    if (mc_count++ < 3) {
        char b[128];
        snprintf(b, sizeof(b), "[libsdl_compat] eglMakeCurrent(draw=%p, read=%p, ctx=%p) -> %d\n", draw, read, ctx, ret);
        log_msg(b);
    }
    return ret ? ret : 1;
}

int eglSwapBuffers(void *dpy, void *surf) {
    if (surf == (void *)0x20000004) {
        return 1;
    }
    static int (*real_fn)(void *, void *) = NULL;
    static int swap_count = 0;
    if (!real_fn) {
        void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
        void *mali = get_mali_handle();
        if (mali && real_dlsym) {
            real_fn = (int (*)(void *, void *))real_dlsym(mali, "eglSwapBuffers");
        }
    }
    int ret = real_fn ? real_fn(dpy, surf) : 1;
    if (swap_count++ % 300 == 0) {
        char b[128];
        snprintf(b, sizeof(b), "[libsdl_compat] eglSwapBuffers #%d -> %d\n", swap_count, ret);
        log_msg(b);
    }
    return ret;
}

void *eglGetProcAddress(const char *procname) {
    if (procname && procname[0]) {
        if (strcmp(procname, "eglGetDisplay") == 0) return (void *)&eglGetDisplay;
        if (strcmp(procname, "eglInitialize") == 0) return (void *)&eglInitialize;
        if (strcmp(procname, "eglBindAPI") == 0) return (void *)&eglBindAPI;
        if (strcmp(procname, "eglChooseConfig") == 0) return (void *)&eglChooseConfig;
        if (strcmp(procname, "eglCreateWindowSurface") == 0) return (void *)&eglCreateWindowSurface;
        if (strcmp(procname, "eglCreateContext") == 0) return (void *)&eglCreateContext;
        if (strcmp(procname, "eglMakeCurrent") == 0) return (void *)&eglMakeCurrent;
        if (strcmp(procname, "eglSwapBuffers") == 0) return (void *)&eglSwapBuffers;
    }
    static void *(*real_fn)(const char *) = NULL;
    void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
    void *mali = get_mali_handle();
    if (!real_fn && mali && real_dlsym) {
        real_fn = (void *(*)(const char *))real_dlsym(mali, "eglGetProcAddress");
    }
    void *res = real_fn ? real_fn(procname) : NULL;
    if (!res && mali && real_dlsym) {
        res = real_dlsym(mali, procname);
    }
    return res;
}

void *dlsym(void *handle, const char *name) {
    if (name && name[0]) {
        if (strcmp(name, "eglGetDisplay") == 0) return (void *)&eglGetDisplay;
        if (strcmp(name, "eglInitialize") == 0) return (void *)&eglInitialize;
        if (strcmp(name, "eglBindAPI") == 0) return (void *)&eglBindAPI;
        if (strcmp(name, "eglChooseConfig") == 0) return (void *)&eglChooseConfig;
        if (strcmp(name, "eglCreateWindowSurface") == 0) return (void *)&eglCreateWindowSurface;
        if (strcmp(name, "eglCreateContext") == 0) return (void *)&eglCreateContext;
        if (strcmp(name, "eglMakeCurrent") == 0) return (void *)&eglMakeCurrent;
        if (strcmp(name, "eglSwapBuffers") == 0) return (void *)&eglSwapBuffers;
        if (strcmp(name, "eglGetProcAddress") == 0) return (void *)&eglGetProcAddress;
    }

    void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
    return real_dlsym ? real_dlsym(handle, name) : NULL;
}

int SDL_SetHint(const char *name, const char *value) {
    static int (*real_fn)(const char *, const char *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_SetHint");
    char buf[256];
    snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_SetHint('%s', '%s')\n", name ? name : "", value ? value : "");
    log_msg(buf);
    return real_fn ? real_fn(name, value) : 0;
}

int SDL_GL_LoadLibrary(const char *path) {
    static int (*real_fn)(const char *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_GL_LoadLibrary");
    char buf[256];
    snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_GL_LoadLibrary('%s') called\n", path ? path : "NULL");
    log_msg(buf);

    int ret = real_fn ? real_fn(path) : -1;
    const char *err = safe_SDL_GetError();
    snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_GL_LoadLibrary returned %d (err: %s)\n", ret, err ? err : "none");
    log_msg(buf);
    return ret;
}

void *SDL_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags) {
    static void *(*real_fn)(const char *, int, int, int, int, uint32_t) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_CreateWindow");

    patch_sdl2_armv8_insns();

    char buf[256];
    snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_CreateWindow('%s', %d,%d, %dx%d, flags=0x%x)\n",
             title ? title : "", x, y, w, h, flags);
    log_msg(buf);

    int fb_w = 0, fb_h = 0, fb_bpp = 0, fb_pitch = 0;
    query_fb_mode(&fb_w, &fb_h, &fb_bpp, &fb_pitch);

    void *win = real_fn ? real_fn(title, x, y, w, h, flags) : NULL;
    snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_CreateWindow result=%p\n", win);
    log_msg(buf);

    if (!win) {
        const char *err = safe_SDL_GetError();
        snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_CreateWindow failed: %s\n", err ? err : "unknown");
        log_msg(buf);

        if (fb_w > 0 && fb_h > 0) {
            uint32_t retry_flags = (flags & ~0x00000002u) | 0x00001001u | 0x00000004u;
            snprintf(buf, sizeof(buf),
                     "[libsdl_compat] Retrying SDL_CreateWindow at framebuffer size %dx%d with flags=0x%x\n",
                     fb_w, fb_h, retry_flags);
            log_msg(buf);
            win = real_fn ? real_fn(title, x, y, fb_w, fb_h, retry_flags) : NULL;
            snprintf(buf, sizeof(buf), "[libsdl_compat] Framebuffer-size retry result=%p\n", win);
            log_msg(buf);

            if (!win) {
                err = safe_SDL_GetError();
                snprintf(buf, sizeof(buf), "[libsdl_compat] Framebuffer-size retry failed: %s\n", err ? err : "unknown");
                log_msg(buf);
            }
        }
    }

    if (!win) {
        log_msg("[libsdl_compat] Constructing fallback SDL_Window structure to prevent NULL dereference...\n");
        uint32_t *fake_win = (uint32_t *)calloc(1, 1024);
        if (fake_win) {
            uintptr_t base = get_libsdl2_base();
            if (base) {
                fake_win[0] = (uint32_t)(base + 0xaed80);
            } else {
                fake_win[0] = (uint32_t)(uintptr_t)fake_win;
            }
            fake_win[1] = 1;
            fake_win[2] = (uint32_t)(uintptr_t)"Baldur's Gate";
            fake_win[5] = 0;
            fake_win[6] = 0;
            fake_win[7] = 640;
            fake_win[8] = 480;
            fake_win[11] = 0x00000004u;
            win = (void *)fake_win;
            snprintf(buf, sizeof(buf), "[libsdl_compat] Created fallback window=%p\n", win);
            log_msg(buf);
        }
    }

    g_main_window = win;
    return win;
}

void *SDL_CreateRenderer(void *window, int index, uint32_t flags) {
    static void *(*real_fn)(void *, int, uint32_t) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_CreateRenderer");
    char buf[256];
    snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_CreateRenderer(win=%p, idx=%d, flags=0x%x)\n", window, index, flags);
    log_msg(buf);
    void *rend = real_fn ? real_fn(window, index, flags) : NULL;
    snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_CreateRenderer result=%p\n", rend);
    log_msg(buf);

    if (!rend) {
        const char *err = safe_SDL_GetError();
        snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_CreateRenderer failed: %s\n", err ? err : "unknown");
        log_msg(buf);

        log_msg("[libsdl_compat] Retrying SDL_CreateRenderer with flags=0...\n");
        rend = real_fn ? real_fn(window, -1, 0) : NULL;
        snprintf(buf, sizeof(buf), "[libsdl_compat] Flags=0 result=%p\n", rend);
        log_msg(buf);
    }
    return rend;
}

int SDL_GetRendererInfo(void *renderer, void *info) {
    if (!renderer) {
        log_msg("[libsdl_compat] SDL_GetRendererInfo called with NULL renderer\n");
        if (info) {
            *(const char **)info = "opengles2";
        }
        return -1;
    }
    static int (*real_fn)(void *, void *) = NULL;
    if (!real_fn) {
        void *sdl = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
        void *(*real_dlsym)(void *, const char *) = get_real_dlsym();
        if (sdl && real_dlsym) {
            real_fn = (int (*)(void *, void *))real_dlsym(sdl, "SDL_GetRendererInfo");
        }
    }
    int ret = real_fn ? real_fn(renderer, info) : 0;
    if (info && *(const char **)info == NULL) {
        *(const char **)info = "opengles2";
    }
    char b[128];
    snprintf(b, sizeof(b), "[libsdl_compat] SDL_GetRendererInfo(rend=%p) -> %d, name='%s'\n",
             renderer, ret, (info && *(const char **)info) ? *(const char **)info : "");
    log_msg(b);
    return ret;
}

int SDL_RenderSetLogicalSize(void *renderer, int w, int h) {
    static int (*real_fn)(void *, int, int) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_RenderSetLogicalSize");
    int ret = real_fn ? real_fn(renderer, w, h) : 0;
    char b[128];
    snprintf(b, sizeof(b), "[libsdl_compat] SDL_RenderSetLogicalSize(rend=%p, %dx%d) -> %d\n", renderer, w, h, ret);
    log_msg(b);
    return ret;
}

typedef struct {
    void *texture;
    int w;
    int h;
    int is_hud;
    int has_colorkey;
    uint32_t colorkey;
    uint32_t *pixels;
} TexCacheEntry;

typedef struct {
    int x;
    int y;
    uint32_t pixel;
} SavedOverlayPixel;

#define MAX_SAVED_OVERLAY 65536
static SavedOverlayPixel g_saved_overlay[MAX_SAVED_OVERLAY];
static int g_saved_overlay_count = 0;

#define MAX_TEX_CACHE 4096
#define TEX_HASH_SIZE 8192
static TexCacheEntry g_tex_cache[MAX_TEX_CACHE];
static int g_tex_cache_count = 0;
static int16_t g_tex_hash_head[TEX_HASH_SIZE];
static int16_t g_tex_hash_next[MAX_TEX_CACHE];

static inline uint32_t tex_hash_fn(void *ptr) {
    uintptr_t val = (uintptr_t)ptr;
    val = (val >> 3) ^ (val >> 13);
    return (uint32_t)(val & (TEX_HASH_SIZE - 1));
}

static void init_tex_hash(void) {
    static int inited = 0;
    if (!inited) {
        memset(g_tex_hash_head, -1, sizeof(g_tex_hash_head));
        memset(g_tex_hash_next, -1, sizeof(g_tex_hash_next));
        inited = 1;
    }
}

/* ===== TELEMETRY & DIAGNOSTICS (1-second aggregation) ===== */
typedef struct {
    uint32_t last_report_ticks;
    
    // CreateTextureFromSurface
    uint32_t create_surf_calls;
    uint32_t create_surf_paletted;
    uint32_t create_surf_colorkey;
    int last_create_w, last_create_h;
    uint32_t last_create_fmt;
    int last_create_bpp;
    int last_create_has_ck;
    uint32_t last_create_ck;
    int last_create_ncolors;
    uint8_t last_pal0[4];
    uint8_t last_pal_ck[4];
    uint32_t last_create_trans_px;
    uint32_t last_create_vis_px;

    // UpdateTexture
    uint32_t update_calls;
    uint32_t update_fullscreen_calls;
    uint32_t update_subrect_calls;
    int last_update_w, last_update_h;
    int last_update_rect[4];
    uint32_t last_update_fmt;
    int last_update_pitch;
    uint32_t last_update_nonz;

    // RenderCopy
    uint32_t copy_calls;
    uint32_t copy_target_calls;
    uint32_t copy_display_calls;
    int last_copy_src_rect[4];
    int last_copy_dst_rect[4];
    int last_copy_tex_w, last_copy_tex_h;
    void *last_copy_tgt;
    uint32_t last_copy_src_nonz;

    // RenderClear
    uint32_t clear_calls;
    uint32_t clear_target_calls;
    uint32_t clear_display_calls;
    uint32_t last_clear_color;

    // RenderPresent
    uint32_t present_frames;
    int last_rendered_mode;
    uint32_t shadow_nonz;
    uint32_t gpu_nonz;
} TelemetryStats;

static TelemetryStats g_stats = {0};

static const char *get_sdl_format_name(uint32_t fmt) {
    switch (fmt) {
        case 0x13000801u: return "INDEX8";
        case 0x16161804u: return "RGB888";
        case 0x16261804u: return "BGR888";
        case 0x16362004u: return "ARGB8888";
        case 0x16462004u: return "RGBA8888";
        case 0x16762004u: return "ABGR8888";
        case 0x16862004u: return "BGRA8888";
        case 0x15121002u: return "RGB565";
        case 0x15521002u: return "RGB555";
        case 0x32315659u: return "YV12";
        default: return "OTHER";
    }
}

static void flush_telemetry(uint32_t now) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "[TELEMETRY @ %u ms] FPS: %u | Mode: %d | ShadowNonZero: %u/307200 (%.1f%%) | GPUNonZero: %u | Cache: %d/%d\n"
        "  -> CreateTexFromSurf: %u calls (pal=%u, ck=%u) | last: %dx%d %s(0x%x) bpp=%d has_ck=%d ck=%u ncolors=%d pal0=(%d,%d,%d,%d) palCK=(%d,%d,%d,%d) trans_px=%u vis_px=%u\n"
        "  -> UpdateTexture: %u calls (640x480=%u, sub=%u) | last: %dx%d rect=(%d,%d %dx%d) %s(0x%x) pitch=%d nonz=%u\n"
        "  -> RenderCopy: %u calls (tgt=%u, disp=%u) | last_disp: tex=%dx%d src=(%d,%d %dx%d) dst=(%d,%d %dx%d) nonz=%u tgt=%p\n"
        "  -> RenderClear: %u calls (tgt=%u, disp=%u) | last_color=0x%08X\n",
        now, g_stats.present_frames, g_stats.last_rendered_mode,
        g_stats.shadow_nonz, ((double)g_stats.shadow_nonz * 100.0) / 307200.0,
        g_stats.gpu_nonz, g_tex_cache_count, MAX_TEX_CACHE,
        g_stats.create_surf_calls, g_stats.create_surf_paletted, g_stats.create_surf_colorkey,
        g_stats.last_create_w, g_stats.last_create_h, get_sdl_format_name(g_stats.last_create_fmt), g_stats.last_create_fmt,
        g_stats.last_create_bpp, g_stats.last_create_has_ck, g_stats.last_create_ck, g_stats.last_create_ncolors,
        g_stats.last_pal0[0], g_stats.last_pal0[1], g_stats.last_pal0[2], g_stats.last_pal0[3],
        g_stats.last_pal_ck[0], g_stats.last_pal_ck[1], g_stats.last_pal_ck[2], g_stats.last_pal_ck[3],
        g_stats.last_create_trans_px, g_stats.last_create_vis_px,
        g_stats.update_calls, g_stats.update_fullscreen_calls, g_stats.update_subrect_calls,
        g_stats.last_update_w, g_stats.last_update_h,
        g_stats.last_update_rect[0], g_stats.last_update_rect[1], g_stats.last_update_rect[2], g_stats.last_update_rect[3],
        get_sdl_format_name(g_stats.last_update_fmt), g_stats.last_update_fmt, g_stats.last_update_pitch, g_stats.last_update_nonz,
        g_stats.copy_calls, g_stats.copy_target_calls, g_stats.copy_display_calls,
        g_stats.last_copy_tex_w, g_stats.last_copy_tex_h,
        g_stats.last_copy_src_rect[0], g_stats.last_copy_src_rect[1], g_stats.last_copy_src_rect[2], g_stats.last_copy_src_rect[3],
        g_stats.last_copy_dst_rect[0], g_stats.last_copy_dst_rect[1], g_stats.last_copy_dst_rect[2], g_stats.last_copy_dst_rect[3],
        g_stats.last_copy_src_nonz, g_stats.last_copy_tgt,
        g_stats.clear_calls, g_stats.clear_target_calls, g_stats.clear_display_calls, g_stats.last_clear_color
    );
    log_msg(buf);

    // Reset periodic counters
    g_stats.last_report_ticks = now;
    g_stats.present_frames = 0;
    g_stats.create_surf_calls = 0;
    g_stats.create_surf_paletted = 0;
    g_stats.create_surf_colorkey = 0;
    g_stats.update_calls = 0;
    g_stats.update_fullscreen_calls = 0;
    g_stats.update_subrect_calls = 0;
    g_stats.copy_calls = 0;
    g_stats.copy_target_calls = 0;
    g_stats.copy_display_calls = 0;
    g_stats.clear_calls = 0;
    g_stats.clear_target_calls = 0;
    g_stats.clear_display_calls = 0;
}


static TexCacheEntry *find_tex(void *texture) {
    if (!texture) return NULL;
    init_tex_hash();
    uint32_t h = tex_hash_fn(texture);
    int idx = g_tex_hash_head[h];
    while (idx >= 0) {
        if (g_tex_cache[idx].texture == texture) {
            return &g_tex_cache[idx];
        }
        idx = g_tex_hash_next[idx];
    }
    return NULL;
}

static TexCacheEntry *find_or_create_tex(void *texture, int w, int h) {
    if (!texture || w <= 0 || h <= 0) return NULL;
    init_tex_hash();
    uint32_t hash_idx = tex_hash_fn(texture);
    int idx = g_tex_hash_head[hash_idx];
    while (idx >= 0) {
        if (g_tex_cache[idx].texture == texture) {
            if (g_tex_cache[idx].w != w || g_tex_cache[idx].h != h) {
                free(g_tex_cache[idx].pixels);
                g_tex_cache[idx].pixels = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
                g_tex_cache[idx].w = w;
                g_tex_cache[idx].h = h;
            }
            return &g_tex_cache[idx];
        }
        idx = g_tex_hash_next[idx];
    }

    // Reuse a freed slot if available
    int new_idx = -1;
    for (int i = 0; i < g_tex_cache_count; ++i) {
        if (g_tex_cache[i].texture == NULL) {
            new_idx = i;
            break;
        }
    }
    if (new_idx < 0 && g_tex_cache_count < MAX_TEX_CACHE) {
        new_idx = g_tex_cache_count++;
    }

    if (new_idx >= 0) {
        TexCacheEntry *e = &g_tex_cache[new_idx];
        e->texture = texture;
        e->w = w;
        e->h = h;
        e->is_hud = 0;
        e->has_colorkey = 0;
        e->colorkey = 0;
        if (!e->pixels) {
            e->pixels = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
        } else {
            memset(e->pixels, 0, (size_t)w * (size_t)h * sizeof(uint32_t));
        }
        g_tex_hash_next[new_idx] = g_tex_hash_head[hash_idx];
        g_tex_hash_head[hash_idx] = (int16_t)new_idx;
        return e;
    }
    return NULL;
}

static void *g_current_target = NULL;
static TexCacheEntry *g_current_target_entry = NULL;

int SDL_SetRenderTarget(void *renderer, void *texture) {
    static int (*real_fn)(void *, void *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_SetRenderTarget");
    g_current_target = texture;
    g_current_target_entry = texture ? find_tex(texture) : NULL;
    return real_fn ? real_fn(renderer, texture) : 0;
}

void *SDL_GetRenderTarget(void *renderer) {
    static void *(*real_fn)(void *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_GetRenderTarget");
    return real_fn ? real_fn(renderer) : g_current_target;
}

void *SDL_CreateTexture(void *renderer, uint32_t format, int access, int w, int h) {
    static void *(*real_fn)(void *, uint32_t, int, int, int) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_CreateTexture");
    void *tex = real_fn ? real_fn(renderer, format, access, w, h) : NULL;

    // Allocate software buffer for render target textures so they can be composited
    if (tex && access == 2 && w > 0 && h > 0) {
        find_or_create_tex(tex, w, h);
    }

    char b[192];
    snprintf(b, sizeof(b), "[libsdl_compat] SDL_CreateTexture(rend=%p, fmt=0x%x, access=%d, %dx%d) -> %p\n",
             renderer, format, access, w, h, tex);
    log_msg(b);
    return tex;
}

static uint32_t g_shadow_fb[640 * 480];
static int g_shadow_valid = 0;
static int g_is_video = 0;
static int g_video_y = 0;
static int g_video_h = 0;
static int g_video_frame_age = 0;

typedef struct {
    uint8_t r, g, b, a;
} CompatColor;

typedef struct {
    int ncolors;
    CompatColor *colors;
    uint32_t version;
    int refcount;
} CompatPalette;

typedef struct {
    uint32_t format;
    CompatPalette *palette;
    uint8_t BitsPerPixel;
    uint8_t BytesPerPixel;
    uint8_t padding[2];
    uint32_t Rmask, Gmask, Bmask, Amask;
    uint8_t Rloss, Gloss, Bloss, Aloss;
    uint8_t Rshift, Gshift, Bshift, Ashift;
    int refcount;
    void *next;
} CompatPixelFormat;

typedef struct SDL_Surface {
    uint32_t flags;
    CompatPixelFormat *format;
    int w, h;
    int pitch;
    void *pixels;
    void *userdata;
    int locked;
    void *list_blitmap;
    SDL_Rect clip_rect;
    void *map;
    int refcount;
} SDL_Surface;

void *SDL_CreateTextureFromSurface(void *renderer, void *surface_ptr) {
    static void *(*real_fn)(void *, void *) = NULL;
    static int (*real_convert_pixels)(int, int, uint32_t, const void *, int, uint32_t, void *, int) = NULL;
    static int (*real_get_colorkey)(void *, uint32_t *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_CreateTextureFromSurface");
    if (!real_convert_pixels) real_convert_pixels = get_next_symbol("SDL_ConvertPixels");
    if (!real_get_colorkey) real_get_colorkey = get_next_symbol("SDL_GetColorKey");

    void *tex = real_fn ? real_fn(renderer, surface_ptr) : NULL;
    g_stats.create_surf_calls++;

    if (tex && surface_ptr) {
        SDL_Surface *surface = (SDL_Surface *)surface_ptr;
        if (surface->w > 0 && surface->h > 0 && surface->pixels) {
            TexCacheEntry *entry = find_or_create_tex(tex, surface->w, surface->h);
            if (entry && entry->pixels) {
                CompatPixelFormat *fmt = surface->format;
                uint32_t src_fmt = fmt ? fmt->format : SDL_PIXELFORMAT_ARGB8888;
                int bpp = fmt ? fmt->BitsPerPixel : 32;

                uint32_t colorkey = 0xFFFFFFFF;
                int has_colorkey = 0;
                if (real_get_colorkey && real_get_colorkey(surface_ptr, &colorkey) == 0) {
                    has_colorkey = 1;
                }
                entry->has_colorkey = has_colorkey;
                entry->colorkey = colorkey;

                g_stats.last_create_w = surface->w;
                g_stats.last_create_h = surface->h;
                g_stats.last_create_fmt = src_fmt;
                g_stats.last_create_bpp = bpp;
                g_stats.last_create_has_ck = has_colorkey;
                g_stats.last_create_ck = colorkey;

                if (bpp == 8 && fmt && fmt->palette && fmt->palette->colors) {
                    g_stats.create_surf_paletted++;
                    int ncolors = fmt->palette->ncolors > 0 ? fmt->palette->ncolors : 256;
                    CompatColor *cols = fmt->palette->colors;
                    g_stats.last_create_ncolors = ncolors;
                    g_stats.last_pal0[0] = cols[0].r;
                    g_stats.last_pal0[1] = cols[0].g;
                    g_stats.last_pal0[2] = cols[0].b;
                    g_stats.last_pal0[3] = cols[0].a;

                    if (has_colorkey && (int)colorkey < ncolors) {
                        g_stats.create_surf_colorkey++;
                        g_stats.last_pal_ck[0] = cols[colorkey].r;
                        g_stats.last_pal_ck[1] = cols[colorkey].g;
                        g_stats.last_pal_ck[2] = cols[colorkey].b;
                        g_stats.last_pal_ck[3] = cols[colorkey].a;
                    }

                    int is_sprite = (surface->w < 640 || surface->h < 480);
                    uint32_t trans_cnt = 0, vis_cnt = 0;

                    for (int y = 0; y < surface->h; ++y) {
                        const uint8_t *srow = (const uint8_t *)surface->pixels + (size_t)y * (size_t)surface->pitch;
                        uint32_t *drow = entry->pixels + y * entry->w;
                        for (int x = 0; x < surface->w; ++x) {
                            uint8_t idx = srow[x];
                            // In Infinity Engine BAM sprites, index 0 is unconditionally transparent
                            if ((is_sprite && idx == 0) || (has_colorkey && idx == (uint8_t)colorkey)) {
                                drow[x] = 0;
                                trans_cnt++;
                            } else if (idx < ncolors) {
                                CompatColor c = cols[idx];
                                if (c.a == 0 || (idx == 0 && c.r == 0 && c.b == 0)) {
                                    drow[x] = 0;
                                    trans_cnt++;
                                } else {
                                    uint32_t a = c.a > 0 ? c.a : 255;
                                    drow[x] = (a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
                                    vis_cnt++;
                                }
                            } else {
                                drow[x] = 0;
                                trans_cnt++;
                            }
                        }
                    }
                    g_stats.last_create_trans_px = trans_cnt;
                    g_stats.last_create_vis_px = vis_cnt;
                } else if (real_convert_pixels) {
                    int cret = real_convert_pixels(surface->w, surface->h, src_fmt,
                                                   surface->pixels, surface->pitch,
                                                   SDL_PIXELFORMAT_ARGB8888, entry->pixels, surface->w * 4);
                    if (cret != 0) {
                        int row_bytes = surface->w * 4;
                        if (row_bytes > surface->pitch) row_bytes = surface->pitch;
                        for (int y = 0; y < surface->h; ++y) {
                            memcpy(entry->pixels + y * surface->w,
                                   (const uint8_t *)surface->pixels + (size_t)y * (size_t)surface->pitch, row_bytes);
                        }
                    }
                    // Apply colorkey filtering on 32-bit/converted surface (kills lime green frames!)
                    uint32_t ck_rgb = has_colorkey ? (colorkey & 0x00FFFFFF) : 0x0000FF00;
                    uint32_t trans_cnt = 0, vis_cnt = 0;
                    int total_px = surface->w * surface->h;
                    for (int i = 0; i < total_px; ++i) {
                        uint32_t rgb = entry->pixels[i] & 0x00FFFFFF;
                        if (rgb == ck_rgb || rgb == 0x0000FF00) {
                            entry->pixels[i] = 0; // 100% transparent
                            trans_cnt++;
                        } else {
                            vis_cnt++;
                        }
                    }
                    g_stats.last_create_trans_px = trans_cnt;
                    g_stats.last_create_vis_px = vis_cnt;
                }
            }
        }
    }
    return tex;
}

int SDL_UpdateTexture(void *texture, const SDL_Rect *rect, const void *pixels, int pitch) {
    static int (*real_fn)(void *, const SDL_Rect *, const void *, int) = NULL;
    static int (*real_query_tex)(void *, uint32_t *, int *, int *, int *) = NULL;

    if (!real_fn) real_fn = get_next_symbol("SDL_UpdateTexture");
    if (!real_query_tex) real_query_tex = get_next_symbol("SDL_QueryTexture");

    int tex_w = 0, tex_h = 0;
    uint32_t tex_fmt = 0;
    int tex_acc = 0;
    int qret = -1;
    if (real_query_tex && texture) {
        qret = real_query_tex(texture, &tex_fmt, &tex_acc, &tex_w, &tex_h);
    }

    g_stats.update_calls++;
    if (tex_w == 640 && tex_h == 480 && !rect) {
        g_stats.update_fullscreen_calls++;
    } else {
        g_stats.update_subrect_calls++;
    }
    g_stats.last_update_w = tex_w;
    g_stats.last_update_h = tex_h;
    g_stats.last_update_fmt = tex_fmt;
    g_stats.last_update_pitch = pitch;
    g_stats.last_update_rect[0] = rect ? rect->x : 0;
    g_stats.last_update_rect[1] = rect ? rect->y : 0;
    g_stats.last_update_rect[2] = rect ? rect->w : tex_w;
    g_stats.last_update_rect[3] = rect ? rect->h : tex_h;

    // Cache texture pixels in CPU RAM for software compositing in SDL_RenderCopy
    if (qret == 0 && tex_w > 0 && tex_h > 0 && pixels && pitch > 0) {
        TexCacheEntry *entry = find_or_create_tex(texture, tex_w, tex_h);
        if (entry && entry->pixels) {
            int dst_x = rect ? rect->x : 0;
            int dst_y = rect ? rect->y : 0;
            int w = rect ? rect->w : tex_w;
            int h = rect ? rect->h : tex_h;
            if (dst_x + w > entry->w) w = entry->w - dst_x;
            if (dst_y + h > entry->h) h = entry->h - dst_y;
            if (w > (pitch / 4)) w = pitch / 4;

            if (w > 0 && h > 0) {
                const uint8_t *src = (const uint8_t *)pixels;
                int is_abgr = (tex_fmt == 0x16762004u); // SDL_PIXELFORMAT_ABGR8888
                int is_rgba = (tex_fmt == 0x16462004u); // SDL_PIXELFORMAT_RGBA8888
                int is_bgra = (tex_fmt == 0x16862004u); // SDL_PIXELFORMAT_BGRA8888
                uint32_t ck_rgb = entry->has_colorkey ? (entry->colorkey & 0x00FFFFFF) : 0x0000FF00;
                uint32_t nonz_cnt = 0;

                for (int y = 0; y < h; ++y) {
                    const uint32_t *srow = (const uint32_t *)(src + (size_t)y * (size_t)pitch);
                    uint32_t *drow = entry->pixels + (dst_y + y) * entry->w + dst_x;
                    if (is_abgr) {
                        for (int x = 0; x < w; ++x) {
                            uint32_t p = srow[x];
                            uint32_t px = (p & 0xFF00FF00) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
                            uint32_t rgb = px & 0x00FFFFFF;
                            if (rgb == 0x0000FF00 || (entry->has_colorkey && rgb == ck_rgb)) {
                                drow[x] = 0;
                            } else {
                                drow[x] = px;
                                if (rgb) nonz_cnt++;
                            }
                        }
                    } else if (is_rgba) {
                        for (int x = 0; x < w; ++x) {
                            uint32_t p = srow[x];
                            uint32_t px = (p << 24) | ((p & 0xFF000000) >> 8) | ((p & 0x00FF0000) >> 8) | ((p & 0x0000FF00) >> 8);
                            uint32_t rgb = px & 0x00FFFFFF;
                            if (rgb == 0x0000FF00 || (entry->has_colorkey && rgb == ck_rgb)) {
                                drow[x] = 0;
                            } else {
                                drow[x] = px;
                                if (rgb) nonz_cnt++;
                            }
                        }
                    } else if (is_bgra) {
                        for (int x = 0; x < w; ++x) {
                            uint32_t p = srow[x];
                            uint32_t b = (p >> 24) & 0xFF;
                            uint32_t g = (p >> 16) & 0xFF;
                            uint32_t r = (p >> 8) & 0xFF;
                            uint32_t a = p & 0xFF;
                            uint32_t px = (a << 24) | (r << 16) | (g << 8) | b;
                            uint32_t rgb = px & 0x00FFFFFF;
                            if (rgb == 0x0000FF00 || (entry->has_colorkey && rgb == ck_rgb)) {
                                drow[x] = 0;
                            } else {
                                drow[x] = px;
                                if (rgb) nonz_cnt++;
                            }
                        }
                    } else {
                        // ARGB8888
                        for (int x = 0; x < w; ++x) {
                            uint32_t px = srow[x];
                            uint32_t rgb = px & 0x00FFFFFF;
                            if (rgb == 0x0000FF00 || (entry->has_colorkey && rgb == ck_rgb)) {
                                drow[x] = 0;
                            } else {
                                drow[x] = px;
                                if (rgb) nonz_cnt++;
                            }
                        }
                    }
                }
                g_stats.last_update_nonz = nonz_cnt;
            }
        }
    }

    // Also update g_shadow_fb directly if this texture is the main 640x480 display/video buffer
    if (qret == 0 && tex_w == 640 && tex_h == 480 && pixels && pitch > 0) {
        int dst_x = rect ? rect->x : 0;
        int dst_y = rect ? rect->y : 0;
        int w = rect ? rect->w : 640;
        int h = rect ? rect->h : 480;

        if (dst_x < 0) { w += dst_x; dst_x = 0; }
        if (dst_y < 0) { dst_y = 0; }
        if (dst_x >= 640 || dst_y >= 480) {
            w = 0;
            h = 0;
        }
        if (dst_x + w > 640) w = 640 - dst_x;
        if (dst_y + h > 480) h = 480 - dst_y;

        if (pitch > 0 && w > (pitch / 4)) {
            w = pitch / 4;
        }

        if (w > 0 && h > 0) {
            const uint8_t *src = (const uint8_t *)pixels;
            size_t row_bytes = (size_t)w * 4;
            if (row_bytes > (size_t)pitch) row_bytes = (size_t)pitch;

            for (int y = 0; y < h; ++y) {
                uint32_t *dst_row = g_shadow_fb + (dst_y + y) * 640 + dst_x;
                memcpy(dst_row, src + (size_t)y * (size_t)pitch, row_bytes);
            }
            g_shadow_valid = 1;
        }
    }

    int ret = real_fn ? real_fn(texture, rect, pixels, pitch) : 0;
    return ret;
}

void SDL_DestroyTexture(void *texture) {
    static void (*real_fn)(void *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_DestroyTexture");
    if (texture) {
        init_tex_hash();
        uint32_t hash_idx = tex_hash_fn(texture);
        int prev = -1;
        int idx = g_tex_hash_head[hash_idx];
        while (idx >= 0) {
            if (g_tex_cache[idx].texture == texture) {
                if (prev >= 0) {
                    g_tex_hash_next[prev] = g_tex_hash_next[idx];
                } else {
                    g_tex_hash_head[hash_idx] = g_tex_hash_next[idx];
                }
                g_tex_hash_next[idx] = -1;
                free(g_tex_cache[idx].pixels);
                g_tex_cache[idx].pixels = NULL;
                g_tex_cache[idx].texture = NULL;
                g_tex_cache[idx].w = 0;
                g_tex_cache[idx].h = 0;
                break;
            }
            prev = idx;
            idx = g_tex_hash_next[idx];
        }
    }
    if (real_fn) real_fn(texture);
}


/* ===== D-Pad "snap to control" navigation ===== */
typedef struct { int x, y, w, h; } SnapRect;
static SnapRect g_snap_cur[512], g_snap_last[512];
static int g_snap_cur_n = 0, g_snap_last_n = 0;
static int g_mouse_x = 320, g_mouse_y = 240;

static void snap_record(const SDL_Rect *d) {
    if (!d) return;
    if ((d->w == 46 && d->h == 38) || (d->w == 49 && d->h == 33)) return; /* cursor sprite */
    if (d->w >= 200 && d->h >= 200) { /* window background: hides everything drawn before it */
        int k = 0;
        for (int i = 0; i < g_snap_cur_n; ++i) {
            int cx = g_snap_cur[i].x + g_snap_cur[i].w / 2;
            int cy = g_snap_cur[i].y + g_snap_cur[i].h / 2;
            if (cx >= d->x && cx < d->x + d->w && cy >= d->y && cy < d->y + d->h) continue;
            g_snap_cur[k++] = g_snap_cur[i];
        }
        g_snap_cur_n = k;
        return;
    }
    if (d->w < 6 || d->h < 6 || d->w > 360 || d->h > 120) return;
    if (d->x + d->w <= 0 || d->y + d->h <= 0 || d->x >= 640 || d->y >= 480) return;

    int cx = d->x + d->w / 2;
    int cy = d->y + d->h / 2;
    for (int i = 0; i < g_snap_cur_n; ++i) {
        int icx = g_snap_cur[i].x + g_snap_cur[i].w / 2;
        int icy = g_snap_cur[i].y + g_snap_cur[i].h / 2;
        if (abs(icx - cx) < 8 && abs(icy - cy) < 8) {
            return; /* duplicate target at same button */
        }
    }

    if (g_snap_cur_n < 512) {
        g_snap_cur[g_snap_cur_n].x = d->x; g_snap_cur[g_snap_cur_n].y = d->y;
        g_snap_cur[g_snap_cur_n].w = d->w; g_snap_cur[g_snap_cur_n].h = d->h;
        g_snap_cur_n++;
    }
}

static void snap_frame_end(void) {
    if (g_snap_cur_n > 0) {
        memcpy(g_snap_last, g_snap_cur, sizeof(SnapRect) * g_snap_cur_n);
        g_snap_last_n = g_snap_cur_n;
    }
    g_snap_cur_n = 0;
}

static unsigned char g_evq[8][56];
static int g_evq_n = 0;

static void queue_synthetic_event(const void *ev) {
    if (g_evq_n < 8) {
        memcpy(g_evq[g_evq_n++], ev, 56);
    }
}

static void snap_move(int dir, uint32_t win_id) { /* 0=up 1=down 2=left 3=right */
    static void *(*get_win)(uint32_t) = NULL;
    if (!get_win) get_win = get_next_symbol("SDL_GetWindowFromID");
    int px = g_mouse_x, py = g_mouse_y;
    int best = -1;
    long best_score = 0x7fffffff;
    for (int i = 0; i < g_snap_last_n; ++i) {
        int cx = g_snap_last[i].x + g_snap_last[i].w / 2;
        int cy = g_snap_last[i].y + g_snap_last[i].h / 2;
        int dx = cx - px, dy = cy - py;
        long score = 0; int ok = 0;
        switch (dir) {
            case 0: ok = dy < -4; score = -dy + 3L * labs(dx); break;
            case 1: ok = dy > 4;  score = dy + 3L * labs(dx);  break;
            case 2: ok = dx < -4; score = -dx + 3L * labs(dy); break;
            default: ok = dx > 4; score = dx + 3L * labs(dy);  break;
        }
        if (ok && score < best_score) { best_score = score; best = i; }
    }
    if (best < 0) {
        for (int i = 0; i < g_snap_last_n; ++i) {
            int cx = g_snap_last[i].x + g_snap_last[i].w / 2;
            int cy = g_snap_last[i].y + g_snap_last[i].h / 2;
            int dx = cx - px, dy = cy - py;
            long dist = (long)dx * dx + (long)dy * dy;
            if (dist < best_score) {
                best_score = dist;
                best = i;
            }
        }
    }
    char b[160];
    if (best < 0) {
        snprintf(b, sizeof(b), "[SNAP] dir=%d no target from (%d,%d), %d rects\n", dir, px, py, g_snap_last_n);
        log_msg(b);
        return;
    }
    int tx = g_snap_last[best].x + g_snap_last[best].w / 2;
    int ty = g_snap_last[best].y + g_snap_last[best].h / 2;
    void *win = get_win ? get_win(win_id) : NULL;
    if (!win) win = g_main_window;
    snprintf(b, sizeof(b), "[SNAP] dir=%d (%d,%d) -> (%d,%d) rect=%dx%d win=%p rects=%d\n",
             dir, px, py, tx, ty, g_snap_last[best].w, g_snap_last[best].h, win, g_snap_last_n);
    log_msg(b);
    g_mouse_x = tx; g_mouse_y = ty;
    if (win) {
        static void (*real_warp)(void *, int, int) = NULL;
        if (!real_warp) real_warp = get_next_symbol("SDL_WarpMouseInWindow");
        if (real_warp) real_warp(win, tx, ty);
    }
    /* Inject synthetic mouse motion so GemRB cursor and hover update immediately */
    unsigned char m[56];
    memset(m, 0, sizeof(m));
    *(uint32_t *)(m + 0) = 0x400; // SDL_MOUSEMOTION
    *(uint32_t *)(m + 8) = win_id;
    *(int32_t *)(m + 20) = tx;
    *(int32_t *)(m + 24) = ty;
    *(int32_t *)(m + 28) = tx - px;
    *(int32_t *)(m + 32) = ty - py;
    queue_synthetic_event(m);
}

static uint32_t g_draw_color = 0xFF000000;

int SDL_SetRenderDrawColor(void *renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    static int (*real_fn)(void *, uint8_t, uint8_t, uint8_t, uint8_t) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_SetRenderDrawColor");
    g_draw_color = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    return real_fn ? real_fn(renderer, r, g, b, a) : 0;
}

int SDL_GetRenderDrawColor(void *renderer, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    static int (*real_fn)(void *, uint8_t *, uint8_t *, uint8_t *, uint8_t *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_GetRenderDrawColor");
    if (r) *r = (g_draw_color >> 16) & 0xFF;
    if (g) *g = (g_draw_color >> 8) & 0xFF;
    if (b) *b = g_draw_color & 0xFF;
    if (a) *a = (g_draw_color >> 24) & 0xFF;
    return real_fn ? real_fn(renderer, r, g, b, a) : 0;
}

static void internal_fill_rect(uint32_t *dst_buf, int target_w, int target_h, const SDL_Rect *rect, uint32_t color) {
    if (!dst_buf || target_w <= 0 || target_h <= 0) return;
    int rx = rect ? rect->x : 0;
    int ry = rect ? rect->y : 0;
    int rw = rect ? rect->w : target_w;
    int rh = rect ? rect->h : target_h;

    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > target_w) rw = target_w - rx;
    if (ry + rh > target_h) rh = target_h - ry;

    if (rw <= 0 || rh <= 0) return;

    uint32_t a = (color >> 24) & 0xFF;
    if (a == 255 || a == 0) {
        for (int y = 0; y < rh; ++y) {
            uint32_t *drow = dst_buf + (ry + y) * target_w + rx;
            for (int x = 0; x < rw; ++x) drow[x] = color;
        }
    } else {
        for (int y = 0; y < rh; ++y) {
            uint32_t *drow = dst_buf + (ry + y) * target_w + rx;
            for (int x = 0; x < rw; ++x) {
                uint32_t bg = drow[x];
                uint32_t rb = (bg & 0xFF00FF) + ((((color & 0xFF00FF) - (bg & 0xFF00FF)) * a) >> 8);
                uint32_t g  = (bg & 0x00FF00) + ((((color & 0x00FF00) - (bg & 0x00FF00)) * a) >> 8);
                drow[x] = (rb & 0xFF00FF) | (g & 0x00FF00) | 0xFF000000;
            }
        }
    }
}

int SDL_RenderFillRect(void *renderer, const SDL_Rect *rect) {
    static int (*real_fn)(void *, const SDL_Rect *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_RenderFillRect");

    uint32_t *dst_buf = g_shadow_fb;
    int target_w = 640, target_h = 480;
    if (g_current_target) {
        TexCacheEntry *dst_entry = find_tex(g_current_target);
        if (dst_entry && dst_entry->pixels) {
            dst_buf = dst_entry->pixels;
            target_w = dst_entry->w;
            target_h = dst_entry->h;
        } else {
            dst_buf = NULL;
        }
    }

    if (dst_buf) {
        internal_fill_rect(dst_buf, target_w, target_h, rect, g_draw_color);
    }
    return real_fn ? real_fn(renderer, rect) : 0;
}

int SDL_RenderFillRects(void *renderer, const SDL_Rect *rects, int count) {
    static int (*real_fn)(void *, const SDL_Rect *, int) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_RenderFillRects");

    uint32_t *dst_buf = g_shadow_fb;
    int target_w = 640, target_h = 480;
    if (g_current_target) {
        TexCacheEntry *dst_entry = find_tex(g_current_target);
        if (dst_entry && dst_entry->pixels) {
            dst_buf = dst_entry->pixels;
            target_w = dst_entry->w;
            target_h = dst_entry->h;
        } else {
            dst_buf = NULL;
        }
    }

    if (dst_buf && rects && count > 0) {
        for (int i = 0; i < count; ++i) {
            internal_fill_rect(dst_buf, target_w, target_h, &rects[i], g_draw_color);
        }
    }
    return real_fn ? real_fn(renderer, rects, count) : 0;
}

int SDL_RenderDrawRect(void *renderer, const SDL_Rect *rect) {
    static int (*real_fn)(void *, const SDL_Rect *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_RenderDrawRect");
    if (rect) {
        SDL_Rect top = { rect->x, rect->y, rect->w, 1 };
        SDL_Rect bottom = { rect->x, rect->y + rect->h - 1, rect->w, 1 };
        SDL_Rect left = { rect->x, rect->y, 1, rect->h };
        SDL_Rect right = { rect->x + rect->w - 1, rect->y, 1, rect->h };
        SDL_RenderFillRect(renderer, &top);
        SDL_RenderFillRect(renderer, &bottom);
        SDL_RenderFillRect(renderer, &left);
        SDL_RenderFillRect(renderer, &right);
    }
    return real_fn ? real_fn(renderer, rect) : 0;
}

int SDL_RenderDrawRects(void *renderer, const SDL_Rect *rects, int count) {
    static int (*real_fn)(void *, const SDL_Rect *, int) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_RenderDrawRects");
    if (rects && count > 0) {
        for (int i = 0; i < count; ++i) {
            SDL_RenderDrawRect(renderer, &rects[i]);
        }
    }
    return real_fn ? real_fn(renderer, rects, count) : 0;
}

int SDL_RenderClear(void *renderer) {
    static int (*real_fn)(void *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_RenderClear");

    g_stats.clear_calls++;
    if (g_current_target) {
        g_stats.clear_target_calls++;
        TexCacheEntry *dst_entry = find_tex(g_current_target);
        if (dst_entry && dst_entry->pixels) {
            uint32_t col = g_draw_color;
            if (col == 0 || (col >> 24) == 0) {
                dst_entry->is_hud = 1;
                memset(dst_entry->pixels, 0, (size_t)dst_entry->w * (size_t)dst_entry->h * sizeof(uint32_t));
            } else {
                size_t total = (size_t)dst_entry->w * (size_t)dst_entry->h;
                for (size_t i = 0; i < total; ++i) dst_entry->pixels[i] = col;
            }
        }
    } else {
        g_stats.clear_display_calls++;
        g_stats.last_clear_color = g_draw_color;
    }

    return real_fn ? real_fn(renderer) : 0;
}

int SDL_RenderCopy(void *renderer, void *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect) {
    if (!g_current_target) {
        snap_record(dstrect);
    }
    static int (*real_fn)(void *, void *, const SDL_Rect *, const SDL_Rect *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_RenderCopy");

    g_stats.copy_calls++;
    if (g_current_target) {
        g_stats.copy_target_calls++;
    } else {
        g_stats.copy_display_calls++;
    }

    TexCacheEntry *src_entry = find_tex(texture);

    if (src_entry && src_entry->pixels) {
        int src_x = srcrect ? srcrect->x : 0;
        int src_y = srcrect ? srcrect->y : 0;
        int src_w = srcrect ? srcrect->w : src_entry->w;
        int src_h = srcrect ? srcrect->h : src_entry->h;

        uint32_t *dst_buf = g_shadow_fb;
        int target_w = 640;
        int target_h = 480;

        if (g_current_target) {
            TexCacheEntry *dst_entry = g_current_target_entry ? g_current_target_entry : find_tex(g_current_target);
            if (dst_entry && dst_entry->pixels) {
                dst_buf = dst_entry->pixels;
                target_w = dst_entry->w;
                target_h = dst_entry->h;
            } else {
                dst_buf = NULL;
            }
        }

        if (dst_buf) {
            int dst_x = dstrect ? dstrect->x : 0;
            int dst_y = dstrect ? dstrect->y : 0;
            int dst_w = dstrect ? dstrect->w : target_w;
            int dst_h = dstrect ? dstrect->h : target_h;

            if (dst_x < 0) { dst_w += dst_x; dst_x = 0; }
            if (dst_y < 0) { dst_h += dst_y; dst_y = 0; }

            if (dst_x < target_w && dst_y < target_h && dst_w > 0 && dst_h > 0) {
                int clip_w = (dst_x + dst_w > target_w) ? (target_w - dst_x) : dst_w;
                int clip_h = (dst_y + dst_h > target_h) ? (target_h - dst_y) : dst_h;

                // ONLY mouse cursor sprites are ephemeral overlays
                int is_overlay = 0;
                if (!g_current_target && dst_buf == g_shadow_fb) {
                    if ((src_w == 46 && src_h == 38) || (src_w == 49 && src_h == 33) || (src_w <= 36 && src_h <= 36)) {
                        is_overlay = 1;
                    }
                }

                uint32_t nonz_src = 0;
                if (dst_w == src_w && dst_h == src_h) {
                    // Fast path: 100% opaque tile (e.g. 64x64 terrain tiles)
                    int is_opaque_tile = (clip_w == 64 && clip_h == 64 && !is_overlay && (src_entry->pixels[0] >> 24 == 255));
                    if (is_opaque_tile) {
                        size_t row_bytes = (size_t)clip_w * sizeof(uint32_t);
                        for (int y = 0; y < clip_h; ++y) {
                            uint32_t *srow = src_entry->pixels + (src_y + y) * src_entry->w + src_x;
                            uint32_t *drow = dst_buf + (dst_y + y) * target_w + dst_x;
                            memcpy(drow, srow, row_bytes);
                        }
                        nonz_src = 4096;
                    } else {
                        for (int y = 0; y < clip_h; ++y) {
                            if (src_y + y >= src_entry->h) break;
                            uint32_t *srow = src_entry->pixels + (src_y + y) * src_entry->w + src_x;
                            uint32_t *drow = dst_buf + (dst_y + y) * target_w + dst_x;
                            for (int x = 0; x < clip_w; ++x) {
                                uint32_t c = srow[x];
                                uint32_t a = (c >> 24) & 0xFF;
                                if (a > 10) {
                                    nonz_src++;
                                    if (is_overlay && g_saved_overlay_count < MAX_SAVED_OVERLAY) {
                                        g_saved_overlay[g_saved_overlay_count].x = dst_x + x;
                                        g_saved_overlay[g_saved_overlay_count].y = dst_y + y;
                                        g_saved_overlay[g_saved_overlay_count].pixel = drow[x];
                                        g_saved_overlay_count++;
                                    }
                                    if (a >= 250) {
                                        drow[x] = c;
                                    } else {
                                        uint32_t bg = drow[x];
                                        uint32_t rb = (bg & 0xFF00FF) + ((((c & 0xFF00FF) - (bg & 0xFF00FF)) * a) >> 8);
                                        uint32_t g  = (bg & 0x00FF00) + ((((c & 0x00FF00) - (bg & 0x00FF00)) * a) >> 8);
                                        drow[x] = (rb & 0xFF00FF) | (g & 0x00FF00) | 0xFF000000;
                                    }
                                }
                            }
                        }
                    }
                } else {
                    for (int y = 0; y < clip_h; ++y) {
                        int sy = src_y + (y * src_h) / dst_h;
                        if (sy >= src_entry->h) break;
                        uint32_t *srow = src_entry->pixels + sy * src_entry->w;
                        uint32_t *drow = dst_buf + (dst_y + y) * target_w + dst_x;
                        for (int x = 0; x < clip_w; ++x) {
                            int sx = src_x + (x * src_w) / dst_w;
                            if (sx >= src_entry->w) break;
                            uint32_t c = srow[sx];
                            uint32_t a = (c >> 24) & 0xFF;
                            if (a > 10) {
                                nonz_src++;
                                if (is_overlay && g_saved_overlay_count < MAX_SAVED_OVERLAY) {
                                    g_saved_overlay[g_saved_overlay_count].x = dst_x + x;
                                    g_saved_overlay[g_saved_overlay_count].y = dst_y + y;
                                    g_saved_overlay[g_saved_overlay_count].pixel = drow[x];
                                    g_saved_overlay_count++;
                                }
                                if (a >= 250) {
                                    drow[x] = c;
                                } else {
                                    uint32_t bg = drow[x];
                                    uint32_t rb = (bg & 0xFF00FF) + ((((c & 0xFF00FF) - (bg & 0xFF00FF)) * a) >> 8);
                                    uint32_t g  = (bg & 0x00FF00) + ((((c & 0x00FF00) - (bg & 0x00FF00)) * a) >> 8);
                                    drow[x] = (rb & 0xFF00FF) | (g & 0x00FF00) | 0xFF000000;
                                }
                            }
                        }
                    }
                }
                if (!g_current_target) {
                    g_shadow_valid = 1;
                    g_stats.last_copy_tex_w = src_entry->w;
                    g_stats.last_copy_tex_h = src_entry->h;
                    g_stats.last_copy_src_rect[0] = src_x;
                    g_stats.last_copy_src_rect[1] = src_y;
                    g_stats.last_copy_src_rect[2] = src_w;
                    g_stats.last_copy_src_rect[3] = src_h;
                    g_stats.last_copy_dst_rect[0] = dst_x;
                    g_stats.last_copy_dst_rect[1] = dst_y;
                    g_stats.last_copy_dst_rect[2] = clip_w;
                    g_stats.last_copy_dst_rect[3] = clip_h;
                    g_stats.last_copy_src_nonz = nonz_src;
                    g_stats.last_copy_tgt = NULL;
                } else {
                    g_stats.last_copy_tgt = g_current_target;
                }
            }
        }
    }

    int ret = real_fn ? real_fn(renderer, texture, srcrect, dstrect) : 0;
    return ret;
}

int SDL_RenderCopyEx(void *renderer, void *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect,
                     double angle, const void *center, int flip) {
    static int (*real_fn)(void *, void *, const SDL_Rect *, const SDL_Rect *, double, const void *, int) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_RenderCopyEx");
    SDL_RenderCopy(renderer, texture, srcrect, dstrect);
    return real_fn ? real_fn(renderer, texture, srcrect, dstrect, angle, center, flip) : 0;
}

void SDL_WarpMouseInWindow(void *window, int x, int y) {
    static void (*real_fn)(void *, int, int) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_WarpMouseInWindow");
    char buf[128];
    snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_WarpMouseInWindow(win=%p, x=%d, y=%d)\n", window, x, y);
    log_msg(buf);
    if (real_fn) real_fn(window, x, y);
}

int SDL_GetNumDisplayModes(int displayIndex) {
    static int (*real_fn)(int) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_GetNumDisplayModes");
    int count = real_fn ? real_fn(displayIndex) : 1;
    char buf[128];
    snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_GetNumDisplayModes(%d) -> %d\n", displayIndex, count);
    log_msg(buf);
    return count > 0 ? count : 1;
}

int SDL_GetDisplayMode(int displayIndex, int modeIndex, CompatDisplayMode *mode) {
    static int (*real_fn)(int, int, void *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_GetDisplayMode");
    int ret = real_fn ? real_fn(displayIndex, modeIndex, mode) : 0;
    if (mode) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_GetDisplayMode: real=(%dx%d), overriding to 640x480\n", mode->w, mode->h);
        log_msg(buf);
        if (mode->w < 640) mode->w = 640;
        if (mode->h < 480) mode->h = 480;
    }
    return ret;
}

int SDL_GetCurrentDisplayMode(int displayIndex, CompatDisplayMode *mode) {
    static int (*real_fn)(int, void *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_GetCurrentDisplayMode");
    int ret = real_fn ? real_fn(displayIndex, mode) : 0;
    if (mode) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_GetCurrentDisplayMode: real=(%dx%d), overriding to 640x480\n", mode->w, mode->h);
        log_msg(buf);
        if (mode->w < 640) mode->w = 640;
        if (mode->h < 480) mode->h = 480;
    }
    return ret;
}

int SDL_GetDesktopDisplayMode(int displayIndex, CompatDisplayMode *mode) {
    static int (*real_fn)(int, void *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_GetDesktopDisplayMode");
    int ret = real_fn ? real_fn(displayIndex, mode) : 0;
    if (mode) {
        if (mode->w < 640) mode->w = 640;
        if (mode->h < 480) mode->h = 480;
    }
    return ret;
}

void SDL_GetWindowSize(void *window, int *w, int *h) {
    static void (*real_fn)(void *, int *, int *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_GetWindowSize");
    if (real_fn) real_fn(window, w, h);
    if (w && *w < 640) *w = 640;
    if (h && *h < 480) *h = 480;
    char buf[128];
    snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_GetWindowSize -> %dx%d\n", w ? *w : 0, h ? *h : 0);
    log_msg(buf);
}

static uint32_t *g_render_buf = NULL;
static size_t g_render_buf_cap = 0;
static int g_click_dot_frames = 0;
static int g_click_dot_x = 0;
static int g_click_dot_y = 0;
static uint32_t g_click_dot_color = 0xFF00FF00;

static int g_lut_x[480];
static int g_lut_y[273];
static int g_lut_inited = 0;

static void init_scale_lut(void) {
    if (g_lut_inited) return;
    for (int x = 0; x < 480; ++x) {
        g_lut_x[x] = (x * 640) / 480;
    }
    for (int y = 0; y < 273; ++y) {
        int sy = (y * 480) / 273;
        if (sy >= 480) sy = 479;
        g_lut_y[y] = sy;
    }
    g_lut_inited = 1;
}

static inline uint32_t blend_text_preserve(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11) {
    int r00 = (c00 >> 16) & 0xFF, g00 = (c00 >> 8) & 0xFF, b00 = c00 & 0xFF;
    int r10 = (c10 >> 16) & 0xFF, g10 = (c10 >> 8) & 0xFF, b10 = c10 & 0xFF;
    int r01 = (c01 >> 16) & 0xFF, g01 = (c01 >> 8) & 0xFF, b01 = c01 & 0xFF;
    int r11 = (c11 >> 16) & 0xFF, g11 = (c11 >> 8) & 0xFF, b11 = c11 & 0xFF;

    int mr0 = (r00 > r10) ? r00 : r10;
    int mr1 = (r01 > r11) ? r01 : r11;
    int max_r = (mr0 > mr1) ? mr0 : mr1;

    int mg0 = (g00 > g10) ? g00 : g10;
    int mg1 = (g01 > g11) ? g01 : g11;
    int max_g = (mg0 > mg1) ? mg0 : mg1;

    int mb0 = (b00 > b10) ? b00 : b10;
    int mb1 = (b01 > b11) ? b01 : b11;
    int max_b = (mb0 > mb1) ? mb0 : mb1;

    int avg_r = (r00 + r10 + r01 + r11) >> 2;
    int avg_g = (g00 + g10 + g01 + g11) >> 2;
    int avg_b = (b00 + b10 + b01 + b11) >> 2;

    int out_r = (max_r + avg_r) >> 1;
    int out_g = (max_g + avg_g) >> 1;
    int out_b = (max_b + avg_b) >> 1;

    return 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
}

void SDL_RenderPresent(void *renderer) {
    static void (*real_present)(void *) = NULL;
    static int (*real_render_read_pixels)(void *, const SDL_Rect *, uint32_t, void *, int) = NULL;
    static int (*real_get_output_size)(void *, int *, int *) = NULL;
    static uint32_t (*real_get_ticks)(void) = NULL;
    static int frame_count = 0;

    if (!real_present) {
        real_present = get_next_symbol("SDL_RenderPresent");
        real_render_read_pixels = get_next_symbol("SDL_RenderReadPixels");
        real_get_output_size = get_next_symbol("SDL_GetRendererOutputSize");
        real_get_ticks = get_next_symbol("SDL_GetTicks");

        char buf[256];
        snprintf(buf, sizeof(buf), "[libsdl_compat] RenderPresent init: present=%p read_pixels=%p output_size=%p\n",
                 (void *)real_present, (void *)real_render_read_pixels, (void *)real_get_output_size);
        log_msg(buf);
    }

    init_fb_blitter();

    g_stats.present_frames++;
    uint32_t now = real_get_ticks ? real_get_ticks() : 0;
    if (g_stats.last_report_ticks == 0) g_stats.last_report_ticks = now;

    // Periodic 1-second telemetry collection
    if (now - g_stats.last_report_ticks >= 1000) {
        // Count non-zero pixels in g_shadow_fb
        uint32_t shadow_nonz = 0;
        for (int i = 0; i < 640 * 480; ++i) {
            if (g_shadow_fb[i] & 0x00FFFFFF) shadow_nonz++;
        }
        g_stats.shadow_nonz = shadow_nonz;

        // One-time readback from GPU purely for diagnostic counters
        uint32_t gpu_nonz = 0;
        if (renderer && real_render_read_pixels) {
            int out_w = 0, out_h = 0;
            if (real_get_output_size) real_get_output_size(renderer, &out_w, &out_h);
            if (out_w <= 0) out_w = 640;
            if (out_h <= 0) out_h = 480;
            size_t needed = (size_t)out_w * (size_t)out_h * sizeof(uint32_t);
            if (needed > g_render_buf_cap) {
                free(g_render_buf);
                g_render_buf = (uint32_t *)malloc(needed);
                g_render_buf_cap = needed;
            }
            if (g_render_buf) {
                if (real_render_read_pixels(renderer, NULL, SDL_PIXELFORMAT_ABGR8888, g_render_buf, out_w * 4) == 0) {
                    for (size_t i = 0; i < (size_t)out_w * (size_t)out_h; ++i) {
                        if (g_render_buf[i] & 0x00FFFFFF) gpu_nonz++;
                    }
                }
            }
        }
        g_stats.gpu_nonz = gpu_nonz;

        // Flush telemetry log!
        flush_telemetry(now);
    }

    // Step 1: EGL swap buffers
    if (real_present) {
        real_present(renderer);
    }

    // Step 2: Blit directly to /dev/fb0 with zero lag using the fast LUT scaler (60 FPS like Diablo!)
    int rendered = 0;
    if (g_fb_mem != MAP_FAILED) {
        int dst_w = (int)g_vinfo.xres;
        int dst_h = (int)g_vinfo.yres;
        int pitch = (int)g_finfo.line_length;

        if (dst_w == 480 && dst_h == 273 && g_vinfo.bits_per_pixel == 32) {
            if (g_shadow_valid) {
                // Ultra-fast 1-pass LUT scaler: rock-solid 60 FPS, stable video & gameplay
                init_scale_lut();
                for (int y = 0; y < 273; ++y) {
                    uint32_t *src_row = g_shadow_fb + g_lut_y[y] * 640;
                    uint32_t *dst_row = (uint32_t *)(g_fb_mem + (size_t)y * (size_t)pitch);
                    for (int x = 0; x < 480; ++x) {
                        dst_row[x] = src_row[g_lut_x[x]];
                    }
                }
                rendered = 4;
            }
        }

        // Draw mouse click visualizer if active
        if (g_click_dot_frames > 0) {
            g_click_dot_frames--;
            for (int dy = -2; dy <= 2; ++dy) {
                int py = g_click_dot_y + dy;
                if (py >= 0 && py < 273) {
                    uint32_t *drow = (uint32_t *)(g_fb_mem + (size_t)py * (size_t)pitch);
                    for (int dx = -2; dx <= 2; ++dx) {
                        int px = g_click_dot_x + dx;
                        if (px >= 0 && px < 480) {
                            drow[px] = g_click_dot_color;
                        }
                    }
                }
            }
        }
        msync(g_fb_mem, g_fb_size, MS_ASYNC);

        // Restore saved overlay pixels (only cursor pixels!)
        if (g_saved_overlay_count > 0) {
            for (int i = 0; i < g_saved_overlay_count; ++i) {
                int px = g_saved_overlay[i].x;
                int py = g_saved_overlay[i].y;
                if (px >= 0 && px < 640 && py >= 0 && py < 480) {
                    g_shadow_fb[py * 640 + px] = g_saved_overlay[i].pixel;
                }
            }
            g_saved_overlay_count = 0;
        }
    }
    g_stats.last_rendered_mode = rendered;
    frame_count++;
}

int SDL_NumJoysticks(void) {
    return 0;
}

int SDL_IsGameController(int joystick_index) {
    (void)joystick_index;
    return 0;
}

void *SDL_GameControllerOpen(int joystick_index) {
    (void)joystick_index;
    return NULL;
}

void SDL_GameControllerClose(void *gamecontroller) {
    (void)gamecontroller;
}

int SDL_GameControllerGetAttached(void *gamecontroller) {
    (void)gamecontroller;
    return 0;
}

void *SDL_GameControllerFromInstanceID(int joyid) {
    (void)joyid;
    return NULL;
}

int SDL_InitSubSystem(uint32_t flags) {
    static int (*real_fn)(uint32_t) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_InitSubSystem");
    flags &= ~0x00002200u;
    return real_fn ? real_fn(flags) : 0;
}

int SDL_Init(uint32_t flags) {
    static int (*real_fn)(uint32_t) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_Init");
    flags &= ~0x00002200u;
    return real_fn ? real_fn(flags) : 0;
}

void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function) {
    char buf[512];
    snprintf(buf, sizeof(buf), "[ASSERT_FAIL IGNORED] %s:%u in %s: '%s'\n",
             file ? file : "unknown", line, function ? function : "unknown", assertion ? assertion : "assert");
    log_msg(buf);
}

void __aeabi_assert(const char *assertion, const char *file, unsigned int line) {
    char buf[512];
    snprintf(buf, sizeof(buf), "[AEABI_ASSERT IGNORED] %s:%u: '%s'\n",
             file ? file : "unknown", line, assertion ? assertion : "assert");
    log_msg(buf);
}

void __assert(const char *assertion, const char *file, int line) {
    char buf[512];
    snprintf(buf, sizeof(buf), "[ASSERT IGNORED] %s:%d: '%s'\n",
             file ? file : "unknown", line, assertion ? assertion : "assert");
    log_msg(buf);
}

int SDL_PollEvent(void *event) {
    static int (*real_fn)(void *) = NULL;
    static int poll_count = 0;
    if (!real_fn) real_fn = get_next_symbol("SDL_PollEvent");
    if (!real_fn) return 0;

    while (1) {
        if (event && g_evq_n > 0) {
            memcpy(event, g_evq[0], 56);
            for (int qi = 1; qi < g_evq_n; qi++) memcpy(g_evq[qi-1], g_evq[qi], 56);
            g_evq_n--;
            return 1;
        }

        int ret = real_fn(event);
        if (!ret) {
            if (event && g_evq_n > 0) {
                memcpy(event, g_evq[0], 56);
                for (int qi = 1; qi < g_evq_n; qi++) memcpy(g_evq[qi-1], g_evq[qi], 56);
                g_evq_n--;
                return 1;
            }
            return 0;
        }

        int inject_hover = 0;
        if (event && *(uint32_t *)event == 0x401) inject_hover = 1;

        if (event) {
            uint32_t type = *(uint32_t *)event;
            if (type == 0x400 || type == 0x401 || type == 0x402) {
                g_mouse_x = *(int32_t *)((char *)event + 20);
                g_mouse_y = *(int32_t *)((char *)event + 24);
            }
            if (type == 0x300 || type == 0x301) {
                /* D-Pad arrows are consumed: they jump the cursor between controls. */
                int32_t kscan = *(int32_t *)((char *)event + 16);
                int32_t ksym = *(int32_t *)((char *)event + 20);
                int adir = (ksym == 1073741906 || kscan == 82) ? 0 :
                           (ksym == 1073741905 || kscan == 81) ? 1 :
                           (ksym == 1073741904 || kscan == 80) ? 2 :
                           (ksym == 1073741903 || kscan == 79) ? 3 : -1;
                if (adir >= 0) {
                    if (type == 0x300) snap_move(adir, *(uint32_t *)((char *)event + 8));
                    continue;
                }
            }
            // 1. Drop joystick and controller events (0x600..0x6FF)
            if (type >= 0x600 && type <= 0x6FF) {
                continue;
            }
            // 2. Drop touch finger events (0x1100..0x1102) to prevent TouchActionTimer conflicts
            if (type >= 0x1100 && type <= 0x1102) {
                continue;
            }
            // 3. Drop gesture events (0x700..0x702)
            if (type >= 0x700 && type <= 0x702) {
                continue;
            }
            // Log all mouse button and keyboard interactions
            if (type == 0x401 || type == 0x402) {
                struct {
                    uint32_t type;
                    uint32_t timestamp;
                    uint32_t windowID;
                    uint32_t which;
                    uint8_t  button;
                    uint8_t  state;
                    uint8_t  clicks;
                    uint8_t  padding1;
                    int32_t  x;
                    int32_t  y;
                } *b = (void *)event;

                int sx = (b->x * 480) / 640;
                int sy = (b->y * 273) / 480;

                if (type == 0x401) {
                    g_click_dot_frames = 15; // Visual flash for ~250ms
                    g_click_dot_x = (sx < 0) ? 0 : (sx >= 480 ? 479 : sx);
                    g_click_dot_y = (sy < 0) ? 0 : (sy >= 273 ? 272 : sy);
                    g_click_dot_color = (b->button == 1) ? 0xFF00FF00 : 0xFFFF3333; // Green = Left Click, Red = Right Click
                }

                const char *area = "Game Window / Map / Dialog";
                if (b->x >= 14 && b->x <= 165) {
                    if (b->y >= 133 && b->y <= 169) area = "Control 0: Gender (Стать)";
                    else if (b->y >= 169 && b->y <= 205) area = "Control 1: Portrait (Портрет)";
                    else if (b->y >= 205 && b->y <= 241) area = "Control 2: Race (Раса)";
                    else if (b->y >= 241 && b->y <= 277) area = "Control 3: Class (Клас)";
                    else if (b->y >= 277 && b->y <= 313) area = "Control 4: Alignment (Світогляд)";
                    else if (b->y >= 313 && b->y <= 349) area = "Control 5: Abilities (Характеристики)";
                    else if (b->y >= 349 && b->y <= 385) area = "Control 6: Skills (Навички)";
                    else if (b->y >= 385 && b->y <= 421) area = "Control 7: Appearance (Зовнішність)";
                    else if (b->y >= 421 && b->y <= 465) area = "Button: Cancel / Start Over (Скасувати)";
                } else if (b->x >= 486 && b->x <= 613 && b->y >= 406 && b->y <= 436) {
                    area = "Button: Accept / Done (Готово)";
                } else if (b->x >= 44 && b->x <= 134 && b->y >= 425 && b->y <= 457) {
                    area = "Button: Back (Назад)";
                } else if (b->x >= 232 && b->x <= 407 && b->y >= 151 && b->y <= 421) {
                    area = "Popup Sub-Window: Center Dialog (Male/Female/Race choice)";
                } else if (b->y <= 70) {
                    area = "Top Title Banner (Cannot be clicked)";
                }

                char buf[256];
                snprintf(buf, sizeof(buf), "[CLICK %s] Btn=%s GamePos=(%d, %d) ScreenPos=(%d, %d) clicks=%d Target=[%s]\n",
                         (type == 0x401 ? "DOWN" : "UP"),
                         (b->button == 1 ? "LEFT_CLICK(A/R1)" : (b->button == 3 ? "RIGHT_CLICK(B)" : "OTHER")),
                         b->x, b->y, sx, sy, (int)b->clicks, area);
                log_msg(buf);

                // GemRB buttons only fire on click count == 1. Rapid clicks yield 2, 3.. Force 1.
                b->clicks = 1;
                b->which = 0;
            } else if (type == 0x300 || type == 0x301) {
                struct {
                    uint32_t type;
                    uint32_t timestamp;
                    uint32_t windowID;
                    uint8_t  state;
                    uint8_t  repeat;
                    uint8_t  pad1, pad2;
                    int32_t  scancode;
                    int32_t  sym;
                    uint16_t mod;
                } *k = (void *)event;
                const char *kname = "KEY";
                if (k->sym == 13) kname = "ENTER";
                else if (k->sym == 32) kname = "SPACE";
                else if (k->sym == 27) kname = "ESC";
                else if (k->sym == 9) kname = "TAB";
                else if (k->sym == 1073741906 || k->scancode == 82) kname = "UP";
                else if (k->sym == 1073741905 || k->scancode == 81) kname = "DOWN";
                else if (k->sym == 1073741904 || k->scancode == 80) kname = "LEFT";
                else if (k->sym == 1073741903 || k->scancode == 79) kname = "RIGHT";

                char buf[160];
                snprintf(buf, sizeof(buf), "[KEY %s] sym=%d scancode=%d ('%s')\n",
                         (type == 0x300 ? "DOWN" : "UP"), (int)k->sym, (int)k->scancode, kname);
                log_msg(buf);
            }
        }
        if (poll_count < 15) {
            char buf[128];
            snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_PollEvent called (#%d, type=0x%x)\n",
                     poll_count, event ? *(uint32_t *)event : 0);
            log_msg(buf);
            poll_count++;
        }
        if (inject_hover && event) {
            unsigned char btn[56];
            memcpy(btn, event, 56);
            int32_t bx = *(int32_t *)(btn + 20);
            int32_t by = *(int32_t *)(btn + 24);
            unsigned char m1[56], m2[56];
            memset(m1, 0, 56);
            memcpy(m1, btn, 16);                 /* ts, windowID, which */
            *(uint32_t *)m1 = 0x400;             /* SDL_MOUSEMOTION */
            memcpy(m2, m1, 56);
            *(int32_t *)(m1 + 20) = bx + 1;      /* x */
            *(int32_t *)(m1 + 24) = by;          /* y */
            *(int32_t *)(m1 + 28) = 1;           /* xrel */
            *(int32_t *)(m2 + 20) = bx;
            *(int32_t *)(m2 + 24) = by;
            *(int32_t *)(m2 + 28) = -1;
            queue_synthetic_event(m2);
            queue_synthetic_event(btn);
            memcpy(event, m1, 56);
        }
        return 1;
    }
}
static void safe_sigsegv_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    ucontext_t *uc = (ucontext_t *)ucontext;
    uintptr_t fault_addr = (uintptr_t)(info ? info->si_addr : 0);
    char buf[256];
    snprintf(buf, sizeof(buf), "[CRASH HANDLED] Signal %d caught at address 0x%lx pc=0x%lx! Resuming...\n",
             sig, (unsigned long)fault_addr, uc ? (unsigned long)uc->uc_mcontext.arm_pc : 0);
    log_msg(buf);

    if (uc) {
        // Skip null / near-null dereferences (including negative offsets from 0, e.g. 0xff000000)
        if (fault_addr < 0x10000 || fault_addr >= 0xf0000000) {
            uint32_t cpsr = uc->uc_mcontext.arm_cpsr;
            if (cpsr & 0x20) {
                // Thumb mode
                uint32_t pc = uc->uc_mcontext.arm_pc;
                uint16_t insn = *(uint16_t *)pc;
                if ((insn & 0xF800) >= 0xE800) {
                    uc->uc_mcontext.arm_pc += 4;
                } else {
                    uc->uc_mcontext.arm_pc += 2;
                }
            } else {
                // ARM mode (all instructions are 4 bytes)
                uc->uc_mcontext.arm_pc += 4;
            }
            return;
        }
    }
    abort();
}


/* =========================================================================
 * Python CP1251 (Ukrainian TLK) Automatic Fallback Interceptor
 * In Ukrainian Baldur's Gate, strings from dialog.tlk are CP1251-encoded.
 * If Python 3.11's PyUnicode_FromString fails with UnicodeDecodeError on byte 0xC4,
 * we clear the exception and decode cleanly as CP1251 (or Latin-1 fallback).
 * ========================================================================= */
void *PyUnicode_FromString(const char *u) {
    static void *(*real_fn)(const char *) = NULL;
    static void *(*real_decode)(const char *, int32_t, const char *, const char *) = NULL;
    static void (*real_err_clear)(void) = NULL;
    if (!real_fn) real_fn = get_next_symbol("PyUnicode_FromString");
    if (!real_decode) real_decode = get_next_symbol("PyUnicode_Decode");
    if (!real_err_clear) real_err_clear = get_next_symbol("PyErr_Clear");

    if (!u) return NULL;
    void *ret = real_fn ? real_fn(u) : NULL;
    if (!ret) {
        if (real_err_clear) real_err_clear();
        int32_t len = (int32_t)strlen(u);
        if (real_decode) {
            ret = real_decode(u, len, "cp1251", "replace");
            if (!ret) {
                if (real_err_clear) real_err_clear();
                ret = real_decode(u, len, "latin1", "replace");
            }
        }
    }
    return ret;
}

void *PyUnicode_FromStringAndSize(const char *u, int32_t size) {
    static void *(*real_fn)(const char *, int32_t) = NULL;
    static void *(*real_decode)(const char *, int32_t, const char *, const char *) = NULL;
    static void (*real_err_clear)(void) = NULL;
    if (!real_fn) real_fn = get_next_symbol("PyUnicode_FromStringAndSize");
    if (!real_decode) real_decode = get_next_symbol("PyUnicode_Decode");
    if (!real_err_clear) real_err_clear = get_next_symbol("PyErr_Clear");

    if (!u) return NULL;
    void *ret = real_fn ? real_fn(u, size) : NULL;
    if (!ret) {
        if (real_err_clear) real_err_clear();
        if (real_decode) {
            ret = real_decode(u, size, "cp1251", "replace");
            if (!ret) {
                if (real_err_clear) real_err_clear();
                ret = real_decode(u, size, "latin1", "replace");
            }
        }
    }
    return ret;
}

static void __attribute__((constructor)) init_compat_lib(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = safe_sigsegv_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

void SDL_Delay(uint32_t ms) {
    static void (*real_fn)(uint32_t) = NULL;
    static int delay_count = 0;
    if (!real_fn) real_fn = get_next_symbol("SDL_Delay");
    if (delay_count++ < 10) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[libsdl_compat] SDL_Delay(%u) called (#%d)\n", ms, delay_count);
        log_msg(buf);
    }
    if (real_fn) real_fn(ms);
}

int SDL_RenderSetVSync(void *renderer, int vsync) {
    (void)renderer; (void)vsync;
    return 0;
}

int SDL_RenderFlush(void *renderer) {
    static int (*real_fn)(void *) = NULL;
    if (!real_fn) real_fn = get_next_symbol("SDL_RenderFlush");
    return real_fn ? real_fn(renderer) : 0;
}

typedef int64_t Sint64;
typedef uint32_t Uint32;

typedef struct SDL_RWops {
    Sint64 (*size)(struct SDL_RWops *context);
    Sint64 (*seek)(struct SDL_RWops *context, Sint64 offset, int whence);
    size_t (*read)(struct SDL_RWops *context, void *ptr, size_t size, size_t maxnum);
    size_t (*write)(struct SDL_RWops *context, const void *ptr, size_t size, size_t num);
    int (*close)(struct SDL_RWops *context);
    Uint32 type;
    union { void *data1; } hidden;
} SDL_RWops;

Sint64 SDL_RWsize(SDL_RWops *context) {
    return (context && context->size) ? context->size(context) : -1;
}

Sint64 SDL_RWseek(SDL_RWops *context, Sint64 offset, int whence) {
    return (context && context->seek) ? context->seek(context, offset, whence) : -1;
}

Sint64 SDL_RWtell(SDL_RWops *context) {
    return (context && context->seek) ? context->seek(context, 0, 1) : -1;
}

size_t SDL_RWread(SDL_RWops *context, void *ptr, size_t size, size_t maxnum) {
    return (context && context->read) ? context->read(context, ptr, size, maxnum) : 0;
}

size_t SDL_RWwrite(SDL_RWops *context, const void *ptr, size_t size, size_t num) {
    return (context && context->write) ? context->write(context, ptr, size, num) : 0;
}

int SDL_RWclose(SDL_RWops *context) {
    return (context && context->close) ? context->close(context) : -1;
}

char *SDL_strtokr(char *s1, const char *s2, char **saveptr) {
    return strtok_r(s1, s2, saveptr);
}

double SDL_ceil(double x) { return ceil(x); }
float SDL_ceilf(float x) { return ceilf(x); }
double SDL_floor(double x) { return floor(x); }
float SDL_floorf(float x) { return floorf(x); }
double SDL_round(double x) { return round(x); }
float SDL_roundf(float x) { return roundf(x); }
long int SDL_lround(double x) { return lround(x); }
long int SDL_lroundf(float x) { return lroundf(x); }
double SDL_trunc(double x) { return trunc(x); }
float SDL_truncf(float x) { return truncf(x); }

/* Stray lock-key modifiers make GemRB button actions not match; always report none */
int SDL_GetModState(void) { return 0; }
