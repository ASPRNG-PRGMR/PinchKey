#pragma once
/*
 * A uinput virtual multitouch touchpad.
 *
 * Geometry is cloned from a real touchpad rather than guessed. This matters:
 * libinput derives physical size from (range / resolution) and uses it to
 * normalise motion and to decide whether gestures are viable at all. The
 * phase-3 prototype fell back to 1232x928 @ 32 units/mm — a 38x29mm pad —
 * whenever a startup race beat it, which is not a plausible touchpad.
 */

#include <string>

namespace pk {

struct PadGeometry {
    int x_min = 0, x_max = 0;
    int y_min = 0, y_max = 0;
    int x_res = 12, y_res = 12;   // units per mm
    int slots = 4;                // ABS_MT_SLOT maximum

    double widthMm()  const { return x_res ? (x_max - x_min) / double(x_res) : 0.0; }
    double heightMm() const { return y_res ? (y_max - y_min) / double(y_res) : 0.0; }
    int    centerX()  const { return (x_min + x_max) / 2; }
    int    centerY()  const { return (y_min + y_max) / 2; }
    bool   valid()    const { return x_max > x_min && y_max > y_min && x_res > 0 && y_res > 0; }
};

/* Locate a real touchpad and read its geometry. Returns false if none found. */
bool probeRealTouchpad(PadGeometry& out, std::string& nameOut, std::string& pathOut);

class VirtualTouchpad {
public:
    VirtualTouchpad() = default;
    ~VirtualTouchpad();

    VirtualTouchpad(const VirtualTouchpad&)            = delete;
    VirtualTouchpad& operator=(const VirtualTouchpad&) = delete;

    /* Creates the device. `settle_ms` gives the compositor time to adopt it
     * before the first gesture is sent. */
    bool create(const PadGeometry& geo, int settle_ms = 400);
    void destroy();

    bool valid() const { return m_fd >= 0; }
    const PadGeometry& geometry() const { return m_geo; }

    /* ── MT Protocol B frame emission ─────────────────────────────────── */
    /* Two fingers on a horizontal line through the pad centre, `half_units`
     * either side of it. */
    void fingersDown(int half_units);
    void fingersMove(int half_units);
    void fingersUp();

    int mmToUnits(double mm) const { return int(mm * m_geo.x_res + 0.5); }

private:
    void emit(unsigned short type, unsigned short code, int value);
    void sync();
    void placeFinger(int slot, int x, int y, bool with_tracking_id);

    int         m_fd = -1;
    PadGeometry m_geo{};
};

}  // namespace pk
