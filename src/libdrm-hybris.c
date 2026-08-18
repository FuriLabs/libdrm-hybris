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
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <android/android-config.h>
#include <hybris/gralloc/gralloc.h>
#include <hardware/gralloc.h>
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
    /*
     * gnome-mali unsets XDG_SESSION_DESKTOP before import-environment,
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
    /*
     * If the resolved copy lives in a library that also exports our
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
    static int (*real_fn) (int, uint64_t, uint64_t *) = NULL;
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
    static int (*real_fn) (int, uint64_t, uint64_t) = NULL;
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
    static EGLBoolean (*real_fn) (EGLDisplay, EGLConfig, EGLint, EGLint *) = NULL;
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
    static void (*real_fn) (HWCNativeWindow *, int) = NULL;
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
static uint64_t g_interval_ns = 0;
static uint64_t g_last_flip_ns = 0;
static uint32_t g_synth_seq = 0;

/* Armed-but-undelivered synthetic flips. A QUEUE, not a single slot: flips are
 * armed from several paths (atomic commits, legacy page-flip ioctls during
 * modesets) and wlroots' user_data is a heap object (wlr_drm_page_flip) whose
 * completion MUST be delivered exactly once. With a single slot, a second arm
 * overwrote an undelivered first flip -- that flip's completion was lost,
 * wlroots' conn->pending_page_flip never cleared, and it refused every further
 * commit: the compositor froze (this was the intermittent cold-boot black and
 * the post-blank freeze). */
#define SYNTH_QMAX 16
static struct { uint32_t crtc; uint64_t user; uint64_t deadline; } g_synth_q[SYNTH_QMAX];
static int g_synth_qh = 0;   /* head index */
static int g_synth_qn = 0;   /* queued count; >0 == "pending" */
/*
 * When the compositor nests the DRM fd inside an epoll instance and waits on
 * that epoll fd from an outer poll/ppoll (phoc: wl_event_loop epoll fd polled
 * by the GLib main loop), the outer wait must treat the epoll fd as a synth
 * target too, then the epoll_wait hook injects the inner DRM readiness.
 */
static int g_epoll_fd = -1;
static epoll_data_t g_drm_epoll_data;
static int g_drm_epoll_valid = 0;
/* Private eventfd we add to the compositor's DRM epoll so that arming a synth
 * flip makes that epoll (and thus the outer GLib loop nesting it) OS-readable,
 * waking the compositor to drill in and consume the flip even when it is
 * otherwise idle (post-blank). Without this the outer loop blocks and the flip
 * is only delivered when some real event happens to wake it (sparse) -> the
 * shell freezes visually after an idle blank. Identified via ev.data.ptr. */
static int          g_wake_fd = -1;
/* Companion timerfd in the same epoll: wakes the loop AT the flip deadline
 * (the compositor dispatches the inner epoll with timeout 0, so without a
 * timer the vsync-aligned deadline would only be met on unrelated activity). */
static int          g_timer_fd = -1;
static int
is_synth_fd(int fd)
{
    return fd == g_drm_fd || (g_drm_epoll_valid && fd == g_epoll_fd);
}

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

