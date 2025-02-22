################################################################################
# Variables
################################################################################

CXX ?= g++

# Default compiler flags
CXXFLAGS = -std=c++17 -Wall -Wextra

# If you run `make DEBUG=1`, we'll build with debugging info and logs.
ifeq ($(DEBUG),1)
  CXXFLAGS += -O0 -g -DDEBUG_LOGS
else
  CXXFLAGS += -O2
endif

# LWM source and binary
LWM_SRC   = lwm.cpp
LWM_BIN   = lwm

# Libraries needed by lwm
LWM_LIBS  = -lxcb -lxcb-icccm -lxcb-ewmh -lxcb-cursor -lxcb-keysyms -lX11 -lpthread

# Where to install the compiled binary and wrapper script
INSTALL_DIR = /usr/bin

################################################################################
# Install build dependencies
################################################################################
.PHONY: deps
deps:
	sudo apt-get update
	sudo apt-get install -y build-essential \
		libxcb1-dev libxcb-icccm4-dev libxcb-keysyms1-dev libxcb-ewmh-dev \
		libxcb-cursor-dev libx11-dev libvulkan-dev picom libcairo2-dev

################################################################################
# Compile lwm
################################################################################
$(LWM_BIN): $(LWM_SRC)
	$(CXX) $(CXXFLAGS) -o $(LWM_BIN) $(LWM_SRC) $(LWM_LIBS)

################################################################################
# Install lwm to $(INSTALL_DIR)
################################################################################
.PHONY: install
install: $(LWM_BIN)
	sudo install -v -m 755 $(LWM_BIN) $(INSTALL_DIR)/$(LWM_BIN)

################################################################################
# Install wrapper script to launch lwm
# (This wrapper starts lwm and launches xbindkeys in the background if installed.)
################################################################################
.PHONY: wrapper
wrapper:
	@echo "#!/bin/sh" | sudo tee $(INSTALL_DIR)/start-lwm > /dev/null
	@echo "if command -v xbindkeys >/dev/null 2>&1; then" | sudo tee -a $(INSTALL_DIR)/start-lwm > /dev/null
	@echo "    xbindkeys &" | sudo tee -a $(INSTALL_DIR)/start-lwm > /dev/null
	@echo "fi" | sudo tee -a $(INSTALL_DIR)/start-lwm > /dev/null
	@echo "exec /usr/bin/lwm" | sudo tee -a $(INSTALL_DIR)/start-lwm > /dev/null
	sudo chmod +x $(INSTALL_DIR)/start-lwm

################################################################################
# LightDM configuration
# Creates a .desktop file so that LightDM will start the X session using the
# start-lwm wrapper script.
################################################################################
.PHONY: lightdm
lightdm:
	@echo "[Desktop Entry]" | sudo tee /usr/share/xsessions/lwm.desktop > /dev/null
	@echo "Name=LWM" | sudo tee -a /usr/share/xsessions/lwm.desktop > /dev/null
	@echo "Comment=Lightweight WM" | sudo tee -a /usr/share/xsessions/lwm.desktop > /dev/null
	@echo "Exec=/usr/bin/start-lwm" | sudo tee -a /usr/share/xsessions/lwm.desktop > /dev/null
	@echo "Type=Application" | sudo tee -a /usr/share/xsessions/lwm.desktop > /dev/null

################################################################################
# DM target: full setup (install dependencies, compile, install, wrapper, and
# configure LightDM)
################################################################################
.PHONY: dm
dm: deps $(LWM_BIN) install wrapper lightdm

################################################################################
# Clean build artifacts and uninstall LightDM session file
################################################################################
.PHONY: clean
clean:
	rm -f $(LWM_BIN)
	sudo rm -f $(INSTALL_DIR)/$(LWM_BIN)
	sudo rm -f $(INSTALL_DIR)/start-lwm
	sudo rm -f /usr/share/xsessions/lwm.desktop

################################################################################
# Default target: show dependencies, install, wrapper, and LightDM config.
################################################################################
.PHONY: all
all:
	$(MAKE) deps
	$(MAKE) install
	$(MAKE) wrapper
	$(MAKE) lightdm
