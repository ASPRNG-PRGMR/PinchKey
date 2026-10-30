#include "modifiers.h"

namespace pk {

ModifierState g_trigger_mod;

void ModifierState::setHeld(int /*key_code*/, bool down)
{
    if (down) {
        m_count.fetch_add(1, std::memory_order_acq_rel);
    } else {
        /* Clamp at zero: a release without a matching press is possible when a
         * device is grabbed while the key is already down. */
        int cur = m_count.load(std::memory_order_acquire);
        while (cur > 0 &&
               !m_count.compare_exchange_weak(cur, cur - 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        }
    }
}

}  // namespace pk
