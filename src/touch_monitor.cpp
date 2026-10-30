#include "touch_monitor.h"

#include "evdev_device.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <libevdev/libevdev.h>
#include <linux/input.h>

namespace pk {

int TouchpadState::activeCount() const
{
    int n = 0;
    for (const auto& s : slots) n += s.active ? 1 : 0;
    return n;
}

std::vector<FingerDist> computeDistances(const TouchpadState& st)
{
    struct FP { int slot, x, y; };
    std::vector<FP> fingers;
    fingers.reserve(kMaxSlots);

    for (int i = 0; i < kMaxSlots; ++i)
        if (st.slots[i].active)
            fingers.push_back({ i, st.slots[i].x, st.slots[i].y });

    std::sort(fingers.begin(), fingers.end(),
              [](const FP& a, const FP& b) { return a.x < b.x; });

    std::vector<FingerDist> out;
    out.reserve(fingers.size() > 0 ? fingers.size() - 1 : 0);
    for (size_t i = 1; i < fingers.size(); ++i) {
        const auto& a = fingers[i - 1];
        const auto& b = fingers[i];
        const float dx = float(b.x - a.x);
        const float dy = float(b.y - a.y);
        out.push_back({ a.slot, b.slot, std::sqrt(dx * dx + dy * dy) });
    }
    return out;
}

float meanDist(const std::vector<FingerDist>& d)
{
    if (d.empty()) return 0.0f;
    float sum = 0.0f;
    for (const auto& fd : d) sum += fd.dist;
    return sum / float(d.size());
}

namespace {

void onEvent(const input_event& ev, TouchpadState& st, const std::string& tag)
{
    if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT) {
        if (ev.value >= 0 && ev.value < kMaxSlots) st.current_slot = ev.value;
        return;
    }

    TouchSlot& ts = st.slots[st.current_slot];

    if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
        if (ev.value == -1) {
            if (ts.active)
                logInfo("[", tag, "] slot ", st.current_slot, " UP   active=",
                        st.activeCount() - 1);
            ts = TouchSlot{};
        } else {
            ts.active      = true;
            ts.tracking_id = ev.value;
            logInfo("[", tag, "] slot ", st.current_slot, " DOWN tid=", ev.value,
                    " active=", st.activeCount());
        }
        return;
    }

    if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) { ts.x = ev.value; return; }
    if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) { ts.y = ev.value; return; }

    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        if (st.activeCount() < 2) return;
        const auto dists = computeDistances(st);

        char buf[256]{};
        int  off = 0;
        for (size_t i = 0; i < dists.size() && off < int(sizeof(buf)) - 40; ++i)
            off += std::snprintf(buf + off, sizeof(buf) - off, "  d[%d->%d]=%.1f",
                                 dists[i].from_slot, dists[i].to_slot, dists[i].dist);

        logInfo("[", tag, "] active=", st.activeCount(), buf,
                "  mean=", meanDist(dists));
    }
}

}  // namespace

void monitorTouchpad(const std::string& path, std::atomic<bool>& running)
{
    /* grab = false: the whole point is to observe without stealing input. */
    EvdevDevice dev(path, /*grab=*/false);
    if (!dev.valid() || classify(dev.raw()) != DeviceKind::Touchpad) return;

    const std::string tag = path.substr(path.find_last_of('/') + 1);
    logInfo("monitoring touchpad ", path, " — ", dev.name(), " (read-only)");

    TouchpadState state{};
    input_event   ev{};

    while (running.load(std::memory_order_relaxed)) {
        const int rc = libevdev_next_event(
            dev.raw(), LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, &ev);

        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            while (libevdev_next_event(dev.raw(), LIBEVDEV_READ_FLAG_SYNC, &ev)
                       == LIBEVDEV_READ_STATUS_SYNC)
                onEvent(ev, state, tag);
            continue;
        }
        if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (rc < 0) break;
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS) onEvent(ev, state, tag);
    }

    logInfo("stopped monitoring ", path);
}

}  // namespace pk
