#!/usr/bin/env bash
# Deploy the YDP03X host, resource overlay and optional QML plugin together.
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mode=release
plugin_only=false
dry_run=false
adb_command=("${ADB:-adb}")

usage() {
    echo "Usage: $0 [--mode release|debug] [--serial SERIAL] [--plugin-only] [--dry-run]"
    echo "Default: deploy libPenMods.so, libPenModsResources.so and FFmpegPlayer to /userdisk/PenMods."
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)
            [[ $# -ge 2 ]] || { usage >&2; exit 1; }
            mode=$2
            shift 2
            ;;
        --serial)
            [[ $# -ge 2 ]] || { usage >&2; exit 1; }
            adb_command+=(-s "$2")
            shift 2
            ;;
        --plugin-only) plugin_only=true; shift ;;
        --dry-run) dry_run=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 1 ;;
    esac
done
[[ "$mode" == release || "$mode" == debug ]] || { usage >&2; exit 1; }

artifact_dir="$repo_dir/build/linux/arm64-v8a/$mode"
module_dir="$artifact_dir/qml/FFmpegPlayer"
destination=/userdisk/PenMods
sources=()
destinations=()
if [[ "$plugin_only" == false ]]; then
    sources+=("$artifact_dir/libPenMods.so" "$artifact_dir/libPenModsResources.so")
    destinations+=("$destination/libPenMods.so" "$destination/libPenModsResources.so")
fi
sources+=("$module_dir/libffmpegplayerplugin.so" "$module_dir/qmldir")
destinations+=("$destination/qml/FFmpegPlayer/libffmpegplayerplugin.so" "$destination/qml/FFmpegPlayer/qmldir")

hashes=()
for index in "${!sources[@]}"; do
    file=${sources[$index]}
    [[ -s "$file" ]] || { echo "Missing artifact: $file" >&2; exit 1; }
    read -r digest _ < <(sha256sum "$file")
    hashes+=("$digest")
    printf '%s -> %s\n  SHA256 %s\n' "$file" "${destinations[$index]}" "$digest"
done

if [[ "$dry_run" == true ]]; then
    echo "Dry run: artifacts checked; no device commands executed."
    exit 0
fi

state=$("${adb_command[@]}" get-state)
[[ "$state" == device ]] || { echo "No ready ADB device; use --serial if needed." >&2; exit 1; }
"${adb_command[@]}" shell "mkdir -p '$destination/qml/FFmpegPlayer'"

staging_suffix=".penmods-new-$$"
staged=()
cleanup() {
    # Only remove this invocation's temporary uploads, never deployed libraries.
    for temporary in "${staged[@]}"; do
        "${adb_command[@]}" shell "rm -f '$temporary'" >/dev/null 2>&1 || true
    done
}
trap cleanup EXIT

# Upload and verify every file before replacing anything. Rename instead of
# truncating .so files that are still mapped by the running host process.
for index in "${!sources[@]}"; do
    temporary="${destinations[$index]}$staging_suffix"
    staged+=("$temporary")
    "${adb_command[@]}" push "${sources[$index]}" "$temporary"
    remote=$("${adb_command[@]}" shell "sha256sum '$temporary'")
    remote=${remote%% *}
    [[ "$remote" == "${hashes[$index]}" ]] || { echo "Upload hash mismatch: $temporary" >&2; exit 1; }
done

publish_command="set -e; "
for index in "${!destinations[@]}"; do
    publish_command+="mv -f '${staged[$index]}' '${destinations[$index]}'; "
done
publish_command+="sync"
"${adb_command[@]}" shell "$publish_command"
staged=()

for index in "${!destinations[@]}"; do
    remote=$("${adb_command[@]}" shell "sha256sum '${destinations[$index]}'")
    remote=${remote%% *}
    [[ "$remote" == "${hashes[$index]}" ]] || { echo "Deployed hash mismatch: ${destinations[$index]}" >&2; exit 1; }
done

"${adb_command[@]}" shell 'if pidof YoudaoDictPen >/dev/null; then killall YoudaoDictPen; fi'
for attempt in {1..12}; do
    sleep 1
    pid=$("${adb_command[@]}" shell pidof YoudaoDictPen 2>/dev/null || true)
    pid=${pid//$'\r'/}
    if [[ -n "$pid" ]]; then
        # Wait once more to catch an immediate startup crash.
        sleep 2
        running=$("${adb_command[@]}" shell pidof YoudaoDictPen 2>/dev/null || true)
        running=${running//$'\r'/}
        if [[ "$running" == "$pid" ]]; then
            echo "Deployed and verified. YoudaoDictPen PID: $pid"
            echo "Open a video in the file manager to verify playback and controls."
            exit 0
        fi
    fi
done
echo "Artifacts verified, but YoudaoDictPen did not remain running; inspect the device startup log." >&2
exit 1
