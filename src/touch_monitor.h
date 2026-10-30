#pragma once
/*
 * Read-only touchpad observer — the old phase 1 and 2 behaviour.
 *
 * Kept as a diagnostic because the real trackpad already produces native
 * pinch gestures through libinput; re-injecting them would double-apply the
 * zoom. This never grabs the pad, so native gestures keep working while you
 * watch slot positions and inter-finger distances.
 *
 * Enable with `monitor_touchpad = true` or --monitor-touchpad.
 */

#include <atomic>
#include <string>
#include <vector>

namespace pk {

constexpr int kMaxSlots = 10;

struct TouchSlot {
    bool active      = false;
    int  x = 0, y = 0;
    int  tracking_id = -1;
};

struct TouchpadState {
    TouchSlot slots[kMaxSlots];
    int       current_slot = 0;

    int activeCount() const;
};

struct FingerDist {
    int   from_slot;
    int   to_slot;
    float dist;
};

/* Adjacent-pair Euclidean distances, fingers sorted left to right by x. */
std::vector<FingerDist> computeDistances(const TouchpadState& st);
float meanDist(const std::vector<FingerDist>& d);

void monitorTouchpad(const std::string& path, std::atomic<bool>& running);

}  // namespace pk
