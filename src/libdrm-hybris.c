/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) 2026 D0gg0Man
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <poll.h>
#include <time.h>
#include <android/android-config.h>
#include <hybris/gralloc/gralloc.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <xf86drmMode.h>
#include <EGL/egl.h>
#include <wayland-server.h>

/*
 * This library is loaded system-wide via ld.so.preload
 * so it lands inside ordinary EGL clients (Qt camera apps, etc)
 * too. Those must pass straight through -- our EGL/DRM/HWC2
 * intercepts are only correct inside the compositor process. We detect the
 * compositor by executable name; everything else gets pass-through behavior.
 */
static int
is_compositor(void)
{
    static int cached = -1;
    if (cached != -1)
        return cached;
    char buf[256] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) {
        cached = 0;
        return 0;
    }
    buf[n] = '\0';
    /* Match on the basename of known compositors that drive HWC2 directly */
    const char *base = strrchr(buf, '/');
    base = base ? base + 1 : buf;
    cached = strcmp(base, "phoc") == 0 ||
             strcmp(base, "gnome-shell") == 0 ||
             strcmp(base, "mutter") == 0 ||
             strcmp(base, "weston") == 0 ||
             strcmp(base, "wlroots") == 0 ||
             strstr(base, "kwin") != NULL |
             strcmp(base, "sway") == 0;
    return cached;
}

static int
is_gnome(void)
{
    const char *d = getenv("XDG_SESSION_DESKTOP");
    if (d && strcmp(d, "gnome") == 0)
        return 1;
    /* gnome-mali unsets XDG_SESSION_DESKTOP before import-environment,
     * so fall back to XDG_CURRENT_DESKTOP. Exact match "GNOME" only --
     * phosh uses "Phosh:GNOME" which must NOT match.
     */
    d = getenv("XDG_CURRENT_DESKTOP");
    return d && strcmp(d, "GNOME") == 0;
}

static void *
resolve_next(const char *name, void *self_addr)
{
    void *fn = dlsym(RTLD_NEXT, name);
    if (!fn || fn == self_addr)
        return NULL;
    /* If the resolved copy lives in a library that also exports our
     * unique marker, it is another copy of this shim -- skip it.
     */
    Dl_info info;
    if (dladdr(fn, &info) && info.dli_fname) {
        void *h = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            int is_dup = dlsym(h, "libdrm_hybris_shim_marker") != NULL;
            dlclose(h);
            if (is_dup)
                return NULL;
        }
    }
    return fn;
}

/* Unique marker exported so resolve_next can identify copies of this shim */
int libdrm_hybris_shim_marker = 1;

#define MAX_DEVICES 32
static struct {
    int device_id;
    int fd;
} devices[MAX_DEVICES] = {
    [0 ... MAX_DEVICES - 1] = { .device_id = 0, .fd = -1 }
};
static int next_device_id = 1;

static void
track_device(int device_id, int fd)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].fd == -1) {
            devices[i].device_id = device_id;
            devices[i].fd = fd;
            return;
        }
    }
    fprintf(stderr, "libdrm-hybris: device table full, closing fd %d\n", fd);
    close(fd);
}
static int
get_fd_for_device(int device_id)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].fd != -1 && devices[i].device_id == device_id)
            return devices[i].fd;
    }
    return -1;
}
static void
untrack_device(int device_id)
{
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].fd != -1 && devices[i].device_id == device_id) {
            devices[i].device_id = 0;
            devices[i].fd = -1;
            return;
        }
    }
}

char *
drmGetRenderDeviceNameFromFd(int fd)
{
    if (!is_compositor()) {
        typedef char *(*fn_t) (int);
        fn_t real=(fn_t)resolve_next("drmGetRenderDeviceNameFromFd", (void *) drmGetRenderDeviceNameFromFd);
        return real ? real(fd) : NULL;
    }
    return strdup("/dev/dri/card0");
}

