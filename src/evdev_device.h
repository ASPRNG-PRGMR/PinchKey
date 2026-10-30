#pragma once
/*
 * RAII libevdev wrapper.
 *
 * Grabbing is an explicit constructor argument. In the prototype the
 * constructor always issued EVIOCGRAB, including on the real touchpad — which
 * was never mirrored, so running PinchKey left the laptop trackpad completely
 * dead (no pointer, no clicks, no scroll).
 */

#include <string>

struct libevdev;

namespace pk {

enum class DeviceKind { Ignore, Touchpad, Mouse, Keyboard };

class EvdevDevice {
public:
    EvdevDevice(const std::string& path, bool grab);
    ~EvdevDevice();

    EvdevDevice(const EvdevDevice&)            = delete;
    EvdevDevice& operator=(const EvdevDevice&) = delete;

    bool        valid()   const { return m_dev != nullptr; }
    bool        grabbed() const { return m_grabbed; }
    libevdev*   raw()     const { return m_dev; }
    const char* name()    const;
    const std::string& path() const { return m_path; }

private:
    std::string m_path;
    int         m_fd      = -1;
    libevdev*   m_dev     = nullptr;
    bool        m_grabbed = false;
};

/* Classify a probed device. Devices we created ourselves are Ignore'd. */
DeviceKind classify(libevdev* dev);
const char* kindName(DeviceKind k);

}  // namespace pk
