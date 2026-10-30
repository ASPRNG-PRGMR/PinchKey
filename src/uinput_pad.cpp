#include "uinput_pad.h"

#include "log.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace pk {

namespace {

constexpr int TRACKING_ID_BASE = 100;

inline bool testBit(const unsigned long* map, int bit)
{
    constexpr int kBits = 8 * sizeof(unsigned long);
    return (map[bit / kBits] >> (bit % kBits)) & 1UL;
}

}  // namespace

/* ═══════════════════════════ real-pad discovery ═════════════════════════ */

bool probeRealTouchpad(PadGeometry& out, std::string& nameOut, std::string& pathOut)
{
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        logError("Cannot open /dev/input: ", std::strerror(errno));
        return false;
    }

    bool found = false;
    while (struct dirent* de = readdir(dir)) {
        if (std::strncmp(de->d_name, "event", 5) != 0) continue;

        const std::string path = std::string("/dev/input/") + de->d_name;
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned long keys[(KEY_MAX + 8) / (8 * sizeof(unsigned long))]{};
        unsigned long abs [(ABS_MAX + 8) / (8 * sizeof(unsigned long))]{};
        ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
        ::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)),  abs);

        /* Mirror udev's own touchpad test (udev-builtin-input_id): absolute
         * coordinates plus BTN_TOOL_FINGER and no stylus. Matching udev means
         * we agree with whatever libinput will decide about this device. */
        const bool is_pad = testBit(abs,  ABS_MT_POSITION_X) &&
                            testBit(abs,  ABS_MT_POSITION_Y) &&
                            testBit(keys, BTN_TOOL_FINGER)   &&
                            !testBit(keys, BTN_TOOL_PEN);
        if (!is_pad) { ::close(fd); continue; }

        struct input_absinfo ai{};
        PadGeometry geo{};

        if (::ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0) {
            geo.x_min = ai.minimum; geo.x_max = ai.maximum;
            geo.x_res = ai.resolution > 0 ? ai.resolution : 12;
        }
        if (::ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0) {
            geo.y_min = ai.minimum; geo.y_max = ai.maximum;
            geo.y_res = ai.resolution > 0 ? ai.resolution : 12;
        }
        if (::ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &ai) == 0)
            geo.slots = ai.maximum > 0 ? ai.maximum : 4;

        char name[256]{};
        ::ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        ::close(fd);

        if (!geo.valid()) continue;

        out     = geo;
        nameOut = name;
        pathOut = path;
        found   = true;
        break;
    }

    closedir(dir);
    return found;
}

/* ═══════════════════════════ VirtualTouchpad ════════════════════════════ */

VirtualTouchpad::~VirtualTouchpad() { destroy(); }