int
drmGetNodeTypeFromFd(int fd)
{
    if (!is_compositor()) {
        typedef int (*fn_t) (int);
        fn_t real = (fn_t) resolve_next("drmGetNodeTypeFromFd", (void *) drmGetNodeTypeFromFd);
        return real ? real(fd) : -1;
    }
    return DRM_NODE_PRIMARY;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
    static int (*real_fn)(int, uint32_t, drmDevicePtr *) = NULL;
    if (!real_fn)
        real_fn = resolve_next("drmGetDevice2", (void *) drmGetDevice2);
    if (!real_fn)
        return -ENOSYS;
    int r = real_fn(fd, flags, device);
    if (r == 0 && *device && is_compositor()) {
        (*device)->available_nodes |= (1 << DRM_NODE_RENDER);
        (*device)->nodes[DRM_NODE_RENDER] = strdup((*device)->nodes[DRM_NODE_PRIMARY]);
    }
    return r;
}

int
drmGetCap(int fd, uint64_t cap, uint64_t *value)
{
    static int (*real_fn)(int, uint64_t, uint64_t *) = NULL;
    if (!real_fn)
        real_fn = resolve_next("drmGetCap", (void *) drmGetCap);
    if (is_compositor()) {
        switch (cap) {
        case DRM_CAP_PRIME:
            *value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
            return 0;
        case DRM_CAP_CRTC_IN_VBLANK_EVENT:
            *value = 1;
            return 0;
        case DRM_CAP_TIMESTAMP_MONOTONIC:
            *value = 1;
            return 0;
        default:
            break;
        }
    }
    return real_fn ? real_fn(fd, cap, value) : -ENOSYS;
}

int
drmSetClientCap(int fd, uint64_t cap, uint64_t value)
{
    static int (*real_fn)(int, uint64_t, uint64_t) = NULL;
    if (!real_fn)
        real_fn = resolve_next("drmSetClientCap", (void *) drmSetClientCap);
    int r = real_fn ? real_fn(fd, cap, value) : -ENOSYS;
    /* ATOMIC + UNIVERSAL_PLANES must really be set on the fd so
     * the kernel exposes the primary/cursor planes and the atomic uAPI -- pass
     * them through (the MediaTek DRM is a real atomic driver and accepts them).
     * Only if the driver rejects one (e.g. because the HWC2 composer owns the
     * master) do we pretend success, so wlroots still takes the atomic path
     * where our faked drmModeAtomicCommit() works. Not for gnome/mutter. */
    if (r != 0 && is_compositor() && !is_gnome() &&
        (cap == DRM_CLIENT_CAP_ATOMIC || cap == DRM_CLIENT_CAP_UNIVERSAL_PLANES))
        return 0;
    return r;
}

int
drmIsKMS(int fd)
{
    if (!is_compositor()) {
        typedef int (*fn_t) (int);
        fn_t real = (fn_t) resolve_next("drmIsKMS", (void *) drmIsKMS);
        return real ? real(fd) : 0;
    }
    return 1;
}

int
drmModeCreateLease(int fd,
                   const uint32_t *o,
                   int n,
                   int f,
                   uint32_t *id)
{
    if (!is_compositor()) {
        typedef int (*fn_t) (int, const uint32_t *, int, int, uint32_t *);
        fn_t real = (fn_t) resolve_next("drmModeCreateLease", (void *) drmModeCreateLease);
        return real ? real(fd, o, n, f, id) : -EINVAL;
    }
    return -EINVAL;
}

