/*
 * PinchKey.cpp — C++17, libevdev + uinput
 *
 * Phase 1 : Detect fingers & positions on trackpad  (TouchpadState / processTouchpadEvent)
 * Phase 2 : Compute distances between adjacent fingers and print them   ← current focus
 * Phase 3 : Simulate distance changes via ALT+'+' / ALT+'-' / ALT+Wheel (ShortcutEngine)
 * Phase 4 : Inject synthetic MT Protocol B pinch gestures via uinput    (emitPinch)
 *
 * With 2 fingers : distance(left, right)
 * With 3 fingers : distance(left, mid)  +  distance(mid, right)
 * … and so on for N fingers, sorted by X coordinate.
 *
 * Build : make
 * Run   : sudo ./PinchKey
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <libevdev/libevdev.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

/* ═══════════════════════════════ Configuration ══════════════════════════ */

constexpr const char* INPUT_DIR = "/dev/input";
constexpr int         MAX_SLOTS = 10;

/* Pinch gesture tuning */
constexpr int    PINCH_FRAMES      = 20;    // position-update frames per press
constexpr int    PINCH_FRAME_MS    = 16;    // ms between frames (≈60 fps)
constexpr double PINCH_START_FRAC  = 0.15;  // starting finger separation as fraction of axis width
constexpr double PINCH_TRAVEL_FRAC = 0.20;  // total travel per finger as fraction of axis width

/* ═══════════════════════════════ Globals ════════════════════════════════ */

static std::atomic<bool> g_running{true};
static std::mutex        g_print_mtx;

/* Virtual touchpad uinput fd — written only by emitPinch(), guarded by g_pinch_mtx */
static int               g_pinch_ufd = -1;
static std::mutex        g_pinch_mtx;

/* Axis bounds populated from the real touchpad at startup */
static int g_ax_min = 0, g_ax_max = 1232;
static int g_ay_min = 0, g_ay_max = 928;
static int g_ax_res = 32, g_ay_res = 32;

/* ═══════════════════════════════ Logging ════════════════════════════════ */

template<typename... Args>
static void log(Args&&... args)
{
    std::lock_guard<std::mutex> lk(g_print_mtx);
    (std::cout << ... << std::forward<Args>(args)) << '\n';
    std::cout.flush();
}

static long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
}

/* ═══════════════════════════════ uinput helpers ═════════════════════════ */

static void uwrite(int ufd, __u16 type, __u16 code, __s32 value)
{
    if (ufd < 0) return;
    struct input_event ev{};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (::write(ufd, &ev, sizeof(ev)) < 0) { /* non-fatal */ }
}

static void usync(int ufd) { uwrite(ufd, EV_SYN, SYN_REPORT, 0); }

static void passEvent(int ufd, const input_event& ev)
{
    if (ufd >= 0 && ::write(ufd, &ev, sizeof(ev)) < 0) { /* non-fatal */ }
}

static void passSync(int ufd)
{
    input_event syn{}; syn.type = EV_SYN; syn.code = SYN_REPORT;
    passEvent(ufd, syn);
}

/* ═══════════════════════════════ Virtual touchpad ═══════════════════════ */

