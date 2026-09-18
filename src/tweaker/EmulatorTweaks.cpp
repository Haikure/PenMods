// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

// This file contains some necessary tweaks for running under QEMU environment.
// Should not be included in a release that runs in a real environment.

#if PL_QEMU

#include "common/Utils.h"
#include "common/util/System.h"

#include "base/YEnum.h"

using namespace mod;

#if PL_BUILD_YDP02X
#define PEN_QEMU_SKU     "OVERHEAD_D2_SKU_EXA_ADV"
#define PEN_QEMU_VERSION "2.1.2"
#else
#define PEN_QEMU_SKU     "OVERHEAD_D3_SKU_CHN_STD"
#define PEN_QEMU_VERSION "2.7.3"
#endif

// from: librkdev.so
// return value:
//   OK    0
//   ERROR -1
PEN_HOOK(int64_t, _console_run, const char* cmd, char* result) {
    switch (H(cmd)) {
    case H(
        R"(update_engine --misc=display | grep "update.info" | awk -F "status=" '{print $2}' | awk -F "." '{print $1}')"
    ):
        strcpy(result, "0");
        break;
    case H("vendor_storage -r VENDOR_CUSTOM_ID_0E -t string | awk '{print $NF}'"):
        strcpy(result, PEN_QEMU_SKU);
        break;
    case H("load_sys_cfg sku"):
        strcpy(result, PEN_QEMU_SKU);
        break;
    case H("load_sys_cfg brightness"):
        strcpy(result, "99");
        break;
    case H("playback_dev_ctrl show"):
        strcpy(result, "spk");
        break;
    case H("load_sys_cfg spk-volume"):
        strcpy(result, "30");
        break;
    case H("cat /Version | grep Version | awk '{print $2}'"):
        strcpy(result, PEN_QEMU_VERSION);
        break;
    case H("/bin/uname -r"):
        strcpy(result, "4.4.159");
        break;
    case H("cat /proc/cpuinfo | grep Features"):
        strcpy(result, "Features        : fp asimd evtstrm aes pmull sha1 sha2 crc32");
        break;
    case H("hciconfig | grep hci0"):
        strcpy(result, "");
        break;
    default:
        spdlog::warn("BANNED executing: \"{}\"", cmd);
        return 0;
    }
    spdlog::warn("INTERRUPTED executing: \"{}\"", cmd);
    return 0;
}

// from: librkdev.so
// result value:
//   OK 0
PEN_HOOK(int64_t, get_battery_info,
#if PL_BUILD_YDP02X
    int& result_capacity, bool& result_charging
#else
    int* capacity, bool* charging
#endif
) {
#if PL_BUILD_YDP02X
    result_capacity = 100;
    result_charging = true;
#else
    if (capacity) *capacity = 100;
    if (charging) *charging = true;
#endif
    return 0;
}

// network

PEN_HOOK(int64_t, get_wifi_status,
#if PL_BUILD_YDP02X
    WifiStatus& result
#else
    void* status
#endif
) {
#if PL_BUILD_YDP02X
    result.mEnabled            = true;
    result.mIsConnected        = true;
    result.mIsNetworkAvailable = true;
    strcpy(result.mSSID, "Emulator Environment");
    result.mSignal = 100;
#else
    // YDP03X: WifiStatus layout is only partially verified; the stub writes no
    // fields to avoid clobbering the caller's stack.
    (void)status;
#endif
    return 0;
}

PEN_HOOK(int64_t, wifi_scan,
#if PL_BUILD_YDP02X
    char* deviceList, uint32& deviceCount
#else
    void
#endif
) {
#if PL_BUILD_YDP02X
    strcpy(deviceList, "Emulator Environment");
    deviceCount = 1;
#endif
    return 0;
}

PEN_HOOK(int64_t, set_wifi_onoff,
#if PL_BUILD_YDP02X
    bool onoff
#else
    int onoff
#endif
) {
    return 0;
}

#if PL_BUILD_YDP02X
PEN_HOOK(int64_t, wifi_connect, const char* a1, const char* a2) { return 0; }

PEN_HOOK(int64_t, wifi_disconnect, const char* onoff) { return 0; }

PEN_HOOK(int64_t, wifi_remove, const char* onoff) { return 0; }
#else
// YDP03X librkdev exposes additional C API stubs for the emulator.
PEN_HOOK(int64_t, get_audio_dev_type) { return 3; } // 3 = digital headset
PEN_HOOK(void, led_init, int gpio) { (void)gpio; }
PEN_HOOK(void, led_on) {}
PEN_HOOK(void, led_off) {}
PEN_HOOK(int, _get_led_gpio, int id) { return id; }
#endif


// block sound play

PEN_HOOK(void*, _ZN13YRecordCenter18startRecordProcessEv, void* self) {
    spdlog::warn("BANNED start record process.");
    return nullptr;
}

PEN_HOOK(void*, _ZN12YSoundCenter17startSoundProcessEv, void* self) {
    spdlog::warn("BANNED start sound process");
    return nullptr;
}

PEN_HOOK(uint32, _ZN12YSoundCenter4playERK7QStringS2_S2_i, void* a1, void* a2, void* a3, void* a4, void* a5) {
    return 0;
}

PEN_HOOK(uint32, _ZN12YSoundCenter8playFileERK7QString, void* a1, void* a2) { return 0; }

PEN_HOOK(uint32, _ZN12YSoundCenter9playMusicERK7QStringxd, void* a1, void* a2, void* a3, void* a4) { return 0; }

PEN_HOOK(uint32, _ZN12YSoundCenter12playFileDataERK7QString, void* self, QString* a2) { return 0; }

// relocation database

#if PL_BUILD_YDP02X
PEN_HOOK(void*, _ZN8Database17ConnectionManager15setDatabaseNameERK7QString, void* self, QString const& path) {
    QString newPath = path;
    if (path.startsWith("/userdisk/database/")) {
        newPath = newPath.replace("/userdisk/database/", util::getModuleFileInfo().absolutePath());
    }
    spdlog::warn("{} database path: {}", newPath == path ? "setting" : "INTERRUPTED", newPath.toStdString());
    return origin(self, newPath);
}
#else
// YDP03X: Database::ConnectionManager::setDatabaseName does not exist; DB paths are
// hard-coded to /userdisk/database/*.db, so no redirection hook is needed.
#endif

// device

PEN_HOOK(int64_t, get_sn, char* result) {
    strcpy(result, "2BC0000011451400000");
    return 0;
}

PEN_HOOK(int64_t, get_mac, char* result) {
    strcpy(result, "43:0b:6f:e2:71:e0");
    return 0;
}

#endif