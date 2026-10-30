# PinchKey

Drive a browser's **native pinch-zoom from a mouse** on Linux.

Hold `Alt` and turn the wheel. PinchKey synthesises a continuous two-finger
pinch on a virtual touchpad; libinput recognises it, the compositor forwards it
as a pointer-gesture pinch, and Chrome/Brave applies its own page-scale zoom
**anchored at the mouse cursor** — exactly as if you had pinched a trackpad.

---

## What this is, and what it is not

This matters, because the three things below look similar and behave nothing
alike.

| | Anchored at | Reflows layout | Scope |
|---|---|---|---|
| **Pinch zoom** (what PinchKey drives) | mouse cursor | no | per page |
| `Ctrl` + scroll (browser zoom) | viewport | **yes** | per site |
| GNOME Magnifier | screen | no | whole desktop |

PinchKey is **not** a macOS-style system-wide zoom, and not a screen
magnifier. It produces the first row: the browser's own visual page-scale
zoom, centred on wherever your pointer is. Nothing is magnified outside the
application that receives the gesture.

Why it lands on the cursor: Wayland delivers a pinch through
`zwp_pointer_gestures_v1` as `begin` / `update` / `end`, and an update carries
**only a scale factor — no coordinates**. Blink applies the delta of that
scale to the current page scale (`Viewport::PinchUpdate`) using the pointer
location as the anchor. Since your mouse drives the pointer, the zoom centres
itself for free.

## Why mouse, not trackpad

If you have a trackpad, **it already does this** — libinput detects the pinch
and the compositor forwards it with no help from anyone. Injecting a synthetic
pinch in response to a real one would just double-apply the zoom.

So PinchKey's job is to give a *mouse* the gesture a trackpad already has.
The real touchpad is used for one thing only: as a **geometry template** for
the virtual pad (see below). Reading the pad for diagnostics is available via
`--monitor-touchpad`, and is strictly read-only — the pad is never grabbed, so
native gestures keep working.

---

## How it works

```
mouse wheel ──► [grabbed + mirrored] ──► trigger held?
                        │                     │ no → replayed as a normal scroll
                        │                     │ yes
                        ▼                     ▼
              everything else          PinchSynth: target scale ×= zoom_step
              replayed verbatim                  │
                                                 ▼
                                     125 Hz worker eases current → target
                                                 │
                                                 ▼
                                   virtual touchpad: 2 fingers at
                                   ±(start_half × scale) from centre
                                                 │
                                                 ▼
                              libinput → compositor → GESTURE_PINCH → Brave
```

**Why one continuous gesture.** Because Blink consumes scale *deltas*, a
natural zoom needs a single long-lived gesture whose scale tracks your input.
A burst of fixed-size gestures can only ever produce a fixed zoom quantum.
Releasing `Alt` lifts the virtual fingers and ends the gesture.

**Geometry is cloned, not guessed.** libinput derives physical size from
`range / resolution` and uses it to normalise motion and decide whether
gestures are viable at all, so the virtual pad copies a real pad's axes via
`EVIOCGABS`.

**Travel is bounded by the pad.** Starting at a 15 mm half-gap on a 121 mm pad
with a 5 mm edge margin, the fingers can reach ~55 mm — about **3.7× per
gesture**. Page scale accumulates *across* gestures, so release `Alt` and
scroll again to keep going. (An in-gesture re-grip would read as `end` +
`begin` and visibly re-anchor the zoom, which is why it is not done.)

---

## Install

```bash
sudo dnf install libevdev-devel gcc-c++ make     # Fedora
sudo apt install libevdev-dev build-essential    # Debian/Ubuntu

make
```

### Permissions

PinchKey needs to read `/dev/input/event*` and write `/dev/uinput`. Either run
it with `sudo`, or install the rules once:

```bash
sudo make install-udev
# log out and back in — group changes need a new session
```

> **Two udev rules are required.** `/dev/uinput` is `SUBSYSTEM=="misc"`, not
> `"input"`, and ships as `0600 root:root`. A rule matching only
> `KERNEL=="event*", SUBSYSTEM=="input"` never touches it. The rule also
> ensures the `uinput` module is loaded at boot, since the node does not exist
> until it is.

Check your setup at any time:

```bash
make check-env
```

### Run

```bash
./pinchkey              # foreground
./pinchkey --list       # show classified devices and their roles
```

### As a service

```bash
sudo make install
make install-service
systemctl --user daemon-reload
systemctl --user enable --now pinchkey.service
```

A **user** service, bound to `graphical-session.target`: PinchKey synthesises
input for a compositor, so it is useless before one exists and should die with
it. It handles `SIGTERM` by lifting any virtual fingers still down, releasing
every grab, and destroying its virtual devices, so restarts leave nothing
behind.

---

## Configuration

Copy `pinchkey.conf.example` to `~/.config/pinchkey/pinchkey.conf`. Every
tunable is runtime, not compile-time — gesture feel takes dozens of iterations
and none of them should need a rebuild.