static int createVirtualTouchpad()
{
    int ufd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) {
        log("[ERROR] Cannot open /dev/uinput — pinch injection disabled.");
        log("[ERROR] Try: sudo ./PinchKey");
        return -1;
    }

    ::ioctl(ufd, UI_SET_EVBIT, EV_SYN);
    ::ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ::ioctl(ufd, UI_SET_EVBIT, EV_ABS);

    ::ioctl(ufd, UI_SET_KEYBIT, BTN_TOUCH);
    ::ioctl(ufd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    ::ioctl(ufd, UI_SET_KEYBIT, BTN_TOOL_DOUBLETAP);

    auto setAxis = [&](int code, int min, int max, int res, int fuzz = 0) {
        ::ioctl(ufd, UI_SET_ABSBIT, code);
        struct uinput_abs_setup as{};
        as.code               = static_cast<__u16>(code);
        as.absinfo.minimum    = min;
        as.absinfo.maximum    = max;
        as.absinfo.resolution = res;
        as.absinfo.fuzz       = fuzz;
        ::ioctl(ufd, UI_ABS_SETUP, &as);
    };

    setAxis(ABS_X, g_ax_min, g_ax_max, g_ax_res);
    setAxis(ABS_Y, g_ay_min, g_ay_max, g_ay_res);

    setAxis(ABS_MT_SLOT,        0,       1,      0);
    setAxis(ABS_MT_TRACKING_ID, 0,  65535,       0);
    setAxis(ABS_MT_POSITION_X,  g_ax_min, g_ax_max, g_ax_res);
    setAxis(ABS_MT_POSITION_Y,  g_ay_min, g_ay_max, g_ay_res);
    setAxis(ABS_MT_PRESSURE,    0,   255,         0);
    setAxis(ABS_MT_TOUCH_MAJOR, 0,   255,         0);

    ::ioctl(ufd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    ::ioctl(ufd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);

    struct uinput_setup us{};
    us.id = { BUS_VIRTUAL, 0x1234, 0x5678, 1 };
    std::snprintf(us.name, UINPUT_MAX_NAME_SIZE, "PinchKey Virtual Touchpad");

    if (::ioctl(ufd, UI_DEV_SETUP, &us) < 0 || ::ioctl(ufd, UI_DEV_CREATE) < 0) {
        log("[ERROR] UI_DEV_CREATE failed for virtual touchpad");
        ::close(ufd);
        return -1;
    }

    std::this_thread::sleep_for(150ms);
    log("[PINCH] Virtual touchpad created — axis X:[", g_ax_min, ",", g_ax_max,
        "] Y:[", g_ay_min, ",", g_ay_max, "]");
    return ufd;
}

/* ═══════════════════════════════ Pinch emitter ══════════════════════════ */

static void emitPinch(int direction)
{
    std::lock_guard<std::mutex> lk(g_pinch_mtx);
    const int ufd = g_pinch_ufd;
    if (ufd < 0) { log("[PINCH] Virtual touchpad not available"); return; }

    const char* label = (direction > 0) ? "OUT" : "IN";
    log("[PINCH] ", label, " — injecting gesture");

    const int axRange   = g_ax_max - g_ax_min;
    const int cx        = g_ax_min + axRange / 2;
    const int cy        = g_ay_min + (g_ay_max - g_ay_min) / 2;
    const int startHalf = static_cast<int>(axRange * PINCH_START_FRAC);
    const int stepPx    = static_cast<int>(axRange * PINCH_TRAVEL_FRAC) / PINCH_FRAMES;
    const int sign      = (direction > 0) ? 1 : -1;

    const int x0_start = cx - startHalf;
    const int x1_start = cx + startHalf;

    /* FRAME 0: fingers down */
    uwrite(ufd, EV_ABS, ABS_MT_SLOT,        0);
    uwrite(ufd, EV_ABS, ABS_MT_TRACKING_ID, 1);
    uwrite(ufd, EV_ABS, ABS_MT_POSITION_X,  x0_start);
    uwrite(ufd, EV_ABS, ABS_MT_POSITION_Y,  cy);
    uwrite(ufd, EV_ABS, ABS_MT_PRESSURE,    50);
    uwrite(ufd, EV_ABS, ABS_MT_TOUCH_MAJOR, 50);
    uwrite(ufd, EV_ABS, ABS_X,              x0_start);
    uwrite(ufd, EV_ABS, ABS_Y,              cy);

    uwrite(ufd, EV_ABS, ABS_MT_SLOT,        1);
    uwrite(ufd, EV_ABS, ABS_MT_TRACKING_ID, 2);
    uwrite(ufd, EV_ABS, ABS_MT_POSITION_X,  x1_start);
    uwrite(ufd, EV_ABS, ABS_MT_POSITION_Y,  cy);
    uwrite(ufd, EV_ABS, ABS_MT_PRESSURE,    50);
    uwrite(ufd, EV_ABS, ABS_MT_TOUCH_MAJOR, 50);

    uwrite(ufd, EV_KEY, BTN_TOUCH,          1);
    uwrite(ufd, EV_KEY, BTN_TOOL_DOUBLETAP, 1);
    usync(ufd);
    std::this_thread::sleep_for(std::chrono::milliseconds(PINCH_FRAME_MS));

    /* FRAMES 1..N: movement */
    for (int f = 1; f <= PINCH_FRAMES; ++f) {
        const int offset = sign * stepPx * f;
        const int x0 = std::clamp(x0_start - offset, g_ax_min, g_ax_max);
        const int x1 = std::clamp(x1_start + offset, g_ax_min, g_ax_max);

        uwrite(ufd, EV_ABS, ABS_MT_SLOT,       0);
        uwrite(ufd, EV_ABS, ABS_MT_POSITION_X, x0);
        uwrite(ufd, EV_ABS, ABS_MT_POSITION_Y, cy);
        uwrite(ufd, EV_ABS, ABS_X,             x0);
        uwrite(ufd, EV_ABS, ABS_Y,             cy);

        uwrite(ufd, EV_ABS, ABS_MT_SLOT,       1);
        uwrite(ufd, EV_ABS, ABS_MT_POSITION_X, x1);
        uwrite(ufd, EV_ABS, ABS_MT_POSITION_Y, cy);

        usync(ufd);
        std::this_thread::sleep_for(std::chrono::milliseconds(PINCH_FRAME_MS));
    }

    /* LAST FRAME: fingers up */
    uwrite(ufd, EV_ABS, ABS_MT_SLOT,        0);
    uwrite(ufd, EV_ABS, ABS_MT_TRACKING_ID, -1);

    uwrite(ufd, EV_ABS, ABS_MT_SLOT,        1);
    uwrite(ufd, EV_ABS, ABS_MT_TRACKING_ID, -1);

    uwrite(ufd, EV_KEY, BTN_TOUCH,          0);
    uwrite(ufd, EV_KEY, BTN_TOOL_DOUBLETAP, 0);
    usync(ufd);

    log("[PINCH] ", label, " — done");
}

/* ═══════════════════════════════ uinput mirror (keyboard/mouse) ═════════ */

static int createMirrorDevice(libevdev* src)
{
    int ufd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) { log("[WARN] Cannot open /dev/uinput for mirror"); return -1; }

    auto enable = [&](int req, int bit) { ::ioctl(ufd, req, bit); };
    enable(UI_SET_EVBIT, EV_SYN);

    if (libevdev_has_event_type(src, EV_KEY)) {
        enable(UI_SET_EVBIT, EV_KEY);
        for (int c = 0; c <= KEY_MAX; ++c)
            if (libevdev_has_event_code(src, EV_KEY, c)) enable(UI_SET_KEYBIT, c);
    }
    if (libevdev_has_event_type(src, EV_REL)) {
        enable(UI_SET_EVBIT, EV_REL);
        for (int c = 0; c <= REL_MAX; ++c)
            if (libevdev_has_event_code(src, EV_REL, c)) enable(UI_SET_RELBIT, c);
    }
    if (libevdev_has_event_type(src, EV_ABS)) {
        enable(UI_SET_EVBIT, EV_ABS);
        for (int c = 0; c <= ABS_MAX; ++c) {
            if (libevdev_has_event_code(src, EV_ABS, c)) {
                enable(UI_SET_ABSBIT, c);
                const struct input_absinfo* ai = libevdev_get_abs_info(src, c);
                if (ai) {
                    struct uinput_abs_setup as{};
                    as.code = static_cast<__u16>(c); as.absinfo = *ai;
                    ::ioctl(ufd, UI_ABS_SETUP, &as);
                }
            }
        }
    }
    if (libevdev_has_event_type(src, EV_MSC)) {
        enable(UI_SET_EVBIT, EV_MSC);
        for (int c = 0; c <= MSC_MAX; ++c)
            if (libevdev_has_event_code(src, EV_MSC, c)) enable(UI_SET_MSCBIT, c);
    }

    struct uinput_setup us{};
    us.id = { BUS_VIRTUAL, 0xBEEF, 0xBEEF, 1 };
    const char* n = libevdev_get_name(src);
    std::snprintf(us.name, UINPUT_MAX_NAME_SIZE, "%.48s [pk]", n ? n : "device");

    if (::ioctl(ufd, UI_DEV_SETUP, &us) < 0 || ::ioctl(ufd, UI_DEV_CREATE) < 0) {
        ::close(ufd); return -1;
    }
    return ufd;
}