EGLBoolean
eglGetConfigAttrib(EGLDisplay dpy,
                   EGLConfig config,
                   EGLint attribute,
                   EGLint *value)
{
    static EGLBoolean (*real_fn)(EGLDisplay, EGLConfig, EGLint, EGLint *) = NULL;
    if (!real_fn)
        real_fn = resolve_next("eglGetConfigAttrib", (void *) eglGetConfigAttrib);
    if (!real_fn)
        return EGL_FALSE;
    EGLBoolean r = real_fn(dpy, config, attribute, value);
    /* Visual-id fix is ONLY for wlroots/phoc (phosh), which needs a non-zero
     * EGL_NATIVE_VISUAL_ID to select a config. It must NOT run for:
     *  - clients (Qt camera apps) -- they need the unmodified value
     *  - gnome/mutter -- the drmadapter EGL platform does the proper fourcc
     *    mapping itself; our forcing it to 1 breaks mutter's GBM format match
     *    ("No EGL config matching supported GBM format found"). */
    if (r && is_compositor() && !is_gnome() &&
        attribute == EGL_NATIVE_VISUAL_ID && *value == 0) {
        EGLint red=0, green=0, blue=0, alpha=0;
        real_fn(dpy, config, EGL_RED_SIZE, &red);
        real_fn(dpy, config, EGL_GREEN_SIZE, &green);
        real_fn(dpy, config, EGL_BLUE_SIZE, &blue);
        real_fn(dpy, config, EGL_ALPHA_SIZE, &alpha);
        if (red == 8 && green == 8 && blue == 8 && alpha == 8)
            *value = 1;
    }
    return r;
}

typedef void *(*server_wlegl_create_t)(struct wl_display *);

struct wl_display *
wl_display_create(void)
{
    typedef struct wl_display* (*fn_t) (void);
    fn_t real = dlsym(RTLD_NEXT, "wl_display_create");
    struct wl_display *dpy = real();
    if (dpy) {
        void *lib = dlopen("libhybris-platformcommon.so", RTLD_NOW|RTLD_NOLOAD);
        if (!lib)
            lib = dlopen("libhybris-platformcommon.so", RTLD_NOW);
        if (lib) {
            server_wlegl_create_t create = dlsym(lib, "_Z19server_wlegl_createP10wl_display");
            if (create) {
                void *wlegl = create(dpy);
                (void) wlegl;
            }
        }
    }
    return dpy;
}

typedef void hwc2_compat_display_t;
typedef int32_t hwc2_error_t;
#define HWC2_ERROR_NONE 0

hwc2_error_t
hwc2_compat_display_set_vsync_enabled(hwc2_compat_display_t *display, int32_t enabled)
{
    static hwc2_error_t (*real_fn) (hwc2_compat_display_t *, int32_t) = NULL;
    if (!real_fn)
        real_fn = resolve_next("hwc2_compat_display_set_vsync_enabled",
                               (void *) hwc2_compat_display_set_vsync_enabled);
    if (real_fn)
        real_fn(display, enabled);
    return HWC2_ERROR_NONE;
}


typedef void HWCNativeWindow;

void
HWCNativeWindowSetBufferCount(HWCNativeWindow *win, int count)
{
    static void (*real_fn)(HWCNativeWindow *, int) = NULL;
    if (!real_fn)
        real_fn = resolve_next("HWCNativeWindowSetBufferCount",
                               (void *) HWCNativeWindowSetBufferCount);
    /* Only force double-buffering in the compositor. Clients keep their count. */
    if (real_fn)
        real_fn(win, is_compositor() ? 2 : count);
}

void
HWCNativeBufferSetFence(ANativeWindowBuffer *buffer, int fd)
{
    static void (*real_fn)(ANativeWindowBuffer *, int) = NULL;
    if (!real_fn)
        real_fn = resolve_next("HWCNativeBufferSetFence",
                               (void *) HWCNativeBufferSetFence);
    /* Only discard fences in the compositor. Clients need their real fence
     * preserved or buffer sync breaks (camera preview texture corruption). */
    if (!is_compositor()) {
        if (real_fn)
            real_fn(buffer, fd);
        return;
    }
    if (real_fn)
        real_fn(buffer, -1);
    if (fd >= 0)
        close(fd);
}

#define MAX 64

static uint32_t frame_w = 0, frame_h = 0;

static struct {
    uint32_t prime_fd;
    buffer_handle_t gralloc;
} gmap[MAX];
static int gmap_n = 0;

static struct {
    uint32_t gem;
    uint32_t fb_id;
} fmap[MAX];
static int fmap_n = 0;

