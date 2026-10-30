#include "input_router.h"

#include "log.h"
#include "modifiers.h"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <thread>

#include <libevdev/libevdev.h>

namespace pk {

namespace {

bool isTriggerKey(const Config& cfg, int code)
{
    switch (cfg.trigger) {
        case Trigger::AltWheel:
            return code == KEY_LEFTALT || code == KEY_RIGHTALT;
        case Trigger::SideWheel:
            return code == BTN_SIDE || code == BTN_EXTRA;
    }
    return false;
}

}  // namespace

/* ── shared modifier bookkeeping ─────────────────────────────────────────── */
void InputRouter::noteModifier(const input_event& ev)
{
    if (ev.value == 1) {
        g_trigger_mod.setHeld(ev.code, true);
        g_trigger_mod.clearConsumed();
        return;
    }
    if (ev.value == 0) {
        g_trigger_mod.setHeld(ev.code, false);
        /* Modifier fully up and a gesture was in flight: let it finish easing
         * to its target, then lift the virtual fingers. */
        if (!g_trigger_mod.held() && g_trigger_mod.consumed())
            m_synth.release();
    }
}

/* ── passive mode ────────────────────────────────────────────────────────────
 * Track the modifier and nothing else. No grab, no mirror, no consumption.
 */
void InputRouter::handlePassive(const input_event& ev)
{
    if (ev.type == EV_KEY && isTriggerKey(m_cfg, ev.code))
        noteModifier(ev);
}

/* ── grabbed mode: trigger modifier ──────────────────────────────────────────
 * The press is buffered rather than forwarded immediately. If a wheel event
 * follows, the combo is ours and the press is never forwarded. Anything else
 * flushes the press first, so Alt+Tab and friends still work.
 */
bool InputRouter::handleModifierKey(const input_event& ev, MirrorDevice& mirror)
{
    if (ev.value == 1) {
        noteModifier(ev);
        m_press_buffered  = true;
        m_press_forwarded = false;
        m_buffered_press  = ev;
        return true;                       // withhold for now
    }

    if (ev.value == 0) {
        const bool was_consumed = g_trigger_mod.consumed();
        noteModifier(ev);

        const bool need_release = m_press_forwarded;
        m_press_buffered = false;

        /* Emit the release only if the matching press actually reached the
         * compositor. The prototype forwarded it unconditionally, leaving the
         * compositor believing a key it never saw pressed had come up. */
        if (was_consumed || !need_release) return true;

        m_press_forwarded = false;
        mirror.forward(ev);
        return false;
    }

    /* Autorepeat: swallow while buffered, so no repeat precedes the press. */
    return m_press_buffered;
}

/* ── wheel → zoom ───────────────────────────────────────────────────────── */
bool InputRouter::handleWheel(const input_event& ev)
{
    if (!g_trigger_mod.held()) return false;

    g_trigger_mod.markConsumed();
    m_press_buffered = false;

    /* REL_WHEEL_HI_RES duplicates REL_WHEEL at finer granularity. Consume both
     * so neither reaches the compositor as a scroll, but act only on the
     * coarse one or every notch would zoom twice. */
    if (ev.code == REL_WHEEL_HI_RES) return true;
    if (ev.value == 0)               return true;

    const int    notches = std::abs(ev.value);
    const double step    = (ev.value > 0) ? m_cfg.zoom_step : 1.0 / m_cfg.zoom_step;
    m_synth.nudge(std::pow(step, notches));

    logDebug("wheel ", ev.value > 0 ? "up" : "down", " x", notches,
             " -> zoom ", ev.value > 0 ? "in" : "out");
    return true;
}

bool InputRouter::handleGrabbed(const input_event& ev, MirrorDevice& mirror)
{
    if (ev.type == EV_KEY && isTriggerKey(m_cfg, ev.code))
        return handleModifierKey(ev, mirror);

    if (ev.type == EV_REL &&
        (ev.code == REL_WHEEL || ev.code == REL_WHEEL_HI_RES) &&
        handleWheel(ev))
        return true;

    /* Flush a buffered trigger press first so ordering is preserved. */
    if (m_press_buffered) {
        mirror.forward(m_buffered_press);
        m_press_buffered  = false;
        m_press_forwarded = true;
    }

    mirror.forward(ev);
    return false;
}

/* ── device loop ─────────────────────────────────────────────────────────── */
void InputRouter::run(const std::string& path, DeviceKind kind, bool grab,
                      unsigned short tag)
{
    EvdevDevice dev(path, grab);
    if (!dev.valid()) return;

    /* Re-check after opening: the device list may have changed since the scan. */
    if (classify(dev.raw()) != kind) {
        logDebug("skip ", path, ": kind changed since scan");
        return;
    }

    MirrorDevice mirror;
    if (grab) {
        if (!dev.grabbed()) {
            logError("Could not grab ", path, " — not routing it, since we would "
                     "consume its events without being able to replay them");
            return;
        }
        if (!mirror.create(dev.raw(), tag)) {
            logError("No mirror for grabbed device ", path,
                     " — refusing to run, that would swallow all its input");
            return;
        }
    }

    logInfo("attached ", path, " (", kindName(kind), ") — ", dev.name(),
            grab ? " [grabbed+mirrored]" : " [passive, modifier only]");

    bool        pending_sync = false;
    input_event ev{};

    while (m_running.load(std::memory_order_relaxed)) {
        const int rc = libevdev_next_event(
            dev.raw(), LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, &ev);

        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            /* Dropped events: replay the resync stream verbatim. */
            while (libevdev_next_event(dev.raw(), LIBEVDEV_READ_FLAG_SYNC, &ev)
                       == LIBEVDEV_READ_STATUS_SYNC) {
                if (grab) mirror.forward(ev);
                else      handlePassive(ev);
            }
            if (grab) mirror.syncFrame();
            pending_sync = false;
            continue;
        }
        if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (rc < 0) break;
        if (rc != LIBEVDEV_READ_STATUS_SUCCESS) continue;

        if (!grab) { handlePassive(ev); continue; }

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            /* Only terminate a frame if something in it survived, so a fully
             * consumed gesture frame produces no empty SYN_REPORT. */
            if (pending_sync) { mirror.syncFrame(); pending_sync = false; }
            continue;
        }

        if (!handleGrabbed(ev, mirror)) pending_sync = true;
    }

    logInfo("detached ", path);
}

}  // namespace pk