/* ═══════════════════════════════ Shortcut engine ════════════════════════ */

struct ShortcutEngine {
    bool            alt_held     = false;
    bool            alt_pending  = false;
    input_event     alt_down_ev  = {};

    bool process(const input_event& ev, int mirror_ufd)
    {
        if (ev.type == EV_KEY &&
            (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT))
        {
            if (ev.value == 1) {
                alt_held    = true;
                alt_pending = true;
                alt_down_ev = ev;
                return false;
            }
            if (ev.value == 0) {
                alt_held = false;
                if (alt_pending) {
                    flushAlt(mirror_ufd);
                    passEvent(mirror_ufd, ev);
                    alt_pending = false;
                } else {
                    passEvent(mirror_ufd, ev);
                }
                return false;
            }
            if (!alt_pending) passEvent(mirror_ufd, ev);
            return false;
        }

        if (alt_held && ev.type == EV_KEY && ev.value == 1) {
            if (ev.code == KEY_EQUAL || ev.code == KEY_KPPLUS) {
                alt_pending = false;
                std::thread([]{ emitPinch(+1); }).detach();
                return true;
            }
            if (ev.code == KEY_MINUS || ev.code == KEY_KPMINUS) {
                alt_pending = false;
                std::thread([]{ emitPinch(-1); }).detach();
                return true;
            }
        }

        if (alt_pending) {
            flushAlt(mirror_ufd);
            alt_pending = false;
        }

        if (alt_held && ev.type == EV_REL &&
            (ev.code == REL_WHEEL || ev.code == REL_WHEEL_HI_RES)) {
            /* ALT+Wheel Up → pinch-out,  ALT+Wheel Down → pinch-in */
            if (ev.code == REL_WHEEL) {
                std::thread([v = ev.value]{ emitPinch(v > 0 ? +1 : -1); }).detach();
            }
            return true;
        }

        passEvent(mirror_ufd, ev);
        return false;
    }

private:
    void flushAlt(int mirror_ufd) {
        passEvent(mirror_ufd, alt_down_ev);
    }
};

