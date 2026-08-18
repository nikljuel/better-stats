CFLAGS   := -O2 -Wall -Wextra -std=gnu99

# Cross-compiler image with the on-device Qt 6.8.2 headers and InkView stubs.
QT_IMG   := ghcr.io/fstanis/pocketbook-sdk-qt6-builder
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

# ---- Build the app (single ELF, links the device's Qt at runtime) ----
qt:
	docker run --rm -v "$(CURDIR):/src" -w /src $(QT_IMG) bash -c '\
	  cmake -B build-qt -DCMAKE_TOOLCHAIN_FILE=/src/third_party/pocketbook-sdk-qt6/cmake/pocketbook.toolchain.cmake \
	  && cmake --build build-qt -j8'
	@echo "Built build-qt/BetterStats.app"

# ---- Dev deploy to a USB-mounted reader ----
deploy:
	cp build-qt/BetterStats.app "$(DEVICE)/applications/BetterStats.app"
	rm -f "$(DEVICE)/applications/._BetterStats.app"
	sync

# ---- Regenerate the launcher icons (needs Pillow) ----
icons:
	python3 tools/make_icon.py qt/qml

clean:
	rm -rf build build-qt

.PHONY: sdk test qt deploy icons clean
