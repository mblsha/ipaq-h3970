BUILD_DIR ?= $(CURDIR)/.build
IWAD ?=

.PHONY: host web sources kernel kit kit-from-kernel

host:
	host/build_activesync.sh "$(BUILD_DIR)/h3970-activesync"
	python3 wince/build_transfers.py "$(BUILD_DIR)/h3970-wince-transfer"
	python3 wince/build_today.py "$(BUILD_DIR)/h3970-wince-today"

web:
	web/build.sh "$(BUILD_DIR)/web"

sources:
	python3 build/fetch.py "$(BUILD_DIR)/sources"

kernel: sources
	linux/build.sh "$(BUILD_DIR)/sources" "$(BUILD_DIR)/kernel"

kit:
	@test -n "$(IWAD)" || { printf '%s\n' 'Usage: make kit IWAD=/path/to/doom1.wad' >&2; exit 2; }
	$(MAKE) kernel BUILD_DIR="$(BUILD_DIR)"
	build/kit.sh "$(BUILD_DIR)/sources" "$(BUILD_DIR)/kernel" "$(IWAD)" "$(BUILD_DIR)/kit"

kit-from-kernel: sources
	build/kit.sh "$(BUILD_DIR)/sources" "$(BUILD_DIR)/kernel" "$(IWAD)" "$(BUILD_DIR)/kit"