/* ═══════════════════════════════ MT slot state ══════════════════════════ */

struct TouchSlot {
    bool active      = false;
    int  x           = 0, y = 0;
    int  tracking_id = -1;
};

struct TouchpadState {
    TouchSlot slots[MAX_SLOTS];
    int       current_slot = 0;
    int activeCount() const {
        int n = 0; for (const auto& s : slots) n += s.active; return n;
    }
};

/* ─────────────────────────────────────────────────────────────────────────
 * Phase 2 — distance computation
 *
 * Collects all currently active finger positions, sorts them left-to-right
 * by X, then reports the Euclidean distance between each adjacent pair:
 *
 *   2 fingers: d(0,1)
 *   3 fingers: d(0,1)  d(1,2)
 *   N fingers: d(0,1)  d(1,2)  …  d(N-2,N-1)
 *
 * A struct is used so the caller can also inspect the values if needed.
 * ─────────────────────────────────────────────────────────────────────────*/

struct FingerDist {
    int   from_slot;   // slot index of the left finger
    int   to_slot;     // slot index of the right finger
    float dist;        // Euclidean distance in raw trackpad units
};

static std::vector<FingerDist> computeDistances(const TouchpadState& st)
{
    /* Collect active fingers */
    struct FP { int slot, x, y; };
    std::vector<FP> fingers;
    fingers.reserve(MAX_SLOTS);
    for (int i = 0; i < MAX_SLOTS; ++i)
        if (st.slots[i].active)
            fingers.push_back({i, st.slots[i].x, st.slots[i].y});

    /* Sort left-to-right by X coordinate */
    std::sort(fingers.begin(), fingers.end(),
              [](const FP& a, const FP& b){ return a.x < b.x; });

    /* Compute adjacent distances */
    std::vector<FingerDist> result;
    for (size_t i = 1; i < fingers.size(); ++i) {
        const auto& a = fingers[i-1];
        const auto& b = fingers[i];
        float dx = static_cast<float>(b.x - a.x);
        float dy = static_cast<float>(b.y - a.y);
        result.push_back({ a.slot, b.slot, std::sqrt(dx*dx + dy*dy) });
    }
    return result;
}