static uint32_t dumb_handle = 0, dumb_fb_id = 0, dumb_pitch = 0;
static void *dumb_map = NULL;
static size_t dumb_size = 0;
static uint32_t next_fake = 0x80000000u;
static __thread int in_hook = 0;

typedef int (*ioctl_t) (int, unsigned long, ...);
static ioctl_t real_ioctl = NULL;
int ioctl(int fd, unsigned long request, ...);

static void
ensure_real(void)
{
    if (!real_ioctl)
        real_ioctl = (ioctl_t) resolve_next("ioctl", (void *) ioctl);
}

/* Env-gated tracing (LIBDRM_HYBRIS_TRACE=1). */
static int trace_on = -1;

static void
tracef(const char *fmt, ...)
{
    if (trace_on < 0)
        trace_on = getenv("LIBDRM_HYBRIS_TRACE") ? 1 : 0;

    if (!trace_on)
        return;

    int saved = in_hook;
    in_hook = 1;

    FILE *f = fopen("/tmp/libdrm-hybris-trace.log", "a");
    if (f) {
        va_list args;

        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);

        fclose(f);
    }

    in_hook = saved;
}

/*
 * Synthetic page-flip completion events.
 *
 * Decouple Mutter's frame clock from card0 DRM master, which the HWC2 composer
 * permanently owns. On an output reconfigure, the timing flips start returning
 * EACCES. We then synthesize DRM_EVENT_FLIP_COMPLETE ourselves, paced at the
 * interval measured from the real flips. The poll/read hooks fast-path out via
 * g_synth_active.
 */
static int g_synth_active = 0;
static int g_drm_fd = -1;
static int g_synth_pending = 0;
static uint32_t g_synth_crtc = 0;
static uint64_t g_synth_user = 0;
static uint64_t g_interval_ns = 0;
static uint64_t g_last_flip_ns = 0;
static uint64_t g_deadline_ns = 0;
static uint32_t g_synth_seq = 0;

static ssize_t (*real_read)(int, void *, size_t) = NULL;
static int (*real_poll)(struct pollfd *, nfds_t, int) = NULL;
static int (*real_ppoll)(struct pollfd *,
                         nfds_t,
                         const struct timespec *,
                         const sigset_t *) = NULL;

static uint64_t
now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);

    return (uint64_t) t.tv_sec * 1000000000ull + t.tv_nsec;
}

static void
synth_note_flip(void)
{
    uint64_t n = now_ns();

    if (g_last_flip_ns) {
        uint64_t d = n - g_last_flip_ns;

        if (d > 1000000ull && d < 100000000ull)
            g_interval_ns = g_interval_ns ? (g_interval_ns * 7 + d) / 8 : d;
    }

    g_last_flip_ns = n;
}

static void
synth_arm(uint32_t crtc, uint64_t user_data)
{
    g_synth_crtc = crtc;
    g_synth_user = user_data;
    g_deadline_ns = now_ns() + (g_interval_ns ? g_interval_ns : 8333333ull);
    g_synth_pending = 1;
    g_synth_active = 1;
}

ssize_t
read(int fd, void *buf, size_t count)
{
    if (!real_read)
        real_read = (ssize_t (*) (int, void *, size_t)) resolve_next("read", (void *) read);

    if (!g_synth_active || fd != g_drm_fd || !g_synth_pending || in_hook)
        return real_read(fd, buf, count);

    if (now_ns() < g_deadline_ns)
        return real_read(fd, buf, count);

    if (count < sizeof(struct drm_event_vblank))
        return real_read(fd, buf, count);

    struct drm_event_vblank ev;

    memset(&ev, 0, sizeof(ev));

    ev.base.type = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof(ev);
    ev.user_data = g_synth_user;

    uint64_t n = now_ns();

    ev.tv_sec = (uint32_t) (n / 1000000000ull);
    ev.tv_usec = (uint32_t) ((n / 1000ull) % 1000000ull);
    ev.sequence = ++g_synth_seq;
    ev.crtc_id = g_synth_crtc;

    memcpy(buf, &ev, sizeof(ev));

    g_synth_pending = 0;

    tracef("SYNTH read delivered crtc=%u user=0x%llx seq=%u\n",
           g_synth_crtc,
           (unsigned long long) g_synth_user,
           g_synth_seq);

    return sizeof(ev);
}

