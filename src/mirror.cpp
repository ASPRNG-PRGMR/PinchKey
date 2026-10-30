#include "mirror.h"

#include "log.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace pk {

MirrorDevice::~MirrorDevice() { destroy(); }

bool MirrorDevice::create(libevdev* src, unsigned short vendor_tag)
{
    if (!src) return false;

    m_fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (m_fd < 0) {
        logError("Cannot open /dev/uinput for mirror: ", std::strerror(errno));
        return false;
    }

    ::ioctl(m_fd, UI_SET_EVBIT, EV_SYN);

    struct TypeMap { int type; int set_bit_req; int max; };
    /* EV_MSC carries MSC_SCAN, which the compositor uses for keymap lookup;
     * dropping it makes a mirrored keyboard behave oddly. */
    const TypeMap types[] = {
        { EV_KEY, UI_SET_KEYBIT, KEY_MAX },
        { EV_REL, UI_SET_RELBIT, REL_MAX },
        { EV_MSC, UI_SET_MSCBIT, MSC_MAX },
    };

    for (const auto& t : types) {
        if (!libevdev_has_event_type(src, t.type)) continue;
        ::ioctl(m_fd, UI_SET_EVBIT, t.type);
        for (int c = 0; c <= t.max; ++c)
            if (libevdev_has_event_code(src, t.type, c))
                ::ioctl(m_fd, t.set_bit_req, c);
    }

    if (libevdev_has_event_type(src, EV_ABS)) {
        ::ioctl(m_fd, UI_SET_EVBIT, EV_ABS);
        for (int c = 0; c <= ABS_MAX; ++c) {
            if (!libevdev_has_event_code(src, EV_ABS, c)) continue;
            ::ioctl(m_fd, UI_SET_ABSBIT, c);
            if (const struct input_absinfo* ai = libevdev_get_abs_info(src, c)) {
                struct uinput_abs_setup as{};
                as.code    = static_cast<unsigned short>(c);
                as.absinfo = *ai;
                ::ioctl(m_fd, UI_ABS_SETUP, &as);
            }
        }
    }

    struct uinput_setup us{};
    /* Distinct product id per mirror; the prototype gave every mirror
     * 0xBEEF/0xBEEF, making them indistinguishable in logs and settings. */
    us.id = { BUS_VIRTUAL, 0x7000, vendor_tag, 1 };
    const char* n = libevdev_get_name(src);
    std::snprintf(us.name, UINPUT_MAX_NAME_SIZE, "%.48s [PinchKey]", n ? n : "device");

    if (::ioctl(m_fd, UI_DEV_SETUP, &us) < 0 || ::ioctl(m_fd, UI_DEV_CREATE) < 0) {
        logError("mirror UI_DEV_CREATE failed: ", std::strerror(errno));
        destroy();
        return false;
    }
    return true;
}

void MirrorDevice::destroy()
{
    if (m_fd < 0) return;
    ::ioctl(m_fd, UI_DEV_DESTROY);
    ::close(m_fd);
    m_fd = -1;
}

void MirrorDevice::forward(const input_event& ev)
{
    if (m_fd < 0) return;
    if (::write(m_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        logDebug("mirror write failed: ", std::strerror(errno));
}

void MirrorDevice::syncFrame()
{
    input_event syn{};
    syn.type = EV_SYN;
    syn.code = SYN_REPORT;
    forward(syn);
}

}  // namespace pk