/* ─────────────────────────────────────────────────────────────────────────
 * processTouchpadEvent
 *
 * Handles raw evdev events for the touchpad.  On every SYN_REPORT it:
 *   1. Counts active fingers.
 *   2. Computes inter-finger distances (Phase 2).
 *   3. Logs the distances in a human-readable line.
 *
 * Position values are intentionally NOT printed in the SYNC line anymore;
 * they are only used internally to compute distances.
 * ─────────────────────────────────────────────────────────────────────────*/
static void processTouchpadEvent(const input_event& ev,
                                 TouchpadState& st, const std::string& tag)
{
    /* ── Slot selector ─────────────────────────────────────────────── */
    if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT) {
        if (ev.value >= 0 && ev.value < MAX_SLOTS) st.current_slot = ev.value;
        return;
    }

    const int  slot = st.current_slot;
    TouchSlot& ts   = st.slots[slot];

    /* ── Contact lifecycle ─────────────────────────────────────────── */
    if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
        if (ev.value == -1) {
            if (ts.active)
                log("[", nowMs(), "ms][", tag, "] [SLOT ", slot, "] UP"
                    "  active=", st.activeCount() - 1);
            ts = TouchSlot{};
        } else {
            ts.active = true; ts.tracking_id = ev.value;
            log("[", nowMs(), "ms][", tag, "] [SLOT ", slot, "] DOWN"
                " tid=", ev.value, "  active=", st.activeCount());
        }
        return;
    }

    /* ── Position updates (kept internally, not logged per-event) ──── */
    if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) { ts.x = ev.value; return; }
    if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) { ts.y = ev.value; return; }

    /* ── SYN_REPORT — emit distance snapshot ──────────────────────── */
    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        const int active = st.activeCount();
        if (active < 2) return;   // need at least 2 fingers for a distance

        auto dists = computeDistances(st);

        /* Build log string: "dist[0→1]=342.1  dist[1→2]=289.6" */
        char buf[256]{}; int off = 0;
        for (size_t i = 0; i < dists.size() && off < (int)sizeof(buf)-40; ++i) {
            off += std::snprintf(buf + off, sizeof(buf) - off,
                                 "  dist[%d→%d]=%.1f",
                                 dists[i].from_slot, dists[i].to_slot, dists[i].dist);
        }

        log("[", nowMs(), "ms][", tag, "] SYNC  active=", active, buf);
    }
}

/* ═══════════════════════════════ RAII evdev wrapper ═════════════════════ */

