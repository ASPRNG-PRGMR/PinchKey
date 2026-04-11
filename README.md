# PinchKey

A four-phase project to bring native pinch-to-zoom to **Fedora Workstation** using the built-in trackpad — no GNOME Magnifier, no `Ctrl+scroll`, pure touchpad gesture injection via `libevdev` + `uinput`.

---

## Project Phases

Each phase incrementally builds on the previous one.

```
Input Events → Phase 1 (Tracking) → Phase 2 (Distance) → Phase 3 (Gesture Engine) → uinput Injection
```

All processing is frame-synchronous with `SYN_REPORT`, ensuring minimal latency.

---

### Phase 1 — Detect finger positions on the trackpad

The trackpad speaks **MT Protocol B** (`libevdev`). Each finger occupies a numbered *slot*; the driver emits `ABS_MT_POSITION_X` / `ABS_MT_POSITION_Y` per slot on every frame, then closes the frame with `SYN_REPORT`.

`scanAndAttach()` probes every `/dev/input/event*` node at startup, classifies each device (touchpad vs keyboard/mouse vs ignore), and spawns a dedicated `pthread` per device. The touchpad thread updates a `TouchpadState` struct holding the latest $(x, y)$ for each active slot.

**Sample output**
```
[13938500ms][event10] SYNC  active=1  [0](26,0)
[13938635ms][event10] SYNC  active=2  [0](26,0) [1](480,276)
[13938642ms][event10] SYNC  active=3  [0](26,0) [1](480,276) [2](900,518)
```

On the test hardware the top-left corner of the pad maps to $(26,\;0)$ and the bottom-right to approximately $(1190,\;676)$ in raw kernel units.

---

### Phase 2 — Compute distances between adjacent fingers

Raw positions are no longer printed. Instead, on every `SYN_REPORT` the code:

1. Collects all active finger coordinates.
2. Sorts them **left-to-right** by the $x$ coordinate.
3. Computes the **Euclidean distance** between each adjacent pair.

For $N$ active fingers sorted as $f_0, f_1, \ldots, f_{N-1}$ (by $x$):

$$d_i = \sqrt{(x_{i+1} - x_i)^2 + (y_{i+1} - y_i)^2}, \qquad i = 0,\,1,\,\ldots,\,N-2$$

With two fingers this yields one distance; with three fingers, two distances (left↔middle, middle↔right).

**Sample output**
```
[13939018ms][event10] SYNC  active=2  dist[0->1]=342.1
[13939032ms][event10] SYNC  active=3  dist[0->1]=289.6  dist[1->2]=301.2
```

A spreading gesture produces an increasing $d_0$; a closing gesture produces a decreasing one.

---

### Phase 3 — Gesture recognition from live distance stream *(current phase)*

This is the active development branch (`PinchKey_phase3.cpp`).

