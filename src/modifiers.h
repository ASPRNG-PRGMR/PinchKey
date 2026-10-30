#pragma once
/*
 * Process-global trigger-modifier state.
 *
 * This exists to fix a concrete bug in the prototype: ShortcutEngine was a
 * local inside each per-device thread, so modifier state was per-device. Alt
 * arrives on the keyboard's event node while the wheel arrives on the mouse's,
 * so the wheel handler's `alt_held` was permanently false and Alt+Wheel could
 * never fire. It only appeared to work for Alt+'+' because both keys ride the
 * same device.
 *
 * A counter rather than a flag: left and right Alt are distinct keys, and a
 * grabbed device can deliver a press whose release lands after a regrab.
 */

#include <atomic>

namespace pk {

class ModifierState {
public:
    void setHeld(int key_code, bool down);
    bool held() const { return m_count.load(std::memory_order_acquire) > 0; }

    /* Marks the modifier as consumed by a gesture, so the buffered press is
     * never forwarded to the compositor. Returns the previous value. */
    bool markConsumed() { return m_consumed.exchange(true, std::memory_order_acq_rel); }
    bool consumed() const { return m_consumed.load(std::memory_order_acquire); }
    void clearConsumed() { m_consumed.store(false, std::memory_order_release); }

private:
    std::atomic<int>  m_count{0};
    std::atomic<bool> m_consumed{false};
};

/* The trigger modifier (Alt, or a mouse side button, per config). */
extern ModifierState g_trigger_mod;

}  // namespace pk