class EvdevDevice {
public:
    explicit EvdevDevice(const std::string& path) : m_path(path) {
        m_fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (m_fd < 0) return;
        if (libevdev_new_from_fd(m_fd, &m_dev) < 0) { ::close(m_fd); m_fd = -1; return; }
        if (::ioctl(m_fd, EVIOCGRAB, 1) < 0)
            log("[WARN] EVIOCGRAB failed on ", path);
    }
    ~EvdevDevice() {
        if (m_fd >= 0) ::ioctl(m_fd, EVIOCGRAB, 0);
        if (m_dev)     libevdev_free(m_dev);
        if (m_fd >= 0) ::close(m_fd);
    }
    EvdevDevice(const EvdevDevice&)            = delete;
    EvdevDevice& operator=(const EvdevDevice&) = delete;

    bool        valid() const { return m_dev != nullptr; }
    const char* name()  const { return libevdev_get_name(m_dev); }
    libevdev*   raw()   const { return m_dev; }

private:
    std::string m_path;
    int         m_fd  = -1;
    libevdev*   m_dev = nullptr;
};

/* ═══════════════════════════════ Device classification ══════════════════ */

enum class DeviceKind { Ignore, Touchpad, KeyboardMouse };

static DeviceKind classify(libevdev* dev)
{
    if (!dev) return DeviceKind::Ignore;
    const char* raw_name = libevdev_get_name(dev);
    std::string lower(raw_name ? raw_name : "");
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("pinchkey") != std::string::npos) return DeviceKind::Ignore;

    bool has_mt = libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X) &&
                  libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_Y);
    if (lower.find("touchpad") != std::string::npos ||
        lower.find("trackpad") != std::string::npos || has_mt)
        return DeviceKind::Touchpad;

    if (libevdev_has_event_code(dev, EV_KEY, KEY_A)             ||
        libevdev_has_event_code(dev, EV_REL, REL_WHEEL)         ||
        libevdev_has_event_code(dev, EV_REL, REL_WHEEL_HI_RES)  ||
        libevdev_has_event_code(dev, EV_KEY, BTN_LEFT))
        return DeviceKind::KeyboardMouse;

    return DeviceKind::Ignore;
}

/* ═══════════════════════════════ Per-device threads ═════════════════════ */

static void touchpadThread(std::string path)
{
    EvdevDevice dev(path);
    if (!dev.valid() || classify(dev.raw()) != DeviceKind::Touchpad) return;

    const struct input_absinfo* ax = libevdev_get_abs_info(dev.raw(), ABS_MT_POSITION_X);
    const struct input_absinfo* ay = libevdev_get_abs_info(dev.raw(), ABS_MT_POSITION_Y);
    if (ax) { g_ax_min = ax->minimum; g_ax_max = ax->maximum; g_ax_res = ax->resolution ? ax->resolution : 32; }
    if (ay) { g_ay_min = ay->minimum; g_ay_max = ay->maximum; g_ay_res = ay->resolution ? ay->resolution : 32; }
    log("[TOUCHPAD] ", path, " — ", dev.name());
    log("[TOUCHPAD] Axis X:[", g_ax_min, ",", g_ax_max,
        "]  Y:[", g_ay_min, ",", g_ay_max, "]");

    TouchpadState state{};
    input_event   ev{};
    std::string   tag = fs::path(path).filename().string();

    while (g_running.load(std::memory_order_relaxed)) {
        int rc = libevdev_next_event(dev.raw(),
                     LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            while (libevdev_next_event(dev.raw(), LIBEVDEV_READ_FLAG_SYNC, &ev)
                   == LIBEVDEV_READ_STATUS_SYNC)
                processTouchpadEvent(ev, state, tag);
            continue;
        }
        if (rc == -EAGAIN) { std::this_thread::sleep_for(1ms); continue; }
        if (rc < 0) break;
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
            processTouchpadEvent(ev, state, tag);
    }
    log("[TOUCHPAD] Detached: ", path);
}

