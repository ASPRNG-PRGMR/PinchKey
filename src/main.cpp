/*
 * PinchKey — drive a browser's native pinch-zoom from a mouse.
 *
 * Hold Alt and turn the wheel; PinchKey synthesises a continuous two-finger
 * pinch on a virtual touchpad. libinput recognises it, the compositor
 * forwards it as a pointer-gesture pinch, and Chrome/Brave applies its native
 * page-scale zoom anchored at the mouse cursor.
 *
 * This is deliberately not a screen magnifier and not Ctrl+scroll browser
 * zoom: it is the same gesture a real trackpad produces, from a mouse.
 */

#include "config.h"
#include "evdev_device.h"
#include "input_router.h"
#include "log.h"
#include "pinch_synth.h"
#include "touch_monitor.h"
#include "uinput_pad.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_running{true};

void handleSignal(int) { g_running.store(false, std::memory_order_relaxed); }

struct Found {
    std::string    path;
    pk::DeviceKind kind;
    bool           has_wheel   = false;
    bool           has_trigger = false;
};

std::vector<Found> scanDevices(const pk::Config& cfg)
{
    std::vector<Found> out;
    std::error_code    ec;

    const int trig_a = (cfg.trigger == pk::Trigger::AltWheel) ? KEY_LEFTALT  : BTN_SIDE;
    const int trig_b = (cfg.trigger == pk::Trigger::AltWheel) ? KEY_RIGHTALT : BTN_EXTRA;

    for (const auto& entry : fs::directory_iterator("/dev/input", ec)) {
        const std::string fname = entry.path().filename().string();
        if (fname.rfind("event", 0) != 0) continue;

        const std::string path = entry.path().string();
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        libevdev* probe = nullptr;
        if (libevdev_new_from_fd(fd, &probe) < 0 || !probe) { ::close(fd); continue; }
        const pk::DeviceKind kind = pk::classify(probe);

        Found f{ path, kind, false, false };
        f.has_wheel   = libevdev_has_event_code(probe, EV_REL, REL_WHEEL) ||
                        libevdev_has_event_code(probe, EV_REL, REL_WHEEL_HI_RES);
        f.has_trigger = libevdev_has_event_code(probe, EV_KEY, trig_a) ||
                        libevdev_has_event_code(probe, EV_KEY, trig_b);

        libevdev_free(probe);
        ::close(fd);

        if (kind != pk::DeviceKind::Ignore) out.push_back(f);
    }

    if (ec) pk::logError("/dev/input: ", ec.message());

    /* Stable order makes logs comparable between runs. */
    std::sort(out.begin(), out.end(),
              [](const Found& a, const Found& b) { return a.path < b.path; });
    return out;
}

