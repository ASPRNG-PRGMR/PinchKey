# Makefile for zoom_listener (C++17)
# Fedora: sudo dnf install libevdev-devel gcc-c++ make

CXX     = g++
TARGET  = exec_PinchKey
SRC     = PinchKey_phase3.cpp # Change this to compile other phases 

CXXFLAGS = -std=c++17 -Wall -Wextra -O2 $(shell pkg-config --cflags libevdev)
LDFLAGS  = $(shell pkg-config --libs libevdev) -lpthread

.PHONY: all clean install-udev

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)
	@echo ""
	@echo "Build OK — run with:  sudo ./$(TARGET)"
	@echo "Or (no sudo needed after group change):"
	@echo "  sudo usermod -aG input \$$USER && newgrp input"

clean:
	rm -f $(TARGET)

# Install a udev rule so the binary can run without sudo
install-udev:
	@echo 'KERNEL=="event*", SUBSYSTEM=="input", GROUP="input", MODE="0660"' \
	    | sudo tee /etc/udev/rules.d/99-input-group.rules
	@sudo udevadm control --reload-rules
	@sudo udevadm trigger
	@sudo usermod -aG input $$USER
	@echo ""
	@echo "Udev rule installed. Log out and back in for group change to take effect."