static void keyboardMouseThread(std::string path)
{
    EvdevDevice dev(path);
    if (!dev.valid() || classify(dev.raw()) != DeviceKind::KeyboardMouse) return;

    int           ufd    = createMirrorDevice(dev.raw());
    bool          needs_sync = false;
    ShortcutEngine engine{};

    log("[ATTACH] ", path, " — ", dev.name(),
        (ufd >= 0 ? " (grab + mirror OK)" : " (grab only)"));

    input_event ev{};
    while (g_running.load(std::memory_order_relaxed)) {
        int rc = libevdev_next_event(dev.raw(),
                     LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, &ev);

        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            while (libevdev_next_event(dev.raw(), LIBEVDEV_READ_FLAG_SYNC, &ev)
                   == LIBEVDEV_READ_STATUS_SYNC) passEvent(ufd, ev);
            passSync(ufd); needs_sync = false; continue;
        }
        if (rc == -EAGAIN) { std::this_thread::sleep_for(1ms); continue; }
        if (rc < 0) break;

        if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
            if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                if (needs_sync) { passSync(ufd); needs_sync = false; }
                continue;
            }
            bool consumed = engine.process(ev, ufd);
            if (!consumed) needs_sync = true;
        }
    }

    log("[DETACH] ", path);
    if (ufd >= 0) { ::ioctl(ufd, UI_DEV_DESTROY); ::close(ufd); }
}

/* ═══════════════════════════════ Scanner ════════════════════════════════ */

static std::vector<std::thread> scanAndAttach()
{
    std::vector<std::thread> threads;
    std::vector<std::string> km_paths;
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(INPUT_DIR, ec)) {
        if (entry.path().filename().string().rfind("event", 0) != 0) continue;
        const std::string path = entry.path().string();

        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        libevdev* probe = nullptr;
        libevdev_new_from_fd(fd, &probe);
        ::close(fd);
        if (!probe) continue;

        DeviceKind kind = classify(probe);
        libevdev_free(probe);

        switch (kind) {
            case DeviceKind::Touchpad:      threads.emplace_back(touchpadThread, path);  break;
            case DeviceKind::KeyboardMouse: km_paths.push_back(path);                    break;
            case DeviceKind::Ignore:                                                      break;
        }
    }

    if (!threads.empty()) std::this_thread::sleep_for(300ms);

    {
        std::lock_guard<std::mutex> lk(g_pinch_mtx);
        g_pinch_ufd = createVirtualTouchpad();
    }

    for (const auto& p : km_paths)
        threads.emplace_back(keyboardMouseThread, p);

    if (ec) std::cerr << "[ERROR] " << INPUT_DIR << ": " << ec.message() << '\n';
    return threads;
}

/* ═══════════════════════════════ Signal + main ══════════════════════════ */

static void handleSignal(int) { g_running.store(false); }

int main()
{
    std::cout
        << "╔══════════════════════════════════════════════════════╗\n"
        << "║  PinchKey — Trackpad Pinch Gesture Engine            ║\n"
        << "║                                                      ║\n"
        << "║  Phase 2 : finger distances logged on every frame    ║\n"
        << "║  Phase 3 : ALT+'+' → pinch-out  /  ALT+'-' → pinch-in║\n"
        << "║            ALT+WheelUp → pinch-out                   ║\n"
        << "║            ALT+WheelDown → pinch-in                  ║\n"
        << "╚══════════════════════════════════════════════════════╝\n\n"
        << "Scanning: " << INPUT_DIR << "\n\n";
    std::cout.flush();

    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    auto threads = scanAndAttach();
    if (threads.empty()) {
        std::cerr << "No accessible devices.\nTry: sudo ./PinchKey\n";
        return 1;
    }

    std::cout << "Listening on " << threads.size()
              << " device(s). Ctrl+C to quit.\n\n";
    std::cout.flush();

    while (g_running.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(200ms);

    std::cout << "\nShutting down...\n";
    {
        std::lock_guard<std::mutex> lk(g_pinch_mtx);
        if (g_pinch_ufd >= 0) {
            ::ioctl(g_pinch_ufd, UI_DEV_DESTROY);
            ::close(g_pinch_ufd);
        }
    }
    for (auto& t : threads) if (t.joinable()) t.detach();
    return 0;
}
