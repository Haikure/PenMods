// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "System.h"

#include <dlfcn.h>
#include <unistd.h>

namespace mod::util {

QFileInfo getModuleFileInfo() {
    Dl_info info;
    if (dladdr((void*)getModuleFileInfo, &info) == 0) return {};
    return QFileInfo(info.dli_fname);
}

QString getStateDirectory() {
#ifdef PL_QEMU
    return getModuleFileInfo().absolutePath();
#else
    return QStringLiteral("/userdata/PenMods");
#endif
}

QString getUpdateDirectory() {
    const auto fromLauncher = qEnvironmentVariable("PENMODS_UPDATE_ROOT");
    return fromLauncher.isEmpty() ? QStringLiteral("/userdisk/PenMods/update-system") : fromLauncher;
}

QFileInfo getApplicationFileInfo() {
    char       path[4096];
    const auto length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length < 0) return {};
    return QFileInfo(QString::fromLocal8Bit(path, static_cast<int>(length)));
}

} // namespace mod::util