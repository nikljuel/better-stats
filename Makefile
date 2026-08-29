CFLAGS   := -O2 -Wall -Wextra -std=gnu99

# Cross-compiler image with the on-device Qt 6.8.2 headers and InkView stubs.
QT_IMG   := ghcr.io/fstanis/pocketbook-sdk-qt6-builder
HARD_FP_IMG := betterstats-hardfp-builder
INKVIEW_INCLUDE := third_party/pocketbook-sdk-qt6/sdk/SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot/usr/local/include
INKVIEW_SOURCES := inkview/main.c src/tracker.c src/stats_db.c src/stats_model.c \
	  src/daemon.c src/paths.c src/file_handler_config.c src/autostart.c src/updater.c \
	  src/sha256.c qt/third_party/sqlite3.c qt/third_party/miniz.c
PACKAGE_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
PACKAGE_ROOT := build-package/BetterStats
PACKAGE_RELEASE := $(PACKAGE_ROOT)/applications/betterstats/releases/$(PACKAGE_VERSION)
# Device mount point for the dev deploy target (adjust to your reader's volume).
DEVICE   := /Volumes/PB710

# ---- One-time setup: fetch the PocketBook Qt6 SDK (sparse, ~2 MB) ----
# Clones fstanis/pocketbook-sdk-qt6 and its SDK submodule, checking out only the
# four files the build needs (inkview.h, hwconfig.h and two stub libs).
sdk:
	@test -d third_party/pocketbook-sdk-qt6 || \
	  git clone --depth 1 https://github.com/fstanis/pocketbook-sdk-qt6 \
	    third_party/pocketbook-sdk-qt6
	cd third_party/pocketbook-sdk-qt6 && \
	  git submodule update --init --depth 1 --filter=blob:none sdk && \
	  git -C sdk sparse-checkout set \
	    SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot/usr/local

# ---- Host tests: tracker session logic (no device needed) ----
test:
	mkdir -p build
	cc $(CFLAGS) -o build/test_tracker test/test_tracker.c \
	  src/tracker.c src/stats_db.c -lsqlite3
	./build/test_tracker
	cc $(CFLAGS) -Isrc -o build/test_file_handler_config \
	  test/test_file_handler_config.c src/file_handler_config.c
	./build/test_file_handler_config
	cc $(CFLAGS) -DSTATS_DIR='"/tmp/bs_model_cache"' \
	  -DCOVER_DIR='"/tmp/bs_model_firmware_covers"' \
	  -Isrc -Iqt/third_party -o build/test_stats_model \
	  test/test_stats_model.c src/stats_model.c src/tracker.c src/stats_db.c \
	  qt/third_party/miniz.c -lsqlite3
	./build/test_stats_model
	cc $(CFLAGS) -Isrc -o build/test_sha256 \
	  test/test_sha256.c src/sha256.c
	./build/test_sha256
	cc $(CFLAGS) -rdynamic -DSTATS_DIR='"/tmp/bs_update_test"' \
	  -Isrc -Iqt/third_party -o build/test_updater \
	  test/test_updater.c src/updater.c src/sha256.c qt/third_party/miniz.c -lsqlite3 -ldl
	./build/test_updater
	sh test/test_launcher.sh

# ---- Build the app (single ELF, links the device's Qt at runtime) ----
qt:
	docker run --rm -v "$(CURDIR):/src" -w /src $(QT_IMG) bash -c '\
	  cmake -B build-qt -DCMAKE_TOOLCHAIN_FILE=/src/third_party/pocketbook-sdk-qt6/cmake/pocketbook.toolchain.cmake \
	  && cmake --build build-qt -j8'
	@echo "Built build-qt/betterstats-qt-softfp and build-qt/betterstats-inkview-softfp"