The `GestureEngine` converts the Phase 2 distance stream into `emitPinch()` calls. Behavior is controlled via tunable thresholds (see [Tuning parameters](#tuning-parameters) below).

#### Mean distance

With $N-1$ adjacent pairs the engine tracks the **arithmetic mean**:

$$\bar{D} = \frac{1}{N-1}\sum_{i=0}^{N-2} d_i$$

Using the mean instead of a single pair makes the gesture robust to three or more fingers and to minor slot re-ordering.

#### Decision rule

Let **D̄_t** be the mean distance at frame *t* and **D̄_(t−1)** the previous frame.
The signed delta is:

$$\Delta D = \bar{D}_t - \bar{D}_{t-1}$$

A pinch is fired when **both** conditions hold:

$$|\Delta D| > \tau \quad \text{and} \quad t - t_{\text{last}} > C$$

where $\tau = 30$ raw units/frame (`PINCH_DIST_THRESHOLD`) and $C = 350\,\text{ms}$ (`PINCH_COOLDOWN_MS`).

| Condition | Direction | Action |
|---|---|---|
| $\Delta D > +\tau$ | fingers spreading | `emitPinch(+1)` — zoom in |
| $\Delta D < -\tau$ | fingers converging | `emitPinch(-1)` — zoom out |

#### State machine

```
IDLE  (< 2 fingers)
  ↓  second finger touches down
ACTIVE
  frame 0  →  record D_prev, no action
  frame k  →  compute ΔD
               |ΔD| > τ AND cooldown OK  →  fire, reset cooldown timer
               always update D_prev ← D_now
  ↓  finger count drops below 2
IDLE  (D_prev reset to −1)
```

**Sample output**
```
[13941020ms][event10] SYNC  active=2  dist[0->1]=342.1
[13941027ms][event10] SYNC  active=2  dist[0->1]=387.4
[13941034ms][event10] GESTURE pinch-OUT (zoom-in)  |delta| > 30
[13941037ms][event10] SYNC  active=2  dist[0->1]=431.8
```

#### Tuning parameters

| Constant | Default | Effect |
|---|---|---|
| `PINCH_DIST_THRESHOLD` $(\tau)$ | `30.0f` | Higher → less sensitive, fewer false positives |
| `PINCH_COOLDOWN_MS` $(C)$ | `350` | Higher → slower repeat rate |
| `PINCH_FRAMES` | `20` | More frames → smoother / slower synthetic gesture |
| `PINCH_FRAME_MS` | `16` | Frame interval of the injected MT sequence |
| `PINCH_START_FRAC` | `0.15` | Initial virtual finger gap as fraction of pad width |
| `PINCH_TRAVEL_FRAC` | `0.20` | Total virtual finger travel as fraction of pad width |

#### Keyboard fallback (still active)

The `ShortcutEngine` lets you test pinch injection without touching the trackpad:

| Shortcut | Action |
|---|---|
| `ALT` + `+` (or numpad `+`) | `emitPinch(+1)` — zoom in |
| `ALT` + `-` (or numpad `-`) | `emitPinch(-1)` — zoom out |
| `ALT` + Scroll Wheel Up | `emitPinch(+1)` — zoom in |
| `ALT` + Scroll Wheel Down | `emitPinch(-1)` — zoom out |

ALT is buffered and forwarded to the mirror device only when the next key is **not** one of the shortcuts above, so `Alt+Tab`, `Alt+F4`, and other standard combos are unaffected.

---

### Phase 4 — Full integration on Fedora Workstation 43 *(planned)*

Phase 4 will harden the Phase 3 implementation for production use on GNOME + Wayland. The key architectural shift is moving from **discrete gesture triggering** (fire once per threshold crossing) to **continuous gesture synthesis** (magnitude scaled proportionally to $|\Delta D|$), giving a far more natural feel.

**Planned work**

On Wayland, Mutter grabs the touchpad early. `EVIOCGRAB` from a process with the `input` group (via the udev rule below) is sufficient to intercept events before Mutter processes them; the synthetic `uinput` events then reach the compositor through the standard libinput path.

Additional items:

- **Systemd unit** — run PinchKey as a user service (`~/.config/systemd/user/pinchkey.service`) so it starts automatically on login without requiring `sudo`.
- **Velocity scaling** — scale the injected pinch magnitude proportionally to $|\Delta D|$ rather than firing a fixed-size gesture, giving a more natural feel. The scaling factor $\alpha$ would be:

$$\text{PINCH\_TRAVEL\_FRAC}_{\text{effective}} = \alpha \cdot \frac{|\Delta D|}{W_x}$$

where $W_x = x_{\max} - x_{\min}$ is the full pad X-range.

- **Multi-finger support** — distinguish two-finger and three-finger pinch; the mean $\bar{D}$ formula already handles $N$ fingers gracefully.
- **libinput debug verification** — confirm with `libinput debug-events` that the virtual touchpad is classified as `TOUCHPAD` and that gestures appear as `GESTURE_PINCH_*` events.

---

## Prerequisites

```bash
# Fedora
sudo dnf install libevdev libevdev-devel gcc-c++ make

#Ubuntu
sudo apt install libevdev-dev build-essential
```

---

## Build

```bash
make
```

Compiles `PinchKey.cpp` (C++17) via `g++` with `pkg-config` for `libevdev`.

---

## Run

### Option A — sudo (quickest)

```bash
sudo ./PinchKey
```

### Option B — no sudo (permanent, recommended)

```bash
make install-udev
# Log out and back in, or: newgrp input
./PinchKey
```

The udev rule grants the `input` group read/write access to all `/dev/input/event*` nodes and `/dev/uinput`.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Permission denied` on `/dev/input/eventN` | `sudo ./PinchKey` or `make install-udev` |
| `Cannot open /dev/uinput` | Same — needs `sudo` or `input` group |
| `pkg-config: libevdev not found` | `sudo dnf install libevdev-devel` |
| No devices attached | `ls -l /dev/input/event*` and check group membership |
| Distances always zero | Two fingers must be down simultaneously |
| Gesture fires but app ignores it | `libinput debug-events` — check device is classified as `TOUCHPAD` |
| Too many spurious zooms | Raise `PINCH_DIST_THRESHOLD` |
| Gesture feels sluggish / unresponsive | Lower `PINCH_DIST_THRESHOLD` or `PINCH_COOLDOWN_MS` |
| GNOME grabs trackpad (Wayland) | Run from a TTY or use `sudo` |

---

## File overview

| File | Purpose |
|---|---|
| `PinchKey_phase1.cpp` | Phase 1 |
| `PinchKey_phase2.cpp` | Phase 2 |
| `PinchKey_phase3.cpp` | Phase 3 (active) |
| `Makefile` | Build + udev-rule installation |

---

## Phase Summary

- **Phase 1** is the **core phase**, where raw finger positions are detected and tracked from the touchpad.
- **Phase 2** builds on Phase 1 by computing distances between fingers using the Euclidean distance formula.
- **Phase 3** extends both Phase 1 and Phase 2 by adding gesture recognition along with a keyboard shortcut fallback for triggering pinch actions.
