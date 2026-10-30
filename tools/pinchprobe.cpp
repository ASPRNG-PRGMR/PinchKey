/*
 * pinchprobe — standalone feasibility probe for PinchKey.
 *
 * Answers one question: does a synthetic uinput MT touchpad pinch reach
 * Brave/Chrome on GNOME Wayland as a real pointer-gesture pinch, and does it
 * zoom anchored at the mouse cursor?
 *
 * Deliberately NOT like the current repo code:
 *   - grabs nothing (your real trackpad and mouse keep working)
 *   - clones real pad geometry via EVIOCGABS instead of guessing
 *   - streams ONE continuous gesture instead of a fixed 20-frame burst
 *
 * Build: g++ -std=c++17 -O2 -o pinchprobe pinchprobe.cpp
 * Run  : sudo ./pinchprobe [out|in] [scale] [ms]
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

static int  g_xmin, g_xmax, g_ymin, g_ymax, g_xres, g_yres, g_slots;

/* ---- find the real touchpad and clone its geometry ------------------- */
static bool probeRealPad(std::string& nameOut, std::string& pathOut)
{
    DIR* d = opendir("/dev/input");
    if (!d) return false;
    bool found = false;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        std::string p = std::string("/dev/input/") + de->d_name;
        int fd = open(p.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned long keys[(KEY_MAX + 8) / (8 * sizeof(long))]{};
        unsigned long abs[(ABS_MAX + 8) / (8 * sizeof(long))]{};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)),  abs);
        auto bit = [](unsigned long* m, int b) {
            return (m[b / (8 * sizeof(long))] >> (b % (8 * sizeof(long)))) & 1UL;
        };
        /* udev's own touchpad test: MT coords + BTN_TOOL_FINGER, no stylus */
        bool isPad = bit(abs, ABS_MT_POSITION_X) && bit(abs, ABS_MT_POSITION_Y)
                  && bit(keys, BTN_TOOL_FINGER)  && !bit(keys, BTN_TOOL_PEN);
        if (!isPad) { close(fd); continue; }

        struct input_absinfo ai{};
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai);
        g_xmin = ai.minimum; g_xmax = ai.maximum; g_xres = ai.resolution ? ai.resolution : 12;
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai);
        g_ymin = ai.minimum; g_ymax = ai.maximum; g_yres = ai.resolution ? ai.resolution : 12;
        ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &ai);
        g_slots = ai.maximum > 0 ? ai.maximum : 4;

        char nm[256]{}; ioctl(fd, EVIOCGNAME(sizeof(nm)), nm);
        nameOut = nm; pathOut = p;
        found = true; close(fd); break;
    }
    closedir(d);
    return found;
}

/* ---- uinput plumbing ------------------------------------------------- */
static int g_ufd = -1;
static void w(__u16 t, __u16 c, __s32 v)
{
    struct input_event ev{};
    ev.type = t; ev.code = c; ev.value = v;
    if (write(g_ufd, &ev, sizeof(ev)) < 0) { /* ignore */ }
}
static void syn() { w(EV_SYN, SYN_REPORT, 0); }