# ---- Qt-free InkView build for hard-float PocketBooks (PB1030/RK3566) ----
hardfp:
	docker build -q -t $(HARD_FP_IMG) -f tools/Dockerfile.hardfp .
	mkdir -p build-hardfp
	docker run --rm -v "$(CURDIR):/src" -w /src $(HARD_FP_IMG) sh -c '\
	  arm-linux-gnueabihf-gcc -fPIC -shared -Wl,-soname,libinkview.so \
	    inkview/hardfp_stubs.c -o build-hardfp/libinkview.so \
	  && arm-linux-gnueabihf-gcc $(CFLAGS) -DPLATFORM_FC \
	    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 \
	    -march=armv7-a -mfpu=neon -mfloat-abi=hard \
	    -I$(INKVIEW_INCLUDE) -Isrc -Iqt/third_party $(INKVIEW_SOURCES) \
	    -Lbuild-hardfp -Wl,--no-as-needed -linkview -Wl,--as-needed \
	    -lm -lpthread -ldl -o build-hardfp/betterstats-inkview-hardfp'
	@echo "Built build-hardfp/betterstats-inkview-hardfp"

# ---- Installable bundle: launcher + one atomic release directory ----
package: qt hardfp
	@case "$(PACKAGE_VERSION)" in ''|.*|*[!A-Za-z0-9._-]*) \
	  echo "Invalid PACKAGE_VERSION: $(PACKAGE_VERSION)" >&2; exit 1;; esac
	rm -rf "$(PACKAGE_ROOT)"
	rm -f "build-package/BetterStats-$(PACKAGE_VERSION).zip"
	mkdir -p "$(PACKAGE_RELEASE)"
	cp packaging/BetterStats.app "$(PACKAGE_ROOT)/applications/BetterStats.app"
	cp build-qt/betterstats-qt-softfp build-qt/betterstats-inkview-softfp \
	  build-hardfp/betterstats-inkview-hardfp "$(PACKAGE_RELEASE)/"
	chmod +x "$(PACKAGE_ROOT)/applications/BetterStats.app" "$(PACKAGE_RELEASE)"/*
	cp packaging/activate-release "$(PACKAGE_ROOT)/applications/betterstats/activate-release"
	chmod +x "$(PACKAGE_ROOT)/applications/betterstats/activate-release"
	printf '%s\n' "$(PACKAGE_VERSION)" > "$(PACKAGE_ROOT)/applications/betterstats/current"
	printf 'version=%s\n' "$(PACKAGE_VERSION)" > "$(PACKAGE_RELEASE)/manifest"
	cd "$(PACKAGE_RELEASE)" && shasum -a 256 betterstats-* > SHA256SUMS
	cp packaging/INSTALL.txt "$(PACKAGE_ROOT)/INSTALL.txt"
	cd "$(PACKAGE_ROOT)" && zip -qr "../BetterStats-$(PACKAGE_VERSION).zip" INSTALL.txt applications
	@echo "Built build-package/BetterStats-$(PACKAGE_VERSION).zip"

# ---- Dev deploy to a USB-mounted reader ----
deploy: package
	cp "$(PACKAGE_ROOT)/applications/BetterStats.app" "$(DEVICE)/applications/BetterStats.app"
	mkdir -p "$(DEVICE)/applications/betterstats/releases"
	cp -R "$(PACKAGE_ROOT)/applications/betterstats/releases/$(PACKAGE_VERSION)" \
	  "$(DEVICE)/applications/betterstats/releases/"
	cp "$(PACKAGE_ROOT)/applications/betterstats/current" \
	  "$(DEVICE)/applications/betterstats/current"
	cp "$(PACKAGE_ROOT)/applications/betterstats/activate-release" \
	  "$(DEVICE)/applications/betterstats/activate-release"
	rm -f "$(DEVICE)/applications/._BetterStats.app"
	sync

# ---- Regenerate the launcher icons (needs Pillow) ----
icons:
	python3 tools/make_icon.py qt/qml

clean:
	rm -rf build build-qt build-hardfp build-package

.PHONY: sdk test qt hardfp package deploy icons clean
