#pragma once
/*
 * A uinput clone of a grabbed device.
 *
 * Grabbing a device hides it from the compositor, so everything we do not
 * consume has to be replayed through a mirror or the user loses their mouse.
 */

#include <linux/input.h>

struct libevdev;

namespace pk {

class MirrorDevice {
public:
    MirrorDevice() = default;
    ~MirrorDevice();

    MirrorDevice(const MirrorDevice&)            = delete;
    MirrorDevice& operator=(const MirrorDevice&) = delete;

    /* Clones every capability of `src` so the compositor applies the same
     * settings (acceleration profile, keymap) as it would to the original. */
    bool create(libevdev* src, unsigned short vendor_tag);
    void destroy();
    bool valid() const { return m_fd >= 0; }

    void forward(const input_event& ev);
    void syncFrame();

private:
    int m_fd = -1;
};

}  // namespace pk
