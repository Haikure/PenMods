#!/bin/sh
# Install only the runDictPen hook. Install the native helper separately via
# scratch-file + rename; never overwrite an executing helper in place.
set -eu
prefix=
if [ "${1:-}" = "--rootfs-prefix" ]; then
    [ "$#" -eq 2 ] || { echo "usage: $0 [--rootfs-prefix DIR]" >&2; exit 1; }
    prefix=$2
    case "$prefix" in /*) ;; *) echo "prefix must be absolute" >&2; exit 1 ;; esac
elif [ "$#" -ne 0 ]; then
    echo "usage: $0 [--rootfs-prefix DIR]" >&2; exit 1
fi
launcher=$prefix/usr/bin/runDictPen
helper=$prefix/userdata/PenMods/penmods-update
backup=$launcher.pre-penmods-update
[ -f "$launcher" ] && [ ! -L "$launcher" ] || { echo "runDictPen must be a regular file" >&2; exit 1; }
[ -x "$helper" ] || { echo "install executable /userdata/PenMods/penmods-update first" >&2; exit 1; }
original='stdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1'
replacement='stdbuf -o0 /userdata/PenMods/penmods-update run -- ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1'
counts=$(awk -v before="$original" -v after="$replacement" '
    $0 == before { original++ }
    $0 == after { replacement++ }
    END { printf "%d %d", original, replacement }
' "$launcher")
case "$counts" in
    '0 1') echo "PenMods launcher already installed"; exit 0 ;;
    '1 0') ;;
    *) echo "unrecognized or ambiguous runDictPen layout; refusing to modify" >&2; exit 1 ;;
esac
if [ -e "$backup" ] || [ -L "$backup" ]; then
    [ -f "$backup" ] && [ ! -L "$backup" ] || { echo "invalid existing launcher backup" >&2; exit 1; }
    cmp -s "$launcher" "$backup" || { echo "launcher differs from existing backup" >&2; exit 1; }
else
    cp -p "$launcher" "$backup"
    sync
fi
temporary=$launcher.penmods-update.$$
[ ! -e "$temporary" ] && [ ! -L "$temporary" ] || { echo "temporary launcher already exists" >&2; exit 1; }
trap 'rm -f "$temporary"' EXIT HUP INT TERM
cp -p "$launcher" "$temporary"
awk -v before="$original" -v after="$replacement" '{ if ($0 == before) print after; else print }' "$launcher" > "$temporary"
sync
mv -f "$temporary" "$launcher"
sync
trap - EXIT HUP INT TERM
echo "PenMods launcher installed; next app start imports baseline and activates pending updates"
