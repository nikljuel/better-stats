#!/bin/sh

install_root=${BETTERSTATS_INSTALL_ROOT:-/mnt/ext1}
base="$install_root/applications/betterstats"
current_file="$base/current"
log_dir="$install_root/system/pbreadstats"
log="$log_dir/app.log"

mkdir -p "$log_dir"
if [ -f "$log" ] && [ "$(wc -c < "$log")" -gt 262144 ]; then
    mv -f "$log" "$log.1"
fi

note() {
    printf '%s Better Stats launcher: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$log"
}

if [ ! -r "$current_file" ]; then
    note "missing release pointer: $current_file"
    exit 1
fi
current=$(sed -n '1p' "$current_file")
case "$current" in
    ''|.*|*[!A-Za-z0-9._-]*) note "invalid release pointer"; exit 1 ;;
esac

release="$base/releases/$current"
soft="$release/betterstats-inkview-softfp"
hard="$release/betterstats-inkview-hardfp"
qt="$release/betterstats-qt-softfp"
abi=${BETTERSTATS_ABI:-auto}
loader_root=${BETTERSTATS_LOADER_ROOT:-}
if [ "$abi" = auto ]; then
    if [ -e "$loader_root/lib/ld-linux-armhf.so.3" ] \
       && [ ! -e "$loader_root/lib/ld-linux.so.3" ]; then
        abi=hardfp
    else
        abi=softfp
    fi
fi

case "$abi" in
    hardfp) core=$hard ;;
    softfp) core=$soft ;;
    *) note "unknown ABI: $abi"; exit 1 ;;
esac
if [ ! -x "$core" ]; then
    note "missing executable: $core"
    exit 1
fi

if [ "${1:-}" = --daemon ]; then
    exec "$core" "$@" >> "$log" 2>&1
fi

if ! "$core" --stop-daemon >> "$log" 2>&1; then
    note "could not flush running daemon before launch"
    exit 1
fi
"$core" --prepare >> "$log" 2>&1 || note "book autostart setup was not available"
sh "$install_root/applications/BetterStats.app" --daemon </dev/null >> "$log" 2>&1 &

if [ "$abi" = hardfp ] || [ ! -x "$qt" ]; then
    note "starting InkView ($abi)"
    exec "$core" >> "$log" 2>&1
fi

ready="${TMPDIR:-/tmp}/betterstats-ready.$$"
rm -f "$ready"
BETTERSTATS_READY_FILE="$ready" "$qt" >> "$log" 2>&1
status=$?
if [ -f "$ready" ]; then
    rm -f "$ready"
    exit "$status"
fi
rm -f "$ready"
note "Qt did not reach ready state (exit $status); falling back to InkView"
exec "$core" >> "$log" 2>&1