static int
synth_poll_fixup(struct pollfd *fds, nfds_t n, struct timespec *cap)
{
    if (!g_synth_active || !g_synth_pending)
        return -1;

    int idx = -1;

    for (nfds_t i = 0; i < n; i++) {
        if (fds[i].fd == g_drm_fd && (fds[i].events & POLLIN)) {
            idx = (int) i;
            break;
        }
    }

    if (idx < 0)
        return -1;

    int64_t rem = (int64_t) g_deadline_ns - (int64_t) now_ns();

    if (rem <= 0) {
        fds[idx].revents |= POLLIN;
        return idx;
    }

    if (cap) {
        cap->tv_sec = rem / 1000000000;
        cap->tv_nsec = rem % 1000000000;
    }

    return -2;
}

int
poll(struct pollfd *fds, nfds_t n, int timeout)
{
    if (!real_poll)
        real_poll = (int (*) (struct pollfd *, nfds_t, int)) resolve_next("poll", (void *) poll);

    if (!g_synth_active || in_hook)
        return real_poll(fds, n, timeout);

    struct timespec cap;
    int r = synth_poll_fixup(fds, n, &cap);

    if (r >= 0)
        return 1;

    if (r == -2) {
        int ms = (int) ((cap.tv_sec * 1000000000ll + cap.tv_nsec) / 1000000) + 1;

        if (timeout < 0 || timeout > ms)
            timeout = ms;
    }

    int got = real_poll(fds, n, timeout);

    if (got == 0 && g_synth_pending && now_ns() >= g_deadline_ns) {
        for (nfds_t i = 0; i < n; i++) {
            if (fds[i].fd == g_drm_fd && (fds[i].events & POLLIN)) {
                fds[i].revents |= POLLIN;
                return 1;
            }
        }
    }

    return got;
}

int
ppoll(struct pollfd *fds,
      nfds_t n,
      const struct timespec *to,
      const sigset_t *ss)
{
    if (!real_ppoll)
        real_ppoll = (int (*) (struct pollfd *,
                               nfds_t,
                               const struct timespec *,
                               const sigset_t *)) resolve_next("ppoll", (void *) ppoll);

    if (!g_synth_active || in_hook)
        return real_ppoll(fds, n, to, ss);

    struct timespec cap;
    int r = synth_poll_fixup(fds, n, &cap);

    if (r >= 0)
        return 1;

    const struct timespec *eff = to;

    if (r == -2) {
        uint64_t to_ns = to
            ? (uint64_t) to->tv_sec * 1000000000ull + to->tv_nsec
            : UINT64_MAX;
        uint64_t cap_ns = (uint64_t) cap.tv_sec * 1000000000ull + cap.tv_nsec;

        if (to_ns > cap_ns)
            eff = &cap;
    }

    int got = real_ppoll(fds, n, eff, ss);

    if (got == 0 && g_synth_pending && now_ns() >= g_deadline_ns) {
        for (nfds_t i = 0; i < n; i++) {
            if (fds[i].fd == g_drm_fd && (fds[i].events & POLLIN)) {
                fds[i].revents |= POLLIN;
                return 1;
            }
        }
    }

    return got;
}

static int gmap_evict = 0;

void
drm_shim_register_bo(uint32_t prime_fd, buffer_handle_t gralloc)
{
    for (int i = 0; i < gmap_n; i++) {
        if (gmap[i].prime_fd == prime_fd) {
            gmap[i].gralloc = gralloc;
            return;
        }
    }

    int slot;
    if (gmap_n < MAX) {
        slot = gmap_n++;
    } else {
        slot = gmap_evict;
        gmap_evict = (gmap_evict + 1) % MAX;
    }
    gmap[slot].prime_fd = prime_fd;
    gmap[slot].gralloc = gralloc;
}