static bool createPad()
{
    g_ufd = open("/dev/uinput", O_WRONLY);
    if (g_ufd < 0) { perror("open /dev/uinput"); return false; }

    ioctl(g_ufd, UI_SET_EVBIT, EV_SYN);
    ioctl(g_ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(g_ufd, UI_SET_EVBIT, EV_ABS);

    /* Mirror the real pad's button set exactly. BTN_LEFT matters: a device
     * declaring INPUT_PROP_BUTTONPAD without it is malformed to libinput. */
    for (int k : { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP,
                   BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP })
        ioctl(g_ufd, UI_SET_KEYBIT, k);

    auto axis = [](int code, int mn, int mx, int res) {
        ioctl(g_ufd, UI_SET_ABSBIT, code);
        struct uinput_abs_setup as{};
        as.code = (__u16)code;
        as.absinfo.minimum = mn; as.absinfo.maximum = mx; as.absinfo.resolution = res;
        ioctl(g_ufd, UI_ABS_SETUP, &as);
    };
    axis(ABS_X,              g_xmin, g_xmax, g_xres);
    axis(ABS_Y,              g_ymin, g_ymax, g_yres);
    axis(ABS_MT_SLOT,        0, g_slots, 0);
    axis(ABS_MT_TRACKING_ID, 0, 65535,   0);
    axis(ABS_MT_POSITION_X,  g_xmin, g_xmax, g_xres);
    axis(ABS_MT_POSITION_Y,  g_ymin, g_ymax, g_yres);
    axis(ABS_MT_TOOL_TYPE,   0, 2, 0);

    ioctl(g_ufd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    ioctl(g_ufd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);

    struct uinput_setup us{};
    us.id = { BUS_I8042, 0x06cb, 0x7357, 1 };
    snprintf(us.name, UINPUT_MAX_NAME_SIZE, "PinchKey Probe Touchpad");
    if (ioctl(g_ufd, UI_DEV_SETUP, &us) < 0) { perror("UI_DEV_SETUP"); return false; }
    if (ioctl(g_ufd, UI_DEV_CREATE) < 0)     { perror("UI_DEV_CREATE"); return false; }
    return true;
}

int main(int argc, char** argv)
{
    const bool  out      = (argc < 2) || std::string(argv[1]) != "in";
    const double scale   = (argc > 2) ? atof(argv[2]) : 2.0;   // target scale factor
    const int    durMs   = (argc > 3) ? atoi(argv[3]) : 600;   // gesture duration

    std::string padName, padPath;
    if (!probeRealPad(padName, padPath)) {
        fprintf(stderr, "No real touchpad found to clone geometry from.\n");
        return 1;
    }
    printf("Cloning geometry from: %s (%s)\n", padName.c_str(), padPath.c_str());
    printf("  X:[%d,%d] res=%d  Y:[%d,%d] res=%d  slots=%d  -> %.0fmm x %.0fmm\n",
           g_xmin, g_xmax, g_xres, g_ymin, g_ymax, g_yres, g_slots,
           (g_xmax - g_xmin) / (double)g_xres, (g_ymax - g_ymin) / (double)g_yres);

    if (!createPad()) return 1;
    printf("\nVirtual pad created. Waiting 1.5s for the compositor to adopt it...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    printf("\n>>> Put the MOUSE CURSOR over the spot in Brave you want to zoom.\n");
    printf(">>> Starting pinch-%s (scale %.2f over %dms) in 4 seconds...\n",
           out ? "OUT (zoom in)" : "IN (zoom out)", scale, durMs);
    for (int i = 4; i > 0; --i) {
        printf("   %d\n", i); fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    /* Geometry: fingers on a horizontal line through the pad centre.
     * 20mm half-gap start gives libinput plenty of separation to call it a
     * pinch rather than a two-finger scroll. */
    const int cx = (g_xmin + g_xmax) / 2;
    const int cy = (g_ymin + g_ymax) / 2;
    const int halfStart = 20 * g_xres;
    const int halfEnd   = (int)std::lround(halfStart * (out ? scale : 1.0 / scale));

    const int    FRAME_MS = 8;                       // ~125 Hz, like a real pad
    const int    frames   = std::max(2, durMs / FRAME_MS);

    auto put = [&](int slot, int x, int y, bool down) {
        w(EV_ABS, ABS_MT_SLOT, slot);
        if (down) {
            w(EV_ABS, ABS_MT_TRACKING_ID, 100 + slot);
            w(EV_ABS, ABS_MT_TOOL_TYPE,   MT_TOOL_FINGER);
        }
        w(EV_ABS, ABS_MT_POSITION_X, std::clamp(x, g_xmin, g_xmax));
        w(EV_ABS, ABS_MT_POSITION_Y, std::clamp(y, g_ymin, g_ymax));
    };

    /* --- fingers down ------------------------------------------------- */
    put(0, cx - halfStart, cy, true);
    put(1, cx + halfStart, cy, true);
    w(EV_KEY, BTN_TOUCH,          1);
    w(EV_KEY, BTN_TOOL_DOUBLETAP, 1);
    w(EV_ABS, ABS_X, cx - halfStart);
    w(EV_ABS, ABS_Y, cy);
    syn();
    std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_MS));

    /* --- continuous spread / converge --------------------------------- */
    for (int f = 1; f <= frames; ++f) {
        const double t    = (double)f / frames;
        const int    half = (int)std::lround(halfStart + (halfEnd - halfStart) * t);
        put(0, cx - half, cy, false);
        put(1, cx + half, cy, false);
        w(EV_ABS, ABS_X, cx - half);
        w(EV_ABS, ABS_Y, cy);
        syn();
        std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_MS));
    }

    /* --- fingers up --------------------------------------------------- */
    w(EV_ABS, ABS_MT_SLOT, 0); w(EV_ABS, ABS_MT_TRACKING_ID, -1);
    w(EV_ABS, ABS_MT_SLOT, 1); w(EV_ABS, ABS_MT_TRACKING_ID, -1);
    w(EV_KEY, BTN_TOUCH,          0);
    w(EV_KEY, BTN_TOOL_DOUBLETAP, 0);
    syn();

    printf("\nGesture sent. Did Brave zoom at the cursor?\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ioctl(g_ufd, UI_DEV_DESTROY);
    close(g_ufd);
    return 0;
}
