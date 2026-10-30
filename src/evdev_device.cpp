#include "evdev_device.h"

#include "log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace pk {

EvdevDevice::EvdevDevice(const std::string& path, bool grab) : m_path(path)
{
    m_fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (m_fd < 0) {
        logDebug("open ", path, ": ", std::strerror(errno));
        return;
    }
    if (libevdev_new_from_fd(m_fd, &m_dev) < 0) {
        logDebug("libevdev_new_from_fd failed on ", path);
        ::close(m_fd);
        m_fd = -1;
        return;
    }
    if (grab) {
        if (::ioctl(m_fd, EVIOCGRAB, 1) == 0) {
            m_grabbed = true;
        } else {
            logWarn("EVIOCGRAB failed on ", path, " (", std::strerror(errno),
                    ") — events will reach the compositor too");
        }
    }
}

EvdevDevice::~EvdevDevice()
{
    if (m_grabbed) ::ioctl(m_fd, EVIOCGRAB, 0);
    if (m_dev)     libevdev_free(m_dev);
    if (m_fd >= 0) ::close(m_fd);
}

const char* EvdevDevice::name() const
{
    const char* n = m_dev ? libevdev_get_name(m_dev) : nullptr;
    return n ? n : "(unnamed)";
}

const char* kindName(DeviceKind k)
{
    switch (k) {
        case DeviceKind::Touchpad: return "touchpad";
        case DeviceKind::Mouse:    return "mouse";
        case DeviceKind::Keyboard: return "keyboard";
        default:                   return "ignore";
    }
}

DeviceKind classify(libevdev* dev)
{
    if (!dev) return DeviceKind::Ignore;

    const char* raw = libevdev_get_name(dev);
    std::string lower(raw ? raw : "");
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    /* Never attach to our own virtual devices, or we would feed synthetic
     * events straight back into the recogniser. */
    if (lower.find("pinchkey") != std::string::npos) return DeviceKind::Ignore;

    const bool has_mt = libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X) &&
                        libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_Y);
    const bool has_finger = libevdev_has_event_code(dev, EV_KEY, BTN_TOOL_FINGER);
    const bool has_pen    = libevdev_has_event_code(dev, EV_KEY, BTN_TOOL_PEN);

    if (has_mt && has_finger && !has_pen) return DeviceKind::Touchpad;

    /* A wheel is what makes a device useful as a zoom source. Check it before
     * the keyboard test: combo devices (gaming mice expose a keyboard node)
     * should be treated as mice when they carry the wheel. */
    const bool has_wheel = libevdev_has_event_code(dev, EV_REL, REL_WHEEL) ||
                           libevdev_has_event_code(dev, EV_REL, REL_WHEEL_HI_RES);
    const bool has_rel_xy = libevdev_has_event_code(dev, EV_REL, REL_X) &&
                            libevdev_has_event_code(dev, EV_REL, REL_Y);
    if (has_wheel || (has_rel_xy && libevdev_has_event_code(dev, EV_KEY, BTN_LEFT)))
        return DeviceKind::Mouse;

    /* Require a letter key, not just any EV_KEY: power buttons and lid
     * switches advertise EV_KEY but are not keyboards. */
    if (libevdev_has_event_code(dev, EV_KEY, KEY_A) ||
        libevdev_has_event_code(dev, EV_KEY, KEY_LEFTALT))
        return DeviceKind::Keyboard;

    return DeviceKind::Ignore;
}

}  // namespace pk