void printUsage(const char* argv0)
{
    std::printf(
        "PinchKey — mouse-driven native pinch zoom\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "  --monitor-touchpad   Also log real touchpad slots/distances (read-only)\n"
        "  --grab-keyboard      Grab keyboards to hide the bare Alt press\n"
        "  --log-level LEVEL    error | warn | info | debug\n"
        "  --list               List classified input devices and exit\n"
        "  -h, --help           This message\n"
        "\n"
        "Config file (first found wins):\n"
        "  $PINCHKEY_CONFIG\n"
        "  ~/.config/pinchkey/pinchkey.conf\n"
        "  /etc/pinchkey.conf\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv)
{
    pk::Config cfg;
    const std::string loaded = cfg.load();

    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help")       { printUsage(argv[0]); return 0; }
        else if (a == "--list")               { list_only = true; }
        else if (a == "--monitor-touchpad")   { cfg.monitor_touchpad = true; }
        else if (a == "--grab-keyboard")      { cfg.grab_keyboard = true; }
        else if (a == "--log-level" && i + 1 < argc) {
            if (!pk::setLogLevelByName(argv[++i])) {
                pk::logError("Unknown log level: ", argv[i]);
                return 2;
            }
        } else {
            pk::logError("Unknown argument: ", a);
            printUsage(argv[0]);
            return 2;
        }
    }

    if (loaded.empty())
        pk::logInfo("no config file found — using built-in defaults");
    else
        pk::logInfo("config loaded from ", loaded);
    cfg.dump();

    const auto devices = scanDevices(cfg);

    if (list_only) {
        for (const auto& d : devices) {
            pk::EvdevDevice dev(d.path, /*grab=*/false);
            const char* role = (d.kind == pk::DeviceKind::Mouse && d.has_wheel)      ? "ZOOM SRC"
                             : (d.kind == pk::DeviceKind::Keyboard && d.has_trigger) ? "TRIGGER"
                             : (d.kind == pk::DeviceKind::Touchpad)                  ? "geometry"
                                                                                     : "-";
            std::printf("%-20s %-9s %-9s %s\n", d.path.c_str(),
                        pk::kindName(d.kind), role,
                        dev.valid() ? dev.name() : "(unreadable)");
        }
        return 0;
    }

    /* Geometry comes from a real pad so libinput sees a plausible device. */
    pk::PadGeometry geo;
    std::string     pad_name, pad_path;
    if (!pk::probeRealTouchpad(geo, pad_name, pad_path)) {
        pk::logError("No real touchpad found to clone geometry from.");
        pk::logError("PinchKey needs one as a template for the virtual pad.");
        return 1;
    }
    pk::logInfo("cloning geometry from ", pad_name, " (", pad_path, ") — ",
                geo.widthMm(), "mm x ", geo.heightMm(), "mm, ",
                geo.slots + 1, " slots");

    pk::VirtualTouchpad pad;
    if (!pad.create(geo)) return 1;

    pk::PinchSynth synth(pad, cfg);
    synth.start();

    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGHUP,  handleSignal);

    std::vector<std::thread> threads;
    unsigned short           tag       = 1;
    int                      n_grabbed = 0;
    int                      n_passive = 0;

    for (const auto& d : devices) {
        if (d.kind == pk::DeviceKind::Touchpad) {
            if (cfg.monitor_touchpad)
                threads.emplace_back(pk::monitorTouchpad, d.path, std::ref(g_running));
            continue;
        }
        /* Attach only to what can actually contribute to the gesture. A
         * keyboard with no Alt key or a mouse with no wheel is left alone;
         * without this filter we touched nine devices on a laptop with one
         * mouse, including the touchpad's own companion pointer node. */
        bool want = false;
        bool grab = false;

        if (d.kind == pk::DeviceKind::Mouse && d.has_wheel) {
            want = true;
            grab = cfg.grab_mouse;   // must grab to swallow the wheel
        } else if (d.kind == pk::DeviceKind::Keyboard && d.has_trigger) {
            want = true;
            grab = cfg.grab_keyboard;
        }

        if (!want) {
            pk::logDebug("skipping ", d.path, " (", pk::kindName(d.kind), ")");
            continue;
        }

        threads.emplace_back([&cfg, &synth, d, grab, tag]() {
            pk::InputRouter router(cfg, synth, g_running);
            router.run(d.path, d.kind, grab, tag);
        });
        ++tag;
        if (grab) ++n_grabbed; else ++n_passive;
    }

    if (n_grabbed == 0) {
        pk::logError("No wheel-capable mouse could be grabbed.");
        pk::logError("Check permissions: ls -l /dev/input/event*  (need group 'input')");
        synth.stop();
        return 1;
    }

    pk::logInfo("running: ", n_grabbed, " grabbed, ", n_passive,
                " passive — hold Alt and scroll to zoom. Ctrl+C to quit.");

    while (g_running.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

    pk::logInfo("shutting down");

    /* Order matters: stop synthesis (which lifts any fingers still down)
     * before tearing down the pad, or the compositor is left holding a
     * half-finished gesture. */
    synth.stop();

    /* Joining is what makes this safe to run under systemd with Restart=:
     * the prototype detached its threads, so a restart could race stale
     * mirror devices against fresh ones. Each router returns once g_running
     * is false and its blocking read is interrupted by the signal. */
    for (auto& t : threads)
        if (t.joinable()) t.join();

    pad.destroy();
    pk::logInfo("stopped cleanly");
    return 0;
}
