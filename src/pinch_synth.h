#pragma once
/*
 * Continuous pinch synthesis.
 *
 * Why this shape: Wayland delivers a pinch as zwp_pointer_gestures_v1
 * begin / update* / end, where each update carries only a scale factor and no
 * coordinates. Chromium's Blink applies the *delta* of that scale to the
 * current page scale (Viewport::PinchUpdate), anchored at the pointer. So a
 * natural zoom needs one long-lived gesture whose scale tracks the input,
 * not a burst of fixed-size gestures.
 *
 * libinput reports scale as (current finger distance / initial distance), so
 * driving scale is simply a matter of placing the fingers at
 * start_half * scale.
 *
 * The prototype instead fired a fixed 20-frame gesture while holding a mutex
 * across ~336ms of sleeps, gated behind a 350ms cooldown. That can only ever
 * produce a fixed zoom quantum.
 *
 * Travel is bounded by the pad: starting at a 15mm half-gap on a 121mm pad
 * leaves room for roughly 3.7x before the fingers reach the edge. Releasing
 * the modifier lifts the fingers, so the next scroll starts a fresh gesture
 * from the base gap — page scale accumulates across gestures, which is how
 * unbounded zoom is reached without an in-gesture re-grip (a re-grip would
 * read as end+begin and visibly re-anchor).
 */

#include "config.h"
#include "uinput_pad.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace pk {

class PinchSynth {
public:
    PinchSynth(VirtualTouchpad& pad, const Config& cfg);
    ~PinchSynth();

    PinchSynth(const PinchSynth&)            = delete;
    PinchSynth& operator=(const PinchSynth&) = delete;

    void start();
    void stop();

    /* Multiply the target scale. >1 zooms in, <1 zooms out. Safe to call from
     * any input thread; never blocks on the gesture. */
    void nudge(double factor);

    /* Trigger released: finish easing to the target, then lift. */
    void release();

private:
    enum class State { Idle, Active };

    void worker();
    void beginGesture(bool zoom_in);
    void endGesture();
    double clampTargetLocked(double want) const;

    VirtualTouchpad& m_pad;
    const Config&    m_cfg;

    std::thread              m_thread;
    std::atomic<bool>        m_running{false};
    std::mutex               m_mtx;
    std::condition_variable  m_cv;

    /* All of the following are guarded by m_mtx. */
    State  m_state       = State::Idle;
    double m_target      = 1.0;   // where the scale is headed
    double m_current     = 1.0;   // where it is now
    double m_start_half  = 0.0;   // mm, fixed for the life of one gesture
    bool   m_release_req = false;
    std::chrono::steady_clock::time_point m_last_input{};

    /* Derived once from pad geometry. */
    double m_max_half_mm = 0.0;
    double m_min_half_mm = 0.0;
};

}  // namespace pk
