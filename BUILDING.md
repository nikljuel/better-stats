# Building Better Stats

Better Stats ships three ARM ELFs behind one small dispatcher:

- `betterstats-qt-softfp` — Qt 6.8.2/QML UI for compatible firmware.
- `betterstats-inkview-softfp` — Qt-free fallback for normal ARM EABI devices.
- `betterstats-inkview-hardfp` — Qt-free hard-float build for PB1030/RK3566.

Qt is never bundled; each executable dynamically links the firmware libraries.
The builds run in Docker, so no host Qt or ARM toolchain is required.

## Prerequisites

- **Docker** (or OrbStack on macOS). The Qt6 builder image is pulled
  automatically on first `make qt`.
- **git**, **make**.
- Optional: **Python 3 + Pillow** only if you want to regenerate the icons.
- A C compiler and `libsqlite3` on the host for the unit tests (`make test`).

## One-time setup

```bash
make sdk
```

This shallow-clones [fstanis/pocketbook-sdk-qt6](https://github.com/fstanis/pocketbook-sdk-qt6)
into `third_party/` and sparse-checks-out only the four SDK files the build
links against (`inkview.h`, `hwconfig.h` and two stub libraries) — about 2 MB,
not the full 2.6 GB SDK submodule.

## Build

```bash
make qt       # Qt + InkView, soft-float
make hardfp   # InkView, hard-float
make package  # both builds + installable ZIP
```

The outputs are:

```
build-qt/betterstats-qt-softfp
build-qt/betterstats-inkview-softfp
build-hardfp/betterstats-inkview-hardfp
build-package/BetterStats-<version>.zip
```

`make qt` uses:

```
docker run --rm -v "$PWD:/src" -w /src \
  ghcr.io/fstanis/pocketbook-sdk-qt6-builder \
  bash -c 'cmake -B build-qt -DCMAKE_TOOLCHAIN_FILE=.../pocketbook.toolchain.cmake \
           && cmake --build build-qt -j8'
```

`make hardfp` builds `tools/Dockerfile.hardfp` locally and uses Debian's
`arm-linux-gnueabihf` cross compiler. Both InkView variants use the same C
sources. Current outputs require glibc 2.34 or newer; an older firmware toolchain
should be added only if testing finds a supported device below that baseline.

## Package layout and updates

The ZIP extracts directly onto the reader:

```
applications/BetterStats.app
applications/betterstats/current
applications/betterstats/activate-release
applications/betterstats/releases/<version>/
  betterstats-qt-softfp
  betterstats-inkview-softfp
  betterstats-inkview-hardfp
  manifest
  SHA256SUMS
```

The on-device updater reads the latest stable GitHub release and expects an
asset named exactly `BetterStats-<tag>.zip`. It verifies the asset size and,
when available, its GitHub SHA-256 digest, stages the entire new release
directory, validates the bundled `SHA256SUMS`, then runs
`activate-release <version>`. The helper rechecks the release name, manifest and
all three executables, then atomically renames a temporary `current` file. The
previous release remains available if validation fails or power is lost before
that final rename.

## Deploy (development)

With the reader mounted over USB:

```bash
make deploy          # copies the complete bundle to $(DEVICE)
```

Adjust `DEVICE` at the top of the `Makefile` to your reader's mount point
(default `/Volumes/PB710`).

## Tests

```bash
make test
```

Builds and runs the tracker, file-handler parser and Autostart preference,
shared statistics model, updater release parser/settings and
dispatcher/activation checks on the host. No device is needed.

## Icons

The launcher icons are checked in under `qt/qml/` (embedded into the binary as a
Qt resource). The focused variant uses a filled disc instead of a filled
rectangle, matching the firmware's round highlight. To regenerate them from
`tools/make_icon.py`:

```bash
pip install Pillow   # or: uv run --with pillow make icons
make icons
```

## Project layout

```
src/            shared C core: tracker, statistics model, covers, daemon, setup
inkview/        Qt-free InkView UI and hard-float link stub
qt/src/         thin Qt/QML adapters and icon installer
qt/qml/         the UI (com.pocketbook.controls) + Tr.qml (i18n) + icons
qt/third_party/ vendored sqlite3 + miniz (source only)
third_party/    pocketbook-sdk-qt6 (fetched by `make sdk`, git-ignored)
test/           host-side unit tests
packaging/      runtime dispatcher and atomic release activator
tools/          icon generator and hard-float builder image
```

## How the pieces fit

- `src/*.c` is plain C shared by both UIs. It derives sessions, aggregates every
  tab, extracts book covers, configures autostart, updates releases and runs the
  daemon.
- `qt/src/main.cpp` boots Qt and creates a ready marker after the QML root loads.
  `stats_bridge.cpp` exposes the shared statistics and updater to QML.
- `inkview/main.c` renders the same four tabs and settings directly through
  InkView and also supplies the daemon entry point for both ABIs.
- `packaging/BetterStats.app` selects the ABI, starts tracking, attempts Qt and
  falls back to InkView when no ready marker appears.
- `qt/qml/` is the UI. Text goes through the `Tr` singleton for DE/EN; all
  spacing/colors come from the firmware's `GlobalValues`.

See the toolchain notes in
[fstanis/pocketbook-sdk-qt6](https://github.com/fstanis/pocketbook-sdk-qt6) for
the Qt-side constraints (`rcc --no-zstd`, soft-float ABI and exact Qt version).
