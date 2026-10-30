# PinchKey — mouse-driven native pinch zoom (C++17)
# Fedora:  sudo dnf install libevdev-devel gcc-c++ make
# Debian:  sudo apt install libevdev-dev build-essential

CXX      ?= g++
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

TARGET    = pinchkey
PROBE     = pinchprobe
BUILD     = build

CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic $(shell pkg-config --cflags libevdev)
LDLIBS    = $(shell pkg-config --libs libevdev) -lpthread

SRCS = $(wildcard src/*.cpp)
OBJS = $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

.PHONY: all clean probe install uninstall install-udev uninstall-udev \
        install-service uninstall-service check-env

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
	@echo
	@echo "Built ./$(TARGET)"
	@echo "Run:  sudo ./$(TARGET)            (or 'make install-udev' for no sudo)"

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	@mkdir -p $(BUILD)

-include $(DEPS)

# Standalone feasibility probe: creates a virtual pad, sends one continuous
# pinch, grabs nothing. Use it to verify the compositor path end to end.
probe: $(PROBE)

$(PROBE): tools/pinchprobe.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)
	@echo "Built ./$(PROBE) — try: sudo ./$(PROBE) out 2.0 600"

clean:
	rm -rf $(BUILD) $(TARGET) $(PROBE)

install: $(TARGET)
	install -Dm0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm0644 pinchkey.conf.example $(DESTDIR)/etc/pinchkey.conf.example
	@echo "Installed to $(DESTDIR)$(BINDIR)/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

# ── Permissions ─────────────────────────────────────────────────────────────
# Two rules are required, and the second is the one the old Makefile missed:
# /dev/uinput is SUBSYSTEM=="misc", not "input", so an event*-only rule never
# matched it and the documented no-sudo path could not have worked.
install-udev:
	install -Dm0644 packaging/99-pinchkey.rules /etc/udev/rules.d/99-pinchkey.rules
	install -Dm0644 packaging/uinput.conf /etc/modules-load.d/uinput.conf
	modprobe uinput || true
	udevadm control --reload-rules
	udevadm trigger --subsystem-match=input --subsystem-match=misc
	@if ! id -nG "$(SUDO_USER)" 2>/dev/null | grep -qw input; then \
	    usermod -aG input "$(SUDO_USER)" && \
	    echo "Added $(SUDO_USER) to the 'input' group."; \
	fi
	@echo
	@echo "Done. Log out and back in (group changes need a new session),"
	@echo "then verify:  ls -l /dev/uinput   # expect  crw-rw---- root input"

uninstall-udev:
	rm -f /etc/udev/rules.d/99-pinchkey.rules /etc/modules-load.d/uinput.conf
	udevadm control --reload-rules

# ── systemd user service ────────────────────────────────────────────────────
# A user service, not a system one: PinchKey needs no root once the udev rule
# is in place, and it should follow the graphical session's lifetime.
install-service: install
	install -Dm0644 packaging/pinchkey.service \
	    $(DESTDIR)$(HOME)/.config/systemd/user/pinchkey.service
	@echo "Enable with:  systemctl --user daemon-reload && \\"
	@echo "              systemctl --user enable --now pinchkey.service"

uninstall-service:
	-systemctl --user disable --now pinchkey.service
	rm -f $(HOME)/.config/systemd/user/pinchkey.service
	systemctl --user daemon-reload

# ── Diagnostics ─────────────────────────────────────────────────────────────
check-env:
	@echo "session type : $${XDG_SESSION_TYPE:-unknown}"
	@echo "desktop      : $${XDG_CURRENT_DESKTOP:-unknown}"
	@printf "uinput node  : "; ls -l /dev/uinput 2>/dev/null || echo "MISSING (modprobe uinput)"
	@printf "in input grp : "; id -nG | grep -qw input && echo yes || echo "no (make install-udev)"
	@printf "libevdev     : "; pkg-config --modversion libevdev 2>/dev/null || echo "MISSING"
	@echo
	@echo "Classified devices:"
	@./$(TARGET) --list 2>/dev/null || echo "  (build first: make)"

# ── Tests ───────────────────────────────────────────────────────────────────
# Covers only what runs without kernel devices: config parsing, distance
# maths, modifier counting. Gesture synthesis needs /dev/uinput, so use
# `make probe` for that.
TEST_BIN  = $(BUILD)/test_logic
TEST_OBJS = $(BUILD)/config.o $(BUILD)/log.o $(BUILD)/modifiers.o $(BUILD)/touch_monitor.o $(BUILD)/evdev_device.o

.PHONY: test
test: $(TEST_BIN)
	@./$(TEST_BIN)

$(TEST_BIN): tests/test_logic.cpp $(TEST_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $< $(TEST_OBJS) $(LDLIBS)