static int g_stamp = -1;
static void
STAMP(const char *tag)
{
    if (g_stamp < 0)
        g_stamp = getenv("LIBDRM_HYBRIS_STAMP") ? 1 : 0;
    if (g_stamp)
        fprintf(stderr, "STAMP %8.3f %s\n", (double)(now_ns() % 100000000000ull) / 1e6, tag);
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

/*
 * Pace the synthetic flip completion at the panel's real vsync period (queried
 * from HWC2, ~8.33ms at 120Hz). Delivering it immediately re-enters wlroots
 * before its atomic-commit state settles and the loop stalls after a few
 * frames; one vsync of delay mirrors real hardware and keeps it stable. The
 * period is set once from drmadapter via drm_shim_set_vsync_period(); 8.33ms
 * is only a startup fallback.
 */
static uint64_t g_vsync_ns = 8333333ull;
void
drm_shim_set_vsync_period(uint64_t ns)
{
    if (ns > 1000000ull && ns < 100000000ull)
        g_vsync_ns = ns;
}

/* Real panel vsync timestamps (CLOCK_MONOTONIC ns), stamped by drmadapter's
 * HWC2 vsync callback. Used to phase-align the synthetic flip deadlines to the
 * actual vblank: a free-running flip clock drifts against the panel and the
 * presents periodically straddle the composer's latch point -> mixed old/new
 * frames = flicker under motion (session-random severity = boot phase). */
static uint64_t g_vsync_stamp_ns = 0;
void drm_shim_vsync_stamp(int64_t ts) { if (ts > 0) g_vsync_stamp_ns = (uint64_t)ts; }

static void
synth_arm(uint32_t crtc, uint64_t user_data)
{
    if (g_synth_qn >= SYNTH_QMAX) {
        /* Should never happen (wlroots keeps <=1 flip in flight per connector);
         * losing a flip means a permanent freeze, so scream if it ever does. */
        fprintf(stderr, "libdrm-hybris: SYNTH QUEUE OVERFLOW, dropping oldest flip!\n");
        g_synth_qh = (g_synth_qh + 1) % SYNTH_QMAX;
        g_synth_qn--;
    }
    int tail = (g_synth_qh + g_synth_qn) % SYNTH_QMAX;
    g_synth_q[tail].crtc = crtc;
    g_synth_q[tail].user = user_data;
    {
        uint64_t now = now_ns();
        uint64_t dl;
        if (g_vsync_stamp_ns && now > g_vsync_stamp_ns &&
            now - g_vsync_stamp_ns < 1000000000ull) {
            /* Fresh vsync reference: deliver at the next real vblank boundary. */
            uint64_t phase = (now - g_vsync_stamp_ns) % g_vsync_ns;
            dl = now + (g_vsync_ns - phase);
        } else {
            dl = now + g_vsync_ns; /* fallback: free-running period */
        }
        g_synth_q[tail].deadline = dl;
    }
    g_synth_qn++;
    g_synth_active = 1;
    /* Kick the wake eventfd so the compositor's (possibly idle) outer event loop
     * wakes and drills into the DRM epoll to consume this flip. Drained+hidden in
     * epoll_synth() so the compositor never sees the eventfd itself. */
    if (g_wake_fd >= 0) { uint64_t one = 1; ssize_t w = write(g_wake_fd, &one, sizeof one); (void)w; }
    /* Arm the deadline timer for the queue head (absolute monotonic). */
    if (g_timer_fd >= 0) {
        struct itimerspec its; memset(&its, 0, sizeof its);
        uint64_t hd = g_synth_q[g_synth_qh].deadline;
        its.it_value.tv_sec = hd / 1000000000ull;
        its.it_value.tv_nsec = hd % 1000000000ull;
        timerfd_settime(g_timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
    }
}

ssize_t
read(int fd, void *buf, size_t count)
{
    if (!real_read)
        real_read = (ssize_t (*) (int, void *, size_t)) resolve_next("read", (void *) read);

    if (!g_synth_active || fd != g_drm_fd || g_synth_qn <= 0 || in_hook)
        return real_read(fd, buf, count);

    if (count < sizeof(struct drm_event_vblank))
        return real_read(fd, buf, count);

    struct drm_event_vblank ev;

    memset(&ev, 0, sizeof(ev));

    ev.base.type = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof(ev);
    ev.user_data = g_synth_q[g_synth_qh].user;

    uint64_t n = now_ns();

    ev.tv_sec = (uint32_t) (n / 1000000000ull);
    ev.tv_usec = (uint32_t) ((n / 1000ull) % 1000000ull);
    ev.sequence = ++g_synth_seq;
    ev.crtc_id = g_synth_q[g_synth_qh].crtc;

    memcpy(buf, &ev, sizeof(ev));

    g_synth_qh = (g_synth_qh + 1) % SYNTH_QMAX;
    g_synth_qn--;

    STAMP("read-deliver");
    tracef("SYNTH read delivered crtc=%u user=0x%llx seq=%u qn=%d\n",
           ev.crtc_id, (unsigned long long)ev.user_data, g_synth_seq, g_synth_qn);

    return sizeof(ev);
}

static int
synth_poll_fixup(struct pollfd *fds, nfds_t n)
{
    if (!g_synth_active || g_synth_qn <= 0)
        return -1;

    for (nfds_t i = 0; i < n; i++) {
        if (is_synth_fd(fds[i].fd) && (fds[i].events & POLLIN)) {
            fds[i].revents |= POLLIN;
            return (int) i;
        }
    }

    return -1;
}

int
poll(struct pollfd *fds, nfds_t n, int timeout)
{
    if (!real_poll)
        real_poll = (int (*) (struct pollfd *, nfds_t, int)) resolve_next("poll", (void *) poll);

    if (!g_synth_active || in_hook)
        return real_poll(fds, n, timeout);

    if (synth_poll_fixup(fds, n) >= 0) return 1;

    int got = real_poll(fds, n, timeout);

    if (got == 0 && g_synth_qn > 0 && now_ns() >= g_synth_q[g_synth_qh].deadline) {
        for (nfds_t i = 0; i < n; i++) {
            if (is_synth_fd(fds[i].fd) && (fds[i].events & POLLIN)) {
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

    if (synth_poll_fixup(fds, n) >= 0) return 1;
    int got = real_ppoll(fds, n, to, ss);

    if (got == 0 && g_synth_qn > 0 && now_ns() >= g_synth_q[g_synth_qh].deadline) {
        for (nfds_t i = 0; i < n; i++) {
            if (is_synth_fd(fds[i].fd) && (fds[i].events & POLLIN)) {
                fds[i].revents |= POLLIN;
                return 1;
            }
        }
    }

    return got;
}

/* epoll variants of the synth delivery, for compositors whose event loop waits
 * on the DRM fd via epoll rather than poll/ppoll (wlroots/wayland uses epoll;
 * mutter's GLib loop uses ppoll). We capture the epoll instance + the
 * wl_event_source data the compositor associated with the DRM fd, then inject a
 * readiness event when a synthetic flip is due. The compositor then read()s the
 * fd and our read() hook hands back the DRM_EVENT_FLIP_COMPLETE. */
static int (*real_epoll_ctl)(int,int,int,struct epoll_event*) = NULL;
static int (*real_epoll_wait)(int,struct epoll_event*,int,int) = NULL;
static int (*real_epoll_pwait)(int,struct epoll_event*,int,int,const sigset_t*) = NULL;

static int fd_is_drm(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    return S_ISCHR(st.st_mode) && major(st.st_rdev) == 226; /* DRM major */
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev) {
    if (!real_epoll_ctl) real_epoll_ctl = (int(*)(int,int,int,struct epoll_event*))resolve_next("epoll_ctl",(void*)epoll_ctl);
    int r = real_epoll_ctl(epfd, op, fd, ev);
    if (is_compositor() && ev && (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) && fd_is_drm(fd)) {
        g_epoll_fd = epfd; g_drm_epoll_data = ev->data; g_drm_epoll_valid = 1;
        if (g_drm_fd < 0) g_drm_fd = fd;
        /* Add our private wake eventfd to the SAME epoll so synth_arm() can make
         * it readable and wake the (possibly idle) outer loop. */
        if (g_wake_fd < 0) {
            g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (g_wake_fd >= 0) {
                struct epoll_event we; memset(&we, 0, sizeof we);
                we.events = EPOLLIN; we.data.ptr = &g_wake_fd;
                if (real_epoll_ctl(epfd, EPOLL_CTL_ADD, g_wake_fd, &we) != 0) {
                    close(g_wake_fd); g_wake_fd = -1;
                }
            }
        }
        if (g_timer_fd < 0) {
            g_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (g_timer_fd >= 0) {
                struct epoll_event te; memset(&te, 0, sizeof te);
                te.events = EPOLLIN; te.data.ptr = &g_timer_fd;
                if (real_epoll_ctl(epfd, EPOLL_CTL_ADD, g_timer_fd, &te) != 0) {
                    close(g_timer_fd); g_timer_fd = -1;
                }
            }
        }
        if (getenv("LIBDRM_HYBRIS_SAMPLE"))
            fprintf(stderr, "libdrm-hybris: epoll_ctl tracked DRM fd=%d epfd=%d data=0x%llx\n",
                    fd, epfd, (unsigned long long)ev->data.u64);
    }
    return r;
}

static unsigned long g_epoll_inject = 0;
static int epoll_synth(int epfd, struct epoll_event *events, int n, int maxevents) {
    if (n < 0) n = 0;
    if (epfd != g_epoll_fd || !g_drm_epoll_valid) return n;
    /* Drain + hide our private wake eventfd: wl_event_loop would deref its
     * data.ptr as a wl_event_source and crash, so it must never leak out. */
    if (g_wake_fd >= 0 || g_timer_fd >= 0) {
        if (!real_read) real_read = (ssize_t(*)(int,void*,size_t))resolve_next("read",(void*)read);
        for (int i = 0; i < n; ) {
            if (events[i].data.ptr == (void *)&g_wake_fd) {
                uint64_t v; int sv = in_hook; in_hook = 1;
                while (real_read && real_read(g_wake_fd, &v, sizeof v) == (ssize_t)sizeof v) {}
                in_hook = sv;
                events[i] = events[n - 1]; n--;
            } else if (events[i].data.ptr == (void *)&g_timer_fd) {
                uint64_t v; int sv = in_hook; in_hook = 1;
                while (real_read && real_read(g_timer_fd, &v, sizeof v) == (ssize_t)sizeof v) {}
                in_hook = sv;
                events[i] = events[n - 1]; n--;
            } else i++;
        }
    }
    if (g_synth_qn <= 0) return n;
    if (now_ns() < g_synth_q[g_synth_qh].deadline) return n; /* not due yet */
    g_epoll_inject++;
    STAMP("epoll-inject");
    for (int i = 0; i < n; i++)
        if (events[i].data.u64 == g_drm_epoll_data.u64) { events[i].events |= EPOLLIN; return n; }
    if (n < maxevents) {
        events[n].events = EPOLLIN;
        events[n].data   = g_drm_epoll_data;
        return n + 1;
    }
    return n;
}

/* A pending flip must be delivered without blocking. */
static int epoll_cap_timeout(int timeout) {
    if (g_synth_qn <= 0) return timeout;
    int64_t rem = (int64_t)g_synth_q[g_synth_qh].deadline - (int64_t)now_ns();
    if (rem <= 0) return 0;
    int ms = (int)((rem + 999999) / 1000000); /* round up: no busy loop */
    return (timeout >= 0 && timeout < ms) ? timeout : ms;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    if (!real_epoll_wait) real_epoll_wait = (int(*)(int,struct epoll_event*,int,int))resolve_next("epoll_wait",(void*)epoll_wait);
    if (!g_synth_active || in_hook || epfd != g_epoll_fd || !g_drm_epoll_valid)
        return real_epoll_wait(epfd, events, maxevents, timeout);
    int n = real_epoll_wait(epfd, events, maxevents, epoll_cap_timeout(timeout));
    return epoll_synth(epfd, events, n, maxevents);
}
int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *ss) {
    if (!real_epoll_pwait) real_epoll_pwait = (int(*)(int,struct epoll_event*,int,int,const sigset_t*))resolve_next("epoll_pwait",(void*)epoll_pwait);
    if (!g_synth_active || in_hook || epfd != g_epoll_fd || !g_drm_epoll_valid)
        return real_epoll_pwait(epfd, events, maxevents, timeout, ss);
    int n = real_epoll_pwait(epfd, events, maxevents, epoll_cap_timeout(timeout), ss);
    return epoll_synth(epfd, events, n, maxevents);
}
static int (*real_epoll_pwait2)(int,struct epoll_event*,int,const struct timespec*,const sigset_t*) = NULL;
int epoll_pwait2(int epfd, struct epoll_event *events, int maxevents, const struct timespec *to, const sigset_t *ss) {
    if (!real_epoll_pwait2) real_epoll_pwait2 = (int(*)(int,struct epoll_event*,int,const struct timespec*,const sigset_t*))resolve_next("epoll_pwait2",(void*)epoll_pwait2);
    if (!g_synth_active || in_hook || epfd != g_epoll_fd || !g_drm_epoll_valid)
        return real_epoll_pwait2(epfd, events, maxevents, to, ss);
    struct timespec cap; const struct timespec *eff = to;
    if (g_synth_qn > 0) {
        int64_t rem = (int64_t)g_synth_q[g_synth_qh].deadline - (int64_t)now_ns(); if (rem < 0) rem = 0;
        cap.tv_sec = rem / 1000000000; cap.tv_nsec = rem % 1000000000;
        uint64_t capn = (uint64_t)cap.tv_sec*1000000000ull + cap.tv_nsec;
        if (!to || (uint64_t)to->tv_sec*1000000000ull + to->tv_nsec > capn) eff = &cap;
    }
    int n = real_epoll_pwait2(epfd, events, maxevents, eff, ss);
    return epoll_synth(epfd, events, n, maxevents);
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

/*
 * Exported so libhybris (eglplatformcommon) can recover the gralloc handle
 * behind a gbm_hybris dmabuf fd when wlroots imports it as an EGL image --
 * hybris EGL imports gralloc ANativeWindowBuffers, not generic Linux
 * dmabufs, so the dmabuf import has to be bridged to a native-buffer import.
 */
buffer_handle_t
drm_shim_lookup_gralloc(uint32_t fd)
{
    return find_gralloc(fd);
}

/* Present a wlroots-rendered gralloc buffer to HWC2. wlroots has no drmadapter
 * EGL window surface, so drmadapter's present_cb (which mutter's eglSwapBuffers
 * drives) never runs and the faked KMS commit alone wouldn't reach the panel.
 * Hand the committed buffer to libhybris, which presents the matching
 * RemoteWindowBuffer via drmadapter's HWC2 display/layer. Only under
 * HYBRIS_WLROOTS; the mutter path presents itself. */
static int g_wlroots = -1;
/* The drmadapter EGL platform registers its HWC2 present callback here at init.
 * It lives in a ws module dlopen()'d RTLD_LAZY (local scope), so it can't be
 * reached by dlsym from here -- but this shim is globally preloaded, so the
 * registration goes the other way (drmadapter -> us). */
static int (*g_present_fn)(buffer_handle_t) = NULL;
void drm_shim_set_present(int (*fn)(buffer_handle_t)) {
    g_present_fn = fn;
    if (getenv("LIBDRM_HYBRIS_SAMPLE")) fprintf(stderr, "libdrm-hybris: present callback registered fn=%p\n", (void*)fn);
}
/* drmadapter also registers a power callback so we can drive the real HWC2
 * display power off/on when wlroots toggles the CRTC ACTIVE state (DPMS). The
 * faked atomic commit otherwise never touches the panel power: "blanking" just
 * stops phoc presenting (backlight stays on showing the last frame) and wake
 * never powers anything back -- so the screen looks frozen. With this, an
 * output-disable commit powers the panel down and the re-enable commit (driven
 * by phosh on wake input) powers it back up. */
static void (*g_power_fn)(int) = NULL;
void drm_shim_set_power(void (*fn)(int)) {
    g_power_fn = fn;
    if (getenv("LIBDRM_HYBRIS_SAMPLE")) fprintf(stderr, "libdrm-hybris: power callback registered fn=%p\n", (void*)fn);
}
/* wlroots' render-completion fence for the current commit (the plane's
 * IN_FENCE_FD). We fake the KMS commit, so the kernel never waits on it -- we
 * must, or the blit/HWC2 scans out a half-rendered (flickery/black) buffer.
 * Captured in drmModeAtomicAddProperty; consumed (waited + closed) here. */
static int g_committed_fence = -1;
static void present_hwc2(buffer_handle_t h) {
    if (g_wlroots < 0) g_wlroots = getenv("HYBRIS_WLROOTS") ? 1 : 0;
    if (!g_wlroots || !h || !g_present_fn) {
        if (g_committed_fence >= 0) { close(g_committed_fence); g_committed_fence = -1; }
        return;
    }
    /* Block until wlroots' GPU render into this buffer is complete. The sync
     * file becomes readable (POLLIN) when signalled; act as the kernel that
     * consumes the in-fence. */
    if (g_committed_fence >= 0) {
        struct pollfd pfd = { .fd = g_committed_fence, .events = POLLIN };
        int saved = in_hook; in_hook = 1;
        real_poll ? real_poll(&pfd, 1, 1000) : poll(&pfd, 1, 1000);
        in_hook = saved;
        static int logged = 0;
        if (!logged && getenv("LIBDRM_HYBRIS_SAMPLE")) { fprintf(stderr, "libdrm-hybris: waited on IN_FENCE_FD %d\n", g_committed_fence); logged = 1; }
        close(g_committed_fence); g_committed_fence = -1;
    }
    int rc = g_present_fn(h);
    STAMP("present:post-hwc2");
    static int logged2 = 0;
    if (!logged2 && getenv("LIBDRM_HYBRIS_SAMPLE")) { fprintf(stderr, "libdrm-hybris: first present_hwc2(h=%p) rc=%d\n", (void*)h, rc); logged2 = 1; }
    (void)rc;
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

/*
 * NB: the dumb buffer is never actually scanned out (the HWC2 composer owns the
 * CRTC; real pixels reach the panel via the drmadapter EGL path). HOWEVER the
 * per-frame gralloc lock/unlock in copy_to_dumb() is load-bearing -- removing it
 * makes gnome-session stop the shell after ~43s (verified on-device). The lock/
 * unlock is evidently a GPU/cache sync the HWC2 presentation relies on, so keep
 * it. (The dumb FB also gives mutter a real FB id to page-flip to.)
 */
static unsigned long g_commit_n = 0;
/* Diagnostic: scan every registered gralloc buffer for non-black content, to
 * tell whether the rendered frame landed in a buffer we simply didn't pick. */
static void sample_all_buffers(void) {
    if (!frame_w || !frame_h) return;
    for (int i = 0; i < gmap_n; i++) {
        buffer_handle_t h = gmap[i].gralloc;
        if (!h) continue;
        void *s = NULL;
        if (hybris_gralloc_lock(h, 0x3, 0, 0, frame_w, frame_h, &s) || !s) continue;
        unsigned long nz = 0;
        for (uint32_t y = 0; y < frame_h; y += 64)
            for (uint32_t x = 0; x < frame_w; x += 64) {
                uint8_t *p = (uint8_t*)s + (size_t)y*frame_w*4 + x*4;
                if (p[0]|p[1]|p[2]) nz++;
            }
        hybris_gralloc_unlock(h);
        if (nz) fprintf(stderr, "libdrm-hybris:   buffer[%d] gralloc=%p NONBLACK samples=%lu\n", i, (void*)h, nz);
    }
}

static void
copy_to_dumb(buffer_handle_t h)
{
    if (!dumb_map || !h || !frame_w || !frame_h)
        return;
    void *src = NULL;
    if (hybris_gralloc_lock(h, GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN, 0, 0, frame_w, frame_h, &src) || !src)
        return;
    uint8_t *d = dumb_map, *s = src;
    /* The full-frame read here is load-bearing: touching every pixel forces the
     * Mali GPU to resolve its render into the buffer before it's presented. */
    for (uint32_t y = 0; y < frame_h; y++) {
        memcpy(d + y * dumb_pitch, s + y * dumb_pitch, frame_w * 4);
    }

    /* Diagnostic: is the committed buffer actually non-black? Sample a grid. */
    if (getenv("LIBDRM_HYBRIS_SAMPLE")) {
        unsigned long nz = 0; uint32_t cx = frame_w/2, cy = frame_h/2;
        for (uint32_t y = 0; y < frame_h; y += 64)
            for (uint32_t x = 0; x < frame_w; x += 64) {
                uint8_t *p = s + y*dumb_pitch + x*4;
                if (p[0]|p[1]|p[2]) nz++;
            }
        uint8_t *c = s + cy*dumb_pitch + cx*4;
        fprintf(stderr, "libdrm-hybris: commit#%lu h=%p nonblack_samples=%lu center=%02x%02x%02x\n",
                g_commit_n, (void*)h, nz, c[0], c[1], c[2]);
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

    /*
     * wlroots imports the gbm_bo's PRIME fd for scan-out, which needs DRM
     * master -- the HWC2 composer owns it, so the real import returns EACCES.
     * Use the prime fd itself as the GEM handle: gbm_hybris registered the
     * prime_fd -> gralloc mapping (gmap) and find_gralloc()/find_by_fb() key on
     * the prime fd, so AddFB2/commit can still recover the buffer.
     */
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
    present_hwc2(h);
    int r = real ? real(fd, crtc_id, dumb_fb_id ? dumb_fb_id : fb_id, flags, ud) : 0;
    if (r == -EACCES) {
        if (flags & DRM_MODE_PAGE_FLIP_EVENT)
            synth_arm(crtc_id, (uint64_t) (uintptr_t) ud);
        r = 0;
    }
    return r;
}

/* The synthetic flip event must carry the real CRTC id: wlroots' version-3
 * page_flip_handler2 matches the event to a connector by crtc_id
 * (drm_page_flip_pop), and drops it otherwise -> the repaint loop never
 * advances. Discover it from the connected connector's encoder (the HWC2
 * composer already has the panel lit, so the kernel reports a live CRTC). */
static uint32_t g_crtc_id = 0;
static uint32_t discover_crtc(int fd) {
    if (g_crtc_id) return g_crtc_id;
    int saved = in_hook; in_hook = 1;
    drmModeRes *res = drmModeGetResources(fd);
    if (res) {
        for (int i = 0; i < res->count_connectors && !g_crtc_id; i++) {
            drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
            if (c && c->connection == DRM_MODE_CONNECTED && c->encoder_id) {
                drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
                if (e && e->crtc_id) g_crtc_id = e->crtc_id;
                if (e) drmModeFreeEncoder(e);
            }
            if (c) drmModeFreeConnector(c);
        }
        if (!g_crtc_id && res->count_crtcs > 0) g_crtc_id = res->crtcs[0];
        drmModeFreeResources(res);
    }
    in_hook = saved;
    if (getenv("LIBDRM_HYBRIS_SAMPLE"))
        fprintf(stderr, "libdrm-hybris: discovered CRTC id=%u\n", g_crtc_id);
    return g_crtc_id;
}

/* The atomic request can reference several framebuffers (one per swapchain
 * buffer queued over time); only the FB_ID set on the primary plane in *this*
 * commit is the frame being scanned out. Guessing the most-recent AddFB2 picks
 * the wrong (un-rendered) buffer. Capture the real FB_ID by intercepting the
 * property the compositor sets on the plane. */
static uint32_t g_fbid_prop = 0, g_crtcid_prop = 0, g_infence_prop = 0;
static int g_infence_learned = 0;
static uint32_t g_committed_fb = 0, g_committed_crtc = 0;
/* CRTC ACTIVE property -> drives DPMS (panel power) via g_power_fn. */
static uint32_t g_active_prop = 0;
static int g_pending_active = -1;   /* ACTIVE value seen in the current commit, -1 if none */
static int g_output_on = 1;          /* current panel power state (init: on) */

/* phoc calls this directly (dlsym) from its output-power-management handler to
 * power the panel off/on WITHOUT disabling the wlroots output. Disabling the
 * output tears down the mode and triggers a modeset on re-enable, after which
 * wlroots stops scheduling frames for client damage on this faked-KMS backend
 * (the screen freezes). Keeping the output enabled and only toggling HWC2 panel
 * power avoids the modeset entirely. */
void drm_shim_panel_power(int on) {
    on = on ? 1 : 0;
    if (on == g_output_on) return;
    g_output_on = on;
    if (g_power_fn) g_power_fn(on);
    fprintf(stderr, "libdrm-hybris: drm_shim_panel_power -> %s\n", on ? "ON" : "OFF");
}
/* phoc queries this on input activity: if the panel was blanked, phoc forces it
 * back on (and repaints), so ANY input wakes the screen even when phosh's
 * lockscreen/idle manager does not request the wake itself. */
int drm_shim_panel_is_on(void) { return g_output_on; }
int drmModeAtomicAddProperty(drmModeAtomicReqPtr req, uint32_t obj, uint32_t prop, uint64_t val) {
    typedef int (*fn_t)(drmModeAtomicReqPtr,uint32_t,uint32_t,uint64_t);
    fn_t real = (fn_t)resolve_next("drmModeAtomicAddProperty",(void*)drmModeAtomicAddProperty);
    if (is_compositor() && g_drm_fd >= 0 && !in_hook) {
        if (!g_fbid_prop || !g_crtcid_prop || !g_infence_learned || !g_active_prop) { /* learn prop ids (device-global) */
            in_hook = 1;
            drmModePropertyPtr p = drmModeGetProperty(g_drm_fd, prop);
            if (p) {
                if (strcmp(p->name, "FB_ID") == 0) g_fbid_prop = prop;
                else if (strcmp(p->name, "CRTC_ID") == 0) g_crtcid_prop = prop;
                else if (strcmp(p->name, "IN_FENCE_FD") == 0) { g_infence_prop = prop; g_infence_learned = 1; }
                else if (strcmp(p->name, "ACTIVE") == 0) g_active_prop = prop;
                drmModeFreeProperty(p);
            }
            in_hook = 0;
        }
        if (prop == g_fbid_prop && val) g_committed_fb = (uint32_t)val;
        /* CRTC ACTIVE=0 disables the output (DPMS off), =1 re-enables it. */
        if (g_active_prop && prop == g_active_prop) g_pending_active = (int)val;
        /* The CRTC the compositor actually drives this connector with -- the
         * synthetic flip event must carry exactly this id or wlroots'
         * handle_page_flip drops it (no buffer release, no next frame). */
        if (prop == g_crtcid_prop && val) g_committed_crtc = (uint32_t)val;
        /* wlroots' render-completion fence for this frame; present_hwc2 waits on
         * it (then closes it) before the buffer is sampled/scanned out. */
        if (g_infence_prop && prop == g_infence_prop && (int64_t)val >= 0)
            g_committed_fence = (int)val;
    }
    return real ? real(req, obj, prop, val) : -ENOSYS;
}

int
drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *ud)
{
    if (!is_compositor()) {
        typedef int (*fn_t) (int, drmModeAtomicReqPtr, uint32_t, void *);
        fn_t real = (fn_t) resolve_next("drmModeAtomicCommit", (void *) drmModeAtomicCommit);
        return real ? real(fd,req,flags,ud) : -ENOSYS;
    }

    /* Atomic check (TEST_ONLY): just report the config valid, present nothing. */
    if (flags & DRM_MODE_ATOMIC_TEST_ONLY) return 0;
    g_drm_fd = fd; synth_note_flip();
    g_commit_n++;
    STAMP("commit");
    /* Present ONLY commits that carry a new framebuffer (FB_ID captured from the
     * plane property). Commits WITHOUT one (cursor/gamma/empty commits -- frequent
     * during interaction) must NOT present: the old "fall back to the most recent
     * fmap buffer" guess re-presented a STALE frame between real ones, which is
     * exactly the rapid flicker on any motion (static content = no interleaved
     * empty commits = no flicker). */
    buffer_handle_t h = g_committed_fb ? find_by_fb(g_committed_fb) : NULL;
    if (!h) for (int i=fmap_n-1; i>=0; i--) { h=find_gralloc(fmap[i].gem); if (h) break; }
    if (getenv("LIBDRM_HYBRIS_SAMPLE"))
        fprintf(stderr, "libdrm-hybris: atomicCommit #%lu flags=0x%x fb=%u h=%p arm=%d\n",
                g_commit_n, flags, g_committed_fb, (void*)h, (flags & DRM_MODE_PAGE_FLIP_EVENT)?1:0);
    /* DPMS is driven by phoc's output-power handler via drm_shim_panel_power()
     * (which keeps the wlr output enabled -> no modeset -> no freeze). We do NOT
     * also toggle power from the CRTC ACTIVE property here: with the output kept
     * enabled, ACTIVE stays 1 every commit and would immediately undo a
     * panel-off, oscillating the backlight. (g_pending_active is left tracked
     * but unused as a harmless fallback hook.) */
    g_pending_active = -1;
    if (h) {
        /* copy_to_dumb (per-frame CPU read of the render buffer) was needed on
         * the mutter/gnome path; for phoc it is skippable -- and its WRITE-usage
         * gralloc lock forces an AFBC writeback on unlock that RACES the blit's
         * GPU sampling (intermittent stale/black frames = motion flicker; whether
         * a session's buffers are AFBC or linear decides blank/flicker/clean).
         * Keep it only if LIBDRM_HYBRIS_DUMBCOPY=1. */
        static int dc = -1;
        if (dc < 0) {
            const char *e = getenv("LIBDRM_HYBRIS_DUMBCOPY");
            /* Default ON for the gnome/mutter session: the per-frame CPU read
             * is load-bearing there (without it gnome-session stops the shell
             * after ~43s). The wlroots/phoc session presents via its own CPU
             * copy and must NOT also do this (the write-usage lock's AFBC
             * writeback races the presenter). */
            if (e) dc = (*e == '1') ? 1 : 0;
            else   dc = is_gnome() ? 1 : 0;
        }
        if (dc) copy_to_dumb(h);
        present_hwc2(h);
    }
    else if (g_committed_fb) {
        /* A buffer WAS committed but didn't resolve to a gralloc -- real problem. */
        static unsigned long z = 0;
        if ((z++ % 300) == 0)
            fprintf(stderr, "libdrm-hybris: PRESENT SKIPPED (unresolved fb=%u) #%lu commit=%lu fmap_n=%d gmap_n=%d\n",
                    g_committed_fb, z, g_commit_n, fmap_n, gmap_n);
    }
    g_committed_fb = 0;
    /* wlroots commits non-blocking and waits for a page-flip completion event
     * before scheduling the next frame. The HWC2 composer owns the CRTC so no
     * real event arrives -- synthesize one (carrying wlroots' user_data) or the
     * repaint loop stalls after a single frame. */
    if (flags & DRM_MODE_PAGE_FLIP_EVENT) {
        uint32_t crtc = g_committed_crtc ? g_committed_crtc : discover_crtc(fd);
        if (getenv("LIBDRM_HYBRIS_SAMPLE") && g_commit_n <= 3)
            fprintf(stderr, "libdrm-hybris: synth crtc=%u (committed=%u discovered=%u)\n",
                    crtc, g_committed_crtc, discover_crtc(fd));
        synth_arm(crtc, (uint64_t)(uintptr_t)ud);
    }

    if (getenv("LIBDRM_HYBRIS_SAMPLE")) sample_all_buffers();

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

    if (_IOC_TYPE(request) != DRM_IOCTL_BASE)
        return real_ioctl(fd, request, arg);

    /*
     * Only the compositor's DRM ioctls drive the fake KMS framebuffer.
     * Client processes (camera etc) must reach the real DRM driver intact.
     */
    if (!is_compositor())
        return real_ioctl(fd, request, arg);

    if (in_hook)
        return real_ioctl(fd, request, arg);

    uint32_t nr = _IOC_NR(request);
    in_hook = 1;

    int ret;

    if (nr == _IOC_NR(DRM_IOCTL_MODE_ADDFB2)) {
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
    } else if (nr == _IOC_NR(DRM_IOCTL_MODE_RMFB)) {
        ret = real_ioctl(fd, request, arg);
    } else if (nr == _IOC_NR(DRM_IOCTL_MODE_SETCRTC)) {
        if (!dumb_map)
            init_dumb(fd);

        real_ioctl(fd, request, arg);
        ret = 0;
    } else if (nr == _IOC_NR(DRM_IOCTL_MODE_PAGE_FLIP) ||
               nr == _IOC_NR(DRM_IOCTL_MODE_GETPLANE)) {
        struct drm_mode_crtc_page_flip *flip = arg;

        g_drm_fd = fd;
        synth_note_flip();

        buffer_handle_t h = find_by_fb(flip->fb_id);
        copy_to_dumb(h);
        present_hwc2(h);

        if (dumb_fb_id)
            flip->fb_id = dumb_fb_id;

        ret = real_ioctl(fd, request, arg);

        if (ret != 0 && errno == EACCES) {
            if (flip->flags & DRM_MODE_PAGE_FLIP_EVENT)
                synth_arm(flip->crtc_id, flip->user_data);

            ret = 0;
        }
    } else if (nr == _IOC_NR(DRM_IOCTL_MODE_ATOMIC)) {
        for (int i = fmap_n - 1; i >= 0; i--) {
            buffer_handle_t h = find_gralloc(fmap[i].gem);

            if (h) {
                copy_to_dumb(h);
                present_hwc2(h);
                break;
            }
        }

        ret = 0;
    } else if (nr == _IOC_NR(DRM_IOCTL_AUTH_MAGIC)) {
        ret = 0;
    } else {
        ret = real_ioctl(fd, request, arg);
    }

    in_hook = 0;

    return ret;
}
