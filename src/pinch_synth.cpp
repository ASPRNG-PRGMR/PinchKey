#include "pinch_synth.h"

#include "log.h"

#include <algorithm>
#include <cmath>

namespace pk {

using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::milliseconds;

namespace {
/* Scale is considered settled once it is this close to the target. */
constexpr double kSettleEps = 0.0015;
}  // namespace

PinchSynth::PinchSynth(VirtualTouchpad& pad, const Config& cfg)
    : m_pad(pad), m_cfg(cfg)
{
    const double half_width_mm = m_pad.geometry().widthMm() / 2.0;
    m_max_half_mm = std::max(1.0, half_width_mm - m_cfg.edge_margin_mm);
    m_min_half_mm = std::min(m_cfg.min_half_mm, m_max_half_mm / 2.0);

    logInfo("pinch travel: half-gap ", m_min_half_mm, "mm .. ", m_max_half_mm,
            "mm  (max ", m_max_half_mm / m_cfg.start_half_mm_in,
            "x zoom-in per gesture, ",
            m_min_half_mm / m_cfg.start_half_mm_out, "x zoom-out)");
}

PinchSynth::~PinchSynth() { stop(); }

void PinchSynth::start()
{
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { worker(); });
}

void PinchSynth::stop()
{
    if (!m_running.exchange(false)) return;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();

    /* Never leave fingers down on the virtual pad. */
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_state == State::Active) {
        m_pad.fingersUp();
        m_state = State::Idle;
    }
}

double PinchSynth::clampTargetLocked(double want) const
{
    /* Bound the target to what the pad can physically express, so a long
     * scroll burst cannot park the target somewhere unreachable and leave the
     * easing loop crawling against a clamp. */
    const double lo = m_min_half_mm / m_start_half;
    const double hi = m_max_half_mm / m_start_half;
    return std::clamp(want, lo, hi);
}

void PinchSynth::nudge(double factor)
{
    if (factor <= 0.0) return;

    std::lock_guard<std::mutex> lk(m_mtx);
    m_last_input  = Clock::now();
    m_release_req = false;

    if (m_state == State::Idle) {
        /* Pick the starting gap from the direction of travel, so each gesture
         * gets the most headroom in the direction actually wanted. */
        const bool zoom_in = factor > 1.0;
        m_start_half = zoom_in ? m_cfg.start_half_mm_in : m_cfg.start_half_mm_out;
        m_current    = 1.0;
        m_target     = clampTargetLocked(factor);
    } else {
        m_target = clampTargetLocked(m_target * factor);
    }
    m_cv.notify_all();
}

void PinchSynth::release()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_state == State::Idle) return;
    m_release_req = true;
    m_cv.notify_all();
}

void PinchSynth::beginGesture(bool zoom_in)
{
    const int half_units = m_pad.mmToUnits(m_start_half);
    m_pad.fingersDown(half_units);
    m_state = State::Active;
    logDebug("gesture begin (", zoom_in ? "in" : "out",
             ") start_half=", m_start_half, "mm target=", m_target);
}

void PinchSynth::endGesture()
{
    m_pad.fingersUp();
    m_state       = State::Idle;
    m_target      = 1.0;
    m_current     = 1.0;
    m_release_req = false;
    logDebug("gesture end");
}

void PinchSynth::worker()
{
    std::unique_lock<std::mutex> lk(m_mtx);

    while (m_running.load(std::memory_order_relaxed)) {
        if (m_state == State::Idle) {
            /* Nothing to animate — sleep until a nudge arrives. */
            m_cv.wait(lk, [this] {
                return !m_running.load(std::memory_order_relaxed) ||
                       m_target != 1.0;
            });
            if (!m_running.load(std::memory_order_relaxed)) break;

            beginGesture(m_target > 1.0);
            continue;
        }

        /* Active: ease toward the target and emit one frame per tick. */
        const double gap = m_target - m_current;
        if (std::abs(gap) < kSettleEps) {
            m_current = m_target;
        } else {
            m_current += gap * m_cfg.ease;
        }

        const double half_mm = std::clamp(m_start_half * m_current,
                                          m_min_half_mm, m_max_half_mm);
        m_pad.fingersMove(m_pad.mmToUnits(half_mm));

        const bool settled = (m_current == m_target);
        const bool idle_expired =
            std::chrono::duration_cast<Ms>(Clock::now() - m_last_input).count()
                >= m_cfg.idle_lift_ms;

        /* Lift once the motion has caught up with the input — either because
         * the trigger was released, or because input stopped arriving. Waiting
         * for `settled` means a release never truncates the zoom mid-glide. */
        if (settled && (m_release_req || idle_expired)) {
            endGesture();
            continue;
        }

        m_cv.wait_for(lk, Ms(m_cfg.tick_ms));
    }

    if (m_state == State::Active) {
        m_pad.fingersUp();
        m_state = State::Idle;
    }
}

}  // namespace pk