| Key | Default | Effect |
|---|---|---|
| `trigger` | `alt_wheel` | `alt_wheel`, or `side_wheel` for a mouse side button |
| `zoom_step` | `1.08` | Scale multiplier per wheel notch. Higher → coarser, faster |
| `tick_ms` | `8` | Gesture frame interval (~125 Hz, like a real I²C pad) |
| `ease` | `0.25` | Smoothing per tick toward target. `1.0` snaps and feels jerky |
| `start_half_mm_in` | `15.0` | Starting half-gap when zooming in — sets the per-gesture ceiling |
| `start_half_mm_out` | `50.0` | Starting half-gap when zooming out |
| `edge_margin_mm` | `5.0` | Keep-out band so fingers never pin to the pad edge |
| `min_half_mm` | `4.0` | Floor, so the two contacts never merge into one |
| `idle_lift_ms` | `400` | Backstop lift if the modifier release is missed |
| `grab_mouse` | `true` | Required — without it you zoom *and* scroll |
| `grab_keyboard` | `false` | Cosmetic only; see below |
| `monitor_touchpad` | `false` | Log pad slots/distances, read-only |
| `log_level` | `info` | `error`, `warn`, `info`, `debug` |

**On `grab_keyboard`.** Keyboards are read *passively* by default, purely to
track whether `Alt` is held. Grabbing them would only hide the bare `Alt`
press from the compositor — which is cosmetic, since a held `Alt` does nothing
in GNOME on its own — and it is not worth putting PinchKey in the path of all
your typing, plus a mirror device per keyboard.

The mouse *is* grabbed, because the wheel has to be swallowed. Everything
PinchKey does not consume is replayed through a mirror device that clones the
original's capabilities, so acceleration and button mapping are unchanged.

---

## Verifying it works

Before debugging the daemon, confirm the compositor path end to end with the
standalone probe. It creates a virtual pad, sends **one** continuous pinch, and
grabs nothing:

```bash
make probe
sudo ./pinchprobe out 2.0 600     # direction, scale, duration-ms
```

Park the mouse over something in Brave during the 4-second countdown. If the
page zooms at the cursor, the mechanism is sound and any remaining problem is
in the daemon.

To watch what libinput makes of the virtual device:

```bash
sudo libinput debug-events | grep -i -e pinch -e PinchKey
```

You want the device classified as `TOUCHPAD` and `GESTURE_PINCH_BEGIN` /
`_UPDATE` / `_END` events.

---

## Troubleshooting

| Problem | Cause / fix |
|---|---|
| `Cannot open /dev/uinput` | `sudo make install-udev`, then start a new session |
| `No real touchpad found` | PinchKey needs one as a geometry template. On a desktop, see the note below |
| Zoom *and* scroll both happen | The mouse was not grabbed — check `grab_mouse` and for another process holding `EVIOCGRAB` |
| Nothing happens, no errors | Run the probe above. If the probe works and the daemon does not, the wheel device is not being grabbed — check `./pinchkey --list` |
| Zoom is jerky / steppy | Lower `zoom_step`, or lower `ease` for a longer glide |
| Zoom stops partway | Per-gesture travel limit. Release `Alt` and scroll again; or raise the ceiling by lowering `start_half_mm_in` |
| Alt+Tab stopped working | Only possible with `grab_keyboard = true`; set it back to `false` |
| GNOME shows a second touchpad in Settings | Expected — that is the virtual pad |

**Desktops with no touchpad** currently cannot run the daemon, because it
clones geometry from a real pad. Synthesising plausible geometry instead is a
known gap (see below).

---

## Project layout

```
src/
  main.cpp           CLI, device scan, wiring, clean shutdown
  config.{h,cpp}     runtime configuration
  uinput_pad.{h,cpp} geometry probe + virtual touchpad, MT Protocol B frames
  pinch_synth.{h,cpp}continuous gesture state machine (the core)
  input_router.{h,cpp} per-device loop: passive modifier tracking or grab+mirror
  mirror.{h,cpp}     uinput clone of a grabbed device
  modifiers.{h,cpp}  process-global trigger state
  evdev_device.{h,cpp} RAII libevdev wrapper, device classification
  touch_monitor.{h,cpp} read-only pad diagnostics
tests/test_logic.cpp config parsing, distance maths, modifier counting
tools/pinchprobe.cpp standalone feasibility probe
packaging/           udev rules, modules-load, systemd unit
```

```bash
make        # build
make test   # logic tests (no kernel devices needed)
make probe  # standalone probe
```

---

## Known gaps

- **No hotplug.** Devices are scanned once at startup, so a mouse unplugged
  and replugged stops working until restart. A service starting at boot may
  also race USB enumeration. Needs a udev monitor.
- **Requires a real touchpad** as a geometry template; desktops are unsupported.
- **Per-gesture zoom ceiling** set by pad geometry, as described above.
- **`side_wheel` trigger is parsed but untested.**
- Verified only on GNOME 50 / Wayland / Fedora 44. X11 delivers pinch through
  XI 2.4 and is untested.

## History

Originally a four-phase trackpad experiment: finger tracking → inter-finger
distances → gesture recognition → integration, as three near-identical
single-file prototypes. Those are preserved in git history at commit
`b35070a`. The restructure here fixed several things worth naming:

- Modifier state was per-device, so `Alt` (keyboard) and the wheel (mouse)
  never met and `Alt`+wheel could not fire.
- The real touchpad was grabbed but never mirrored, leaving it completely dead
  while PinchKey ran.
- The virtual pad's fallback geometry described a 38 × 29 mm pad, reachable
  through a startup race.
- `INPUT_PROP_BUTTONPAD` was declared without `BTN_LEFT`.
- The documented no-sudo path could not work, as the udev rule never matched
  `/dev/uinput`.
