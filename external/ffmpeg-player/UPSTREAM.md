# Embedded FFmpeg player

Vendored from the second-generation PenMods repository's `external/ffmpeg-player`
on 2026-10-09. Its upstream is [Haikure/ffmpeg-player](https://github.com/Haikure/ffmpeg-player),
v1.0.0 / `48ca849`, under GPL-3.0. The source, FFmpeg patches, and ALSA/MPP/DRM
headers retain the upstream notices. PenMods is licensed under GPL-3.0-only.

The independent `ffmpegplayerplugin` xmake target is enabled with
`PENMODS_WITH_PLAYER=1`, as in the second-generation repository. Dependencies are
statically linked and hidden with `src/ffmpegplayer.exports` to avoid symbol
interposition with the host's FFmpeg and OpenSSL libraries.

The ARM64 ALSA, MPP and DRM link libraries are included in `external/dictpen`,
with the original relative symbolic links from the second-generation repository.
The build uses this repository-relative directory without extra path configuration.
These files are cross-link inputs; deployment uses the device's existing system
libraries and does not replace them.

Local build changes: corrected moc header dependency paths and FFmpeg cache
revision 9 for the optimized `ffplayer-openssl3` dependency. OpenSSL uses
`no-tests` and `install_sw`. YDP03X already enables VPU; this integration does
not alter the kernel, device tree or hardware decoder environment.

The player QML pages live in the separate resource repository at
`~/build/qrc/ydp03x/qml/audiopages`. PenMods stores their generated resource
header. See [the integration guide](../../doc/FFMPEG_PLAYER.md).
