#!/bin/sh
set -eu

tmp=$(mktemp -d "${TMPDIR:-/tmp}/betterstats-launcher.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
release="$tmp/applications/betterstats/releases/v1"
calls="$tmp/calls"
mkdir -p "$release" "$tmp/system/pbreadstats"
printf 'v1\n' > "$tmp/applications/betterstats/current"
cp packaging/BetterStats.app "$tmp/applications/BetterStats.app"
chmod +x "$tmp/applications/BetterStats.app"

for name in betterstats-inkview-softfp betterstats-inkview-hardfp; do
    label=${name##*-}
    sed "s/@LABEL@/$label/" > "$release/$name" <<'SCRIPT'
#!/bin/sh
printf '%s:%s\n' '@LABEL@' "${1:-}" >> "$BETTERSTATS_TEST_CALLS"
SCRIPT
    chmod +x "$release/$name"
done
cat > "$release/betterstats-qt-softfp" <<'SCRIPT'
#!/bin/sh
printf 'qt\n' >> "$BETTERSTATS_TEST_CALLS"
if [ "${BETTERSTATS_QT_TEST:-fail}" = ready ]; then
    : > "$BETTERSTATS_READY_FILE"
    exit 0
fi
exit 127
SCRIPT
chmod +x "$release/betterstats-qt-softfp"
printf 'version=v1\n' > "$release/manifest"
cp packaging/activate-release "$tmp/applications/betterstats/activate-release"
chmod +x "$tmp/applications/betterstats/activate-release"
BETTERSTATS_BASE="$tmp/applications/betterstats" \
    sh "$tmp/applications/betterstats/activate-release" v1 >/dev/null
if BETTERSTATS_BASE="$tmp/applications/betterstats" \
    sh "$tmp/applications/betterstats/activate-release" ../escape >/dev/null 2>&1; then
    echo "invalid release was activated" >&2
    exit 1
fi
if BETTERSTATS_BASE="$tmp/applications/betterstats" \
    sh "$tmp/applications/betterstats/activate-release" .. >/dev/null 2>&1; then
    echo "dot release was activated" >&2
    exit 1
fi
printf 'version=v2\n' > "$release/manifest"
if BETTERSTATS_BASE="$tmp/applications/betterstats" \
    sh "$tmp/applications/betterstats/activate-release" v1 >/dev/null 2>&1; then
    echo "mismatched manifest was activated" >&2
    exit 1
fi
printf 'version=v1\n' > "$release/manifest"

run_launcher() {
    status=0
    BETTERSTATS_INSTALL_ROOT="$tmp" BETTERSTATS_TEST_CALLS="$calls" \
        sh "$tmp/applications/BetterStats.app" "$@" || status=$?
    sleep 1
    return "$status"
}

: > "$calls"
BETTERSTATS_QT_TEST=ready run_launcher
grep -q '^qt$' "$calls"
! grep -q '^softfp:$' "$calls"

: > "$calls"
BETTERSTATS_QT_TEST=fail run_launcher
grep -q '^qt$' "$calls"
grep -q '^softfp:$' "$calls"

: > "$calls"
mkdir -p "$tmp/loaders/lib"
: > "$tmp/loaders/lib/ld-linux-armhf.so.3"
BETTERSTATS_ABI=auto BETTERSTATS_LOADER_ROOT="$tmp/loaders" run_launcher
grep -q '^hardfp:$' "$calls"
! grep -q '^qt$' "$calls"

: > "$calls"
BETTERSTATS_ABI=hardfp run_launcher --daemon
grep -q '^hardfp:--daemon$' "$calls"

printf '../escape\n' > "$tmp/applications/betterstats/current"
if run_launcher; then
    echo "invalid release pointer was accepted" >&2
    exit 1
fi

printf '..\n' > "$tmp/applications/betterstats/current"
if run_launcher; then
    echo "dot release pointer was accepted" >&2
    exit 1
fi

echo "all launcher tests ok"