static buffer_handle_t
find_gralloc(uint32_t gem)
{
    for (int i = 0; i < gmap_n; i++) {
        if (gmap[i].prime_fd == gem)
            return gmap[i].gralloc;
    }
    return NULL;
}

static buffer_handle_t
find_by_fb(uint32_t fb_id)
{
    for (int i = 0; i < fmap_n; i++) {
        if (fmap[i].fb_id == fb_id)
            return find_gralloc(fmap[i].gem);
    }
    return NULL;
}

static int fmap_evict = 0;

static void
fmap_insert(uint32_t gem, uint32_t fb_id)
{
    for (int i = 0; i < fmap_n; i++) {
        if (fmap[i].gem == gem) {
            fmap[i].fb_id = fb_id;
            return;
        }
    }
    int slot;
    if (fmap_n < MAX) {
        slot = fmap_n++;
    } else {
        slot = fmap_evict;
        fmap_evict = (fmap_evict + 1) % MAX;
    }
    fmap[slot].gem = gem;
    fmap[slot].fb_id = fb_id;
}

/* NB: the dumb buffer is never actually scanned out (the HWC2 composer owns the
 * CRTC; real pixels reach the panel via the drmadapter EGL path). HOWEVER the
 * per-frame gralloc lock/unlock in copy_to_dumb() is load-bearing -- removing it
 * makes gnome-session stop the shell after ~43s (verified on-device). The lock/
 * unlock is evidently a GPU/cache sync the HWC2 presentation relies on, so keep
 * it. (The dumb FB also gives mutter a real FB id to page-flip to.) */
static void
copy_to_dumb(buffer_handle_t h)
{
    if (!dumb_map || !h || !frame_w || !frame_h)
        return;
    void *src = NULL;
    if (hybris_gralloc_lock(h, GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN, 0, 0, frame_w, frame_h, &src) || !src)
        return;
    uint8_t *d = dumb_map, *s = src;
    for (uint32_t y = 0; y < frame_h; y++) {
        memcpy(d + y * dumb_pitch, s + y * dumb_pitch, frame_w * 4);
    }
    hybris_gralloc_unlock(h);
}

static int
init_dumb(int fd)
{
    if (dumb_map)
        return 0;
    if (!frame_w || !frame_h)
        return -1;
    int saved = in_hook;

    in_hook = 1;
    struct drm_mode_create_dumb cd = {
        .height = frame_h,
        .width = frame_w,
        .bpp = 32,
    };

    if (real_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
        in_hook = saved;
        return -1;
    }

    dumb_handle = cd.handle;
    dumb_pitch = cd.pitch;
    dumb_size = cd.size;
    struct drm_mode_fb_cmd fb = {
        .width = frame_w,
        .height = frame_h,
        .pitch = dumb_pitch,
        .bpp = 32,
        .depth = 24,
        .handle = dumb_handle
    };

    if (real_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) == 0) {
        dumb_fb_id = fb.fb_id;
    } else {
        struct drm_mode_fb_cmd2 fb2 = {
            .width = frame_w,
            .height = frame_h,
            .pixel_format = DRM_FORMAT_XRGB8888
        };
        fb2.handles[0] = dumb_handle;
        fb2.pitches[0] = dumb_pitch;
        if (real_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2)) {
            in_hook = saved;
            return -1;
        }
        dumb_fb_id = fb2.fb_id;
    }

    struct drm_mode_map_dumb md = {
        .handle = dumb_handle
    };
    if (real_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        in_hook = saved;
        return -1;
    }

    dumb_map = mmap(NULL, dumb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (dumb_map == MAP_FAILED) {
        dumb_map = NULL;
        in_hook = saved;
        return -1;
    }

    in_hook = saved;

    return 0;
}

