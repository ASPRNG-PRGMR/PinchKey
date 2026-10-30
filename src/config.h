#pragma once
/*
 * Runtime configuration.
 *
 * Every value here was a `constexpr` in the phase-3 prototype, which meant a
 * recompile for every tuning change. Gesture feel needs dozens of iterations,
 * so these are now read from a file at startup.
 *
 * Search order (first file found wins):
 *   $PINCHKEY_CONFIG
 *   $XDG_CONFIG_HOME/pinchkey/pinchkey.conf   (~/.config/pinchkey/pinchkey.conf)
 *   /etc/pinchkey.conf
 */

#include <string>

namespace pk {

enum class Trigger {
    AltWheel,     // hold Alt, turn the wheel
    SideWheel,    // hold a mouse side button, turn the wheel
};

struct Config {
    /* ── trigger ──────────────────────────────────────────────────────── */
    Trigger trigger = Trigger::AltWheel;

    /* Scale factor applied per wheel notch. 1.08 => +8% per notch.
     * Raise for coarser, faster zoom; lower for finer control. */
    double zoom_step = 1.08;

    /* ── synthetic gesture shape ──────────────────────────────────────── */
    /* Tick interval of the gesture worker. 8ms ~= 125Hz, matching the report
     * rate of a real I2C touchpad. */
    int tick_ms = 8;

    /* Exponential smoothing applied per tick toward the target scale, in
     * (0,1]. 1.0 jumps instantly (jerky); 0.25 is a smooth ~4-tick glide. */
    double ease = 0.25;

    /* Initial half-gap between the two virtual fingers, in millimetres.
     * Chosen per direction so each gesture gets maximum travel: a zoom-in
     * starts with the fingers close together, a zoom-out starts them wide. */
    double start_half_mm_in  = 15.0;
    double start_half_mm_out = 50.0;

    /* Keep-out band at the pad edge so fingers never pin to the boundary. */
    double edge_margin_mm = 5.0;

    /* Smallest permitted half-gap, so the fingers never merge into one. */
    double min_half_mm = 4.0;

    /* Lift the virtual fingers after this long with no further input. Acts as
     * a backstop; normally the modifier release ends the gesture. */
    int idle_lift_ms = 400;

    /* ── device handling ──────────────────────────────────────────────── */
    /* The mouse must be grabbed to consume wheel events, or you would zoom
     * AND scroll at once. */
    bool grab_mouse = true;

    /* Grabbing the keyboard only serves to hide the bare Alt press from the
     * compositor. That is cosmetic — a held Alt does nothing in GNOME by
     * itself — and it is not worth putting PinchKey in the path of all
     * typing, nor worth a mirror device per keyboard. Off by default:
     * keyboards are read passively just to track the modifier. */
    bool grab_keyboard = false;

    /* Read the real touchpad and log slot positions / inter-finger distances.
     * Diagnostic only (the old phase 1-2 behaviour). Never grabs the pad, so
     * native trackpad gestures keep working. */
    bool monitor_touchpad = false;

    std::string log_level = "info";

    /* ── loading ──────────────────────────────────────────────────────── */
    /* Returns the path actually loaded, or "" if defaults were used. */
    std::string load();

    /* Parse one "key = value" pair. Returns false on unknown key or bad
     * value. Exposed for testing. */
    bool setOption(const std::string& key, const std::string& value);

    void dump() const;
};

}  // namespace pk