void VirtualTouchpad::emit(unsigned short type, unsigned short code, int value)
{
    if (m_fd < 0) return;
    struct input_event ev{};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (::write(m_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        logDebug("uinput write failed: ", std::strerror(errno));
}

void VirtualTouchpad::sync() { emit(EV_SYN, SYN_REPORT, 0); }

bool VirtualTouchpad::create(const PadGeometry& geo, int settle_ms)
{
    if (!geo.valid()) {
        logError("Refusing to create virtual pad with invalid geometry");
        return false;
    }
    m_geo = geo;

    m_fd = ::open("/dev/uinput", O_WRONLY);
    if (m_fd < 0) {
        logError("Cannot open /dev/uinput: ", std::strerror(errno));
        logError("Run as root, or install the udev rule: sudo make install-udev");
        return false;
    }

    ::ioctl(m_fd, UI_SET_EVBIT, EV_SYN);
    ::ioctl(m_fd, UI_SET_EVBIT, EV_KEY);
    ::ioctl(m_fd, UI_SET_EVBIT, EV_ABS);

    /* BTN_LEFT is not optional: we declare INPUT_PROP_BUTTONPAD below, and a
     * buttonpad without BTN_LEFT is a malformed device to libinput. The
     * prototype declared the prop but never the button. */
    for (int key : { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER,
                     BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP })
        ::ioctl(m_fd, UI_SET_KEYBIT, key);

    auto setAxis = [&](int code, int mn, int mx, int res) {
        ::ioctl(m_fd, UI_SET_ABSBIT, code);
        struct uinput_abs_setup as{};
        as.code               = static_cast<unsigned short>(code);
        as.absinfo.minimum    = mn;
        as.absinfo.maximum    = mx;
        as.absinfo.resolution = res;
        ::ioctl(m_fd, UI_ABS_SETUP, &as);
    };

    setAxis(ABS_X,              m_geo.x_min, m_geo.x_max, m_geo.x_res);
    setAxis(ABS_Y,              m_geo.y_min, m_geo.y_max, m_geo.y_res);
    setAxis(ABS_MT_SLOT,        0,           m_geo.slots, 0);
    setAxis(ABS_MT_TRACKING_ID, 0,           65535,       0);
    setAxis(ABS_MT_POSITION_X,  m_geo.x_min, m_geo.x_max, m_geo.x_res);
    setAxis(ABS_MT_POSITION_Y,  m_geo.y_min, m_geo.y_max, m_geo.y_res);
    setAxis(ABS_MT_TOOL_TYPE,   0,           2,           0);

    /* Deliberately NOT declaring ABS_MT_PRESSURE / ABS_MT_TOUCH_MAJOR. The
     * prototype advertised both and reported a constant 50. Real Synaptics
     * I2C pads here expose neither, and advertising pressure can engage
     * libinput's pressure-based touch detection against a value we invented. */

    ::ioctl(m_fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    ::ioctl(m_fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);

    struct uinput_setup us{};
    us.id = { BUS_I8042, 0x06cb, 0x7357, 1 };
    std::snprintf(us.name, UINPUT_MAX_NAME_SIZE, "PinchKey Virtual Touchpad");

    if (::ioctl(m_fd, UI_DEV_SETUP, &us) < 0) {
        logError("UI_DEV_SETUP failed: ", std::strerror(errno));
        destroy();
        return false;
    }
    if (::ioctl(m_fd, UI_DEV_CREATE) < 0) {
        logError("UI_DEV_CREATE failed: ", std::strerror(errno));
        destroy();
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));

    logInfo("virtual pad ready: X[", m_geo.x_min, ",", m_geo.x_max,
            "] Y[", m_geo.y_min, ",", m_geo.y_max,
            "] res=", m_geo.x_res, "/mm  (", m_geo.widthMm(), "mm x ",
            m_geo.heightMm(), "mm)");
    return true;
}

void VirtualTouchpad::destroy()
{
    if (m_fd < 0) return;
    ::ioctl(m_fd, UI_DEV_DESTROY);
    ::close(m_fd);
    m_fd = -1;
}

void VirtualTouchpad::placeFinger(int slot, int x, int y, bool with_tracking_id)
{
    emit(EV_ABS, ABS_MT_SLOT, slot);
    if (with_tracking_id) {
        emit(EV_ABS, ABS_MT_TRACKING_ID, TRACKING_ID_BASE + slot);
        emit(EV_ABS, ABS_MT_TOOL_TYPE,   MT_TOOL_FINGER);
    }
    emit(EV_ABS, ABS_MT_POSITION_X, std::clamp(x, m_geo.x_min, m_geo.x_max));
    emit(EV_ABS, ABS_MT_POSITION_Y, std::clamp(y, m_geo.y_min, m_geo.y_max));
}

void VirtualTouchpad::fingersDown(int half_units)
{
    const int cx = m_geo.centerX(), cy = m_geo.centerY();
    placeFinger(0, cx - half_units, cy, true);
    placeFinger(1, cx + half_units, cy, true);

    /* Exactly one BTN_TOOL_* reflects the finger count. Two fingers means
     * DOUBLETAP asserted and FINGER clear. */
    emit(EV_KEY, BTN_TOUCH,          1);
    emit(EV_KEY, BTN_TOOL_FINGER,    0);
    emit(EV_KEY, BTN_TOOL_DOUBLETAP, 1);

    /* Single-touch compat axes track slot 0. */
    emit(EV_ABS, ABS_X, std::clamp(cx - half_units, m_geo.x_min, m_geo.x_max));
    emit(EV_ABS, ABS_Y, cy);
    sync();
}

void VirtualTouchpad::fingersMove(int half_units)
{
    const int cx = m_geo.centerX(), cy = m_geo.centerY();
    placeFinger(0, cx - half_units, cy, false);
    placeFinger(1, cx + half_units, cy, false);
    emit(EV_ABS, ABS_X, std::clamp(cx - half_units, m_geo.x_min, m_geo.x_max));
    emit(EV_ABS, ABS_Y, cy);
    sync();
}

void VirtualTouchpad::fingersUp()
{
    emit(EV_ABS, ABS_MT_SLOT,        0);
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(EV_ABS, ABS_MT_SLOT,        1);
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(EV_KEY, BTN_TOUCH,          0);
    emit(EV_KEY, BTN_TOOL_DOUBLETAP, 0);
    sync();
}

}  // namespace pk