int
drmModeAddFB2WithModifiers(int fd,
                           uint32_t w,
                           uint32_t h,
                           uint32_t fmt,
                           const uint32_t handles[4],
                           const uint32_t pitches[4],
                           const uint32_t offsets[4],
                           const uint64_t mod[4],
                           uint32_t *buf_id,
                           uint32_t flags)
{
    if (!is_compositor()) {
        typedef int (*fn_t) (int, uint32_t, uint32_t, uint32_t, const uint32_t *,
                             const uint32_t *, const uint32_t *, const uint64_t *, uint32_t *, uint32_t);
        fn_t real = (fn_t) resolve_next("drmModeAddFB2WithModifiers",
                                        (void *) drmModeAddFB2WithModifiers);
        return real ? real(fd, w, h, fmt, handles, pitches, offsets, mod, buf_id, flags) : -ENOSYS;
    }

    if (!frame_w) {
        frame_w = w;
        frame_h = h;
    }

    if (!dumb_map)
        init_dumb(fd);
    uint32_t id = next_fake++;

    *buf_id = id;
    fmap_insert(handles[0], id);

    return 0;
}

int
drmModeAddFB2(int fd,
              uint32_t w,
              uint32_t h,
              uint32_t fmt,
              const uint32_t handles[4],
              const uint32_t pitches[4],
              const uint32_t offsets[4],
              uint32_t *buf_id,
              uint32_t flags)
{
    if (!is_compositor()) {
        typedef int (*fn_t) (int, uint32_t, uint32_t, uint32_t, const uint32_t *,
                             const uint32_t *, const uint32_t *, uint32_t *, uint32_t);
        fn_t real = (fn_t) resolve_next("drmModeAddFB2", (void *) drmModeAddFB2);
        return real ? real(fd, w, h, fmt, handles, pitches, offsets, buf_id, flags) : -ENOSYS;
    }

    if (!frame_w) {
        frame_w = w;
        frame_h = h;
    }

    if (!dumb_map)
        init_dumb(fd);

    uint32_t id = next_fake++;

    *buf_id = id;
    fmap_insert(handles[0], id);

    return 0;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
    typedef int (*fn_t) (int, int, uint32_t *);
    fn_t real = (fn_t) resolve_next("drmPrimeFDToHandle", (void *) drmPrimeFDToHandle);

    if (!is_compositor() || is_gnome())
        return real ? real(fd, prime_fd, handle) : -ENOSYS;

    int r = real ? real(fd, prime_fd, handle) : -EACCES;

    /* wlroots imports the gbm_bo's PRIME fd for scan-out, which needs DRM
     * master -- the HWC2 composer owns it, so the real import returns EACCES.
     * Use the prime fd itself as the GEM handle: gbm_hybris registered the
     * prime_fd -> gralloc mapping (gmap) and find_gralloc()/find_by_fb() key on
     * the prime fd, so AddFB2/commit can still recover the buffer. */
    if (r != 0 && handle) {
        *handle = (uint32_t) prime_fd;
        r = 0;
    }

    return r;
}

int
drmModeRmFB(int fd, uint32_t id)
{
    if (!is_compositor()) {
        typedef int (*fn_t) (int, uint32_t);
        fn_t real = (fn_t) resolve_next("drmModeRmFB", (void *) drmModeRmFB);
        return real ? real(fd,id) : 0;
    }
    return 0;
}

int
drmModeSetCrtc(int fd,
               uint32_t crtcId,
               uint32_t bufferId,
               uint32_t x,
               uint32_t y,
               uint32_t *connectors,
               int count,
               drmModeModeInfoPtr mode)
{
    if (!is_compositor()) {
        typedef int (*fn_t) (int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, int, drmModeModeInfoPtr);
        fn_t real = (fn_t) resolve_next("drmModeSetCrtc", (void *) drmModeSetCrtc);
        return real ? real(fd, crtcId, bufferId, x, y, connectors, count, mode) : -ENOSYS;
    }

    if (!dumb_map)
        init_dumb(fd);

    return 0;
}

