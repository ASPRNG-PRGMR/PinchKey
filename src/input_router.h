#pragma once
/*
 * Per-device event loop.
 *
 * Two modes, chosen by role:
 *
 *   Passive (keyboards, by default)
 *     Read without grabbing, purely to track whether the trigger modifier is
 *     held. Nothing is consumed and nothing is mirrored. A bare held Alt does
 *     nothing in GNOME on its own, and the wheel is swallowed at the mouse, so
 *     letting Alt reach the compositor is harmless — and it keeps PinchKey out
 *     of the path of all typing.
 *
 *   Grabbed (mice, and keyboards if grab_keyboard = true)
 *     EVIOCGRAB the device, consume the trigger, and replay everything else
 *     through a mirror. The mouse must be grabbed or the wheel would both zoom
 *     and scroll.
 */

#include "config.h"
#include "evdev_device.h"
#include "mirror.h"
#include "pinch_synth.h"

#include <atomic>
#include <linux/input.h>
#include <string>

namespace pk {

class InputRouter {
public:
    InputRouter(const Config& cfg, PinchSynth& synth, std::atomic<bool>& running)
        : m_cfg(cfg), m_synth(synth), m_running(running) {}

    /* Blocks until `running` goes false or the device disappears. */
    void run(const std::string& path, DeviceKind kind, bool grab, unsigned short tag);

private:
    /* Returns true if the event was consumed (must not be forwarded). */
    bool handleGrabbed(const input_event& ev, MirrorDevice& mirror);
    void handlePassive(const input_event& ev);

    bool handleModifierKey(const input_event& ev, MirrorDevice& mirror);
    bool handleWheel(const input_event& ev);
    void noteModifier(const input_event& ev);

    const Config&      m_cfg;
    PinchSynth&        m_synth;
    std::atomic<bool>& m_running;

    /* Buffered trigger press, withheld until we know whether it belongs to a
     * zoom gesture or to a normal shortcut like Alt+Tab. Grabbed mode only. */
    bool        m_press_buffered  = false;
    bool        m_press_forwarded = false;
    input_event m_buffered_press{};
};

}  // namespace pk
