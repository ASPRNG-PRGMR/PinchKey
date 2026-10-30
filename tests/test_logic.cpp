/*
 * Unit tests for the parts that need no kernel devices: config parsing,
 * distance maths, and modifier counting.
 *
 * Build & run:  make test
 */

#include "../src/config.h"
#include "../src/log.h"
#include "../src/modifiers.h"
#include "../src/touch_monitor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <linux/input.h>

static int g_fail = 0;
static int g_ran  = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_ran;                                                               \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b)                                                       \
    do {                                                                       \
        ++g_ran;                                                               \
        if (std::fabs(double(a) - double(b)) > 1e-3) {                         \
            std::printf("  FAIL %s:%d  %s (%g) != %s (%g)\n", __FILE__,        \
                        __LINE__, #a, double(a), #b, double(b));               \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

static void testConfigParsing()
{
    std::printf("config parsing\n");
    pk::Config c;

    CHECK(c.setOption("zoom_step", "1.25"));
    CHECK_NEAR(c.zoom_step, 1.25);

    /* zoom_step must exceed 1: it is a multiplier per notch, and <=1 would
     * mean scrolling up zooms out or does nothing. */
    CHECK(!c.setOption("zoom_step", "1.0"));
    CHECK(!c.setOption("zoom_step", "0.5"));

    CHECK(c.setOption("trigger", "side_wheel"));
    CHECK(c.trigger == pk::Trigger::SideWheel);
    CHECK(!c.setOption("trigger", "nonsense"));

    CHECK(c.setOption("ease", "1.0"));
    CHECK(!c.setOption("ease", "0"));      // would never converge
    CHECK(!c.setOption("ease", "1.5"));    // would overshoot every tick

    CHECK(c.setOption("tick_ms", "8"));
    CHECK(!c.setOption("tick_ms", "0"));
    CHECK(!c.setOption("tick_ms", "500"));

    CHECK(c.setOption("grab_mouse", "no"));
    CHECK(c.grab_mouse == false);
    CHECK(c.setOption("grab_mouse", "true"));
    CHECK(c.grab_mouse == true);
    CHECK(!c.setOption("grab_mouse", "maybe"));

    /* Trailing garbage must not parse as a number. */
    CHECK(!c.setOption("tick_ms", "8abc"));
    CHECK(!c.setOption("zoom_step", "1.2x"));

    CHECK(!c.setOption("no_such_key", "1"));
}

static void testDistances()
{
    std::printf("distance maths\n");

    pk::TouchpadState st{};
    CHECK(st.activeCount() == 0);
    CHECK(pk::computeDistances(st).empty());
    CHECK_NEAR(pk::meanDist(pk::computeDistances(st)), 0.0);

    /* Two fingers, a clean 3-4-5 triangle. */
    st.slots[0] = { true, 100, 100, 1 };
    st.slots[1] = { true, 130, 140, 2 };
    CHECK(st.activeCount() == 2);

    auto d = pk::computeDistances(st);
    CHECK(d.size() == 1);
    CHECK_NEAR(d[0].dist, 50.0);
    CHECK_NEAR(pk::meanDist(d), 50.0);

    /* Three fingers give two adjacent pairs, and must be ordered by x
     * regardless of which slot they occupy. Slot 2 sits leftmost here. */
    st.slots[2] = { true, 0, 100, 3 };
    d = pk::computeDistances(st);
    CHECK(d.size() == 2);
    CHECK(d[0].from_slot == 2);   // leftmost first
    CHECK(d[0].to_slot   == 0);
    CHECK_NEAR(d[0].dist, 100.0);
    CHECK_NEAR(pk::meanDist(d), (100.0 + 50.0) / 2.0);

    /* A lifted finger must drop out of the calculation entirely. */
    st.slots[2] = pk::TouchSlot{};
    d = pk::computeDistances(st);
    CHECK(d.size() == 1);
}

static void testModifierCounting()
{
    std::printf("modifier counting\n");
    pk::ModifierState m;

    CHECK(!m.held());
    m.setHeld(KEY_LEFTALT, true);
    CHECK(m.held());

    /* Both Alts down, then one up: still held. This is the case a plain bool
     * gets wrong. */
    m.setHeld(KEY_RIGHTALT, true);
    m.setHeld(KEY_LEFTALT, false);
    CHECK(m.held());
    m.setHeld(KEY_RIGHTALT, false);
    CHECK(!m.held());

    /* A release with no matching press must not drive the count negative, or
     * the modifier would read as held forever after. */
    m.setHeld(KEY_LEFTALT, false);
    CHECK(!m.held());
    m.setHeld(KEY_LEFTALT, true);
    CHECK(m.held());
    m.setHeld(KEY_LEFTALT, false);
    CHECK(!m.held());

    CHECK(!m.consumed());
    CHECK(!m.markConsumed());
    CHECK(m.consumed());
    CHECK(m.markConsumed());     // returns previous value
    m.clearConsumed();
    CHECK(!m.consumed());
}

int main()
{
    pk::g_log_level = pk::LogLevel::Error;   // keep test output clean

    testConfigParsing();
    testDistances();
    testModifierCounting();

    std::printf("\n%d checks, %d failed\n", g_ran, g_fail);
    return g_fail == 0 ? 0 : 1;
}