int
drmModePageFlip(int fd,
                uint32_t crtc_id,
                uint32_t fb_id,
                uint32_t flags,
                void *ud)
{
    typedef int (*fn_t) (int, uint32_t, uint32_t, uint32_t, void *);
    fn_t real = (fn_t) resolve_next("drmModePageFlip", (void *) drmModePageFlip);
    if (!is_compositor())
        return real ? real(fd, crtc_id, fb_id, flags, ud) : -ENOSYS;

    g_drm_fd = fd; synth_note_flip();
    buffer_handle_t h = find_by_fb(fb_id);

    copy_to_dumb(h);
    int r = real ? real(fd, crtc_id, dumb_fb_id ? dumb_fb_id : fb_id, flags, ud) : 0;
    if (r == -EACCES) {
        if (flags & DRM_MODE_PAGE_FLIP_EVENT)
            synth_arm(crtc_id, (uint64_t) (uintptr_t) ud);
        r = 0;
    }
    return r;
}

int
drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *ud)
{
    if (!is_compositor()) {
        typedef int (*fn_t) (int, drmModeAtomicReqPtr, uint32_t, void *);
        fn_t real = (fn_t) resolve_next("drmModeAtomicCommit", (void *) drmModeAtomicCommit);
        return real ? real(fd,req,flags,ud) : -ENOSYS;
    }

    for (int i = fmap_n - 1; i >= 0; i--) {
        buffer_handle_t h = find_gralloc(fmap[i].gem);

        if (h) {
            copy_to_dumb(h);
            break;
        }
    }

    return 0;
}

int
ioctl(int fd, unsigned long request, ...)
{
    ensure_real();
    va_list args;

    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    uint32_t magic = (request >> 8) & 0xff;
    if (magic != 0x64)
        return real_ioctl(fd, request, arg);

    /* Only the compositor's DRM ioctls drive the fake KMS framebuffer.
     * Client processes (camera etc) must reach the real DRM driver intact.
     */
    if (!is_compositor())
        return real_ioctl(fd, request, arg);
    if (in_hook)
        return real_ioctl(fd, request, arg);

    uint32_t nr = request & 0xff;
    in_hook = 1;

    int ret;

    if (nr == DRM_IOCTL_MODE_ADDFB2) {
        uint32_t *fb = arg;
        uint32_t w = fb[0];
        uint32_t h = fb[1];
        uint32_t gem = fb[5];

        if (!frame_w) {
            frame_w = w;
            frame_h = h;
        }

        if (!dumb_map)
            init_dumb(fd);
        uint32_t id = next_fake++;

        fb[6] = id;
        fmap_insert(gem, id);
        ret = 0;
    } else if (nr == 0xaf) {
        ret = real_ioctl(fd, request, arg);
    } else if (nr == 0xa2) {
        if (!dumb_map)
            init_dumb(fd);

        real_ioctl(fd, request, arg);
        ret = 0;
    } else if (nr == 0xb0 || nr == 0xb6) {
        struct drm_mode_crtc_page_flip *flip = arg;
        g_drm_fd=fd;
        synth_note_flip();
        buffer_handle_t h = find_by_fb(flip->fb_id);

        copy_to_dumb(h);

        if (dumb_fb_id)
            flip->fb_id = dumb_fb_id;
        ret = real_ioctl(fd, request, arg);
        if (ret != 0 && errno == EACCES) {
            if (flip->flags & DRM_MODE_PAGE_FLIP_EVENT)
                synth_arm(flip->crtc_id, flip->user_data);
            ret=0;
        }
    } else if (nr == 0xbc) {
        for (int i = fmap_n - 1; i >= 0; i--) {
            buffer_handle_t h = find_gralloc(fmap[i].gem);
            if (h) {
                copy_to_dumb(h);
                break;
            }
        }

        ret = 0;
    } else if (nr == 0x11) {
        ret = 0;
    } else {
        ret = real_ioctl(fd, request, arg);
    }
    in_hook = 0;

    return ret;
}
