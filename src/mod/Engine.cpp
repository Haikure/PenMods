// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "spdlog/spdlog.h"
#if PL_BUILD_YDP02X
#include "../resource/models/YDP02X/qrc_qml.h"
#elif PL_BUILD_YDP03X
#include "../resource/models/YDP03X/qrc_qml.h"
#endif

#include <QQmlContext>
#include <QQuickView>

#include <QFile>
#include <cstdlib>
#include <dlfcn.h>

#include "base/SymDB.h"

#include "base/YPointer.h"

#include "common/Event.h"

#if PL_BUILD_YDP03X
// YDP03X: the Qt resource namespace is shared by three libraries under
// qrc:/qml/... (YoudaoDictPen + libPluginLoader.so + libXmSDK.so). Replacing only
// the main binary's qml group would drop the libPluginLoader `qml/common` set and
// the libXmSDK `qml/3rdpart/xmly` pages. We therefore register ONE merged tree that
// covers every path of the main process, then best-effort unregister the stock
// groups. The arrays are compiled into this library (embedded fallback) and the
// external libPenModsResources.so is preferred when present.
namespace {

constexpr const char* kResourceLibPath = "/userdisk/PenMods/libPenModsResources.so";

struct ResourceOverride {
    const uchar* qtStruct = nullptr;
    const uchar* qtName   = nullptr;
    const uchar* qtData   = nullptr;
    void*        handle   = nullptr;
};

using GetResourcePtr = const uchar* (*)();

bool resourceOverrideDisabled() {
    const char* value = std::getenv("PENMODS_DISABLE_RESOURCE_OVERRIDE");
    return value && QString::fromLatin1(value) == QStringLiteral("1");
}

ResourceOverride loadExternalResources() {
    ResourceOverride res;
    if (resourceOverrideDisabled() || !QFile::exists(kResourceLibPath)) {
        return res;
    }
    res.handle = dlopen(kResourceLibPath, RTLD_NOW);
    if (!res.handle) {
        spdlog::error("dlopen failed for {}: {}", kResourceLibPath, dlerror());
        return {};
    }
    auto getStruct = reinterpret_cast<GetResourcePtr>(dlsym(res.handle, "get_qt_resource_struct"));
    auto getName   = reinterpret_cast<GetResourcePtr>(dlsym(res.handle, "get_qt_resource_name"));
    auto getData   = reinterpret_cast<GetResourcePtr>(dlsym(res.handle, "get_qt_resource_data"));
    if (!getStruct || !getName || !getData) {
        spdlog::error("{} does not export the expected qrc accessors", kResourceLibPath);
        dlclose(res.handle);
        return {};
    }
    res.qtStruct = getStruct();
    res.qtName   = getName();
    res.qtData   = getData();
    return res;
}

using CleanupFn = void (*)();

// Best-effort cleanup of the stock resource groups, run only AFTER the merged
// tree has been registered (every stock path is already covered by it).
int cleanupStockResourceGroups() {
    static const char* kStockCleanups[] = {
        "_Z21qCleanupResources_qmlv",      // qCleanupResources_qml
        "_Z25qCleanupResources_BaseQmlv",  // qCleanupResources_BaseQml
        "_Z18qCleanupResources_xmlyv",     // qCleanupResources_xmly
    };
    int cleaned = 0;
    for (const char* name : kStockCleanups) {
        auto fn = reinterpret_cast<CleanupFn>(mod::SymDB::getInstance().query(name));
        if (!fn) {
            spdlog::debug("stock resource group not present (skip cleanup): {}", name);
            continue;
        }
        fn();
        ++cleaned;
        spdlog::info("cleaned stock resource group: {}", name);
    }
    return cleaned;
}

bool replaceQmlResources(const ResourceOverride& res) {
    if (!res.qtStruct || !res.qtName || !res.qtData) {
        return false;
    }
    auto cleanup = PEN_CALL(void, "_Z21qCleanupResources_qmlv");
    auto init    = PEN_CALL(void, "_Z18qInitResources_qmlv");
    auto reg     = PEN_CALL(bool, "_Z21qRegisterResourceDataiPKhS0_S0_", int, const uchar*, const uchar*, const uchar*);
    if (!cleanup || !init || !reg) {
        spdlog::error("Qt resource functions are incomplete; skip qrc override");
        return false;
    }
    // 1) Unregister the stock qml group compiled into YoudaoDictPen.
    cleanup();
    // 2) Register the single merged tree; late registration wins the lookup chain.
    const bool ok = reg(0x3, res.qtStruct, res.qtName, res.qtData);
    if (!ok) {
        spdlog::error("qRegisterResourceData failed; restoring original qml resources");
        init();
        return false;
    }
    // 3) Drop the other stock groups (BaseQml, and xmly if already loaded).
    cleanupStockResourceGroups();
    return true;
}

ResourceOverride makeResourceOverride() {
    ResourceOverride res = loadExternalResources();
    if (res.qtStruct && res.qtName && res.qtData) {
        spdlog::info("Using external merged YDP03X qrc (libPenModsResources.so)");
        return res;
    }
    // Embedded fallback (compiled into libPenMods.so).
    res.qtStruct = qt_resource_struct;
    res.qtName   = qt_resource_name;
    res.qtData   = qt_resource_data;
    spdlog::info("Using embedded merged YDP03X qrc");
    return res;
}

} // namespace
#endif

PEN_HOOK(void, _ZN22YGuiApplicationPrivate6initUiEv,
#if PL_BUILD_YDP02X
    QWindow** self
#else
    void* self
#endif
) {
#if PL_BUILD_YDP02X
    auto& view    = *(QQuickView*)*self;
    auto* context = view.rootContext();

    mod::YPointer<QQuickView>::setInstance(&view);

    emit mod::Event::getInstance().beforeUiInitialization(view, context);

    bool                 using_external_resources = false;
    const char*          ResourceLibPath          = "/userdata/PenMods/libPenModsResources.so";
    const unsigned char* new_qt_resource_struct;
    const unsigned char* new_qt_resource_data;
    const unsigned char* new_qt_resource_name;
    if (QFile::exists(ResourceLibPath)) {
        void* lib = dlopen(ResourceLibPath, RTLD_NOW);
        if (!lib) {
            spdlog::error("Can't dlopen libPenModsResources.so");
        } else {
            using get_res_t = const unsigned char* (*)();

            auto get_struct = (get_res_t)dlsym(lib, "get_qt_resource_struct");
            auto get_data   = (get_res_t)dlsym(lib, "get_qt_resource_data");
            auto get_name   = (get_res_t)dlsym(lib, "get_qt_resource_name");

            Q_ASSERT(get_struct && get_data && get_name);

            new_qt_resource_struct = get_struct();
            new_qt_resource_data   = get_data();
            new_qt_resource_name   = get_name();

            spdlog::info("Using external Qt res.");
            using_external_resources = true;
        }
    }

    // Replace QResources
    PEN_CALL(void*, "_Z21qCleanupResources_qmlv")();
    bool res;
    if (using_external_resources) {
        res = PEN_CALL(bool, "_Z21qRegisterResourceDataiPKhS0_S0_", int, const uchar*, const uchar*, const uchar*)(
            0x3,
            new_qt_resource_struct,
            new_qt_resource_name,
            new_qt_resource_data
        );
    } else {
        res = PEN_CALL(bool, "_Z21qRegisterResourceDataiPKhS0_S0_", int, const uchar*, const uchar*, const uchar*)(
            0x3,
            qt_resource_struct,
            qt_resource_name,
            qt_resource_data
        );
    }
    if (res) {
        spdlog::info("Resource files have been replaced!");
    } else {
        spdlog::error("The resource file replacement failed, reload the original resource file.");
        PEN_CALL(void*, "_Z18qInitResources_qmlv")();
    }

    origin(self);
#else
    // YDP03X: the injected view pointer is the first member of YGuiApplicationPrivate.
    auto* view    = reinterpret_cast<QQuickView*>(*reinterpret_cast<void**>(self));
    auto* context = view ? view->rootContext() : nullptr;
    if (view) {
        mod::YPointer<QQuickView>::setInstance(view);
    }
    if (context) {
        emit mod::Event::getInstance().beforeUiInitialization(*view, context);
    }

    // Full merged qrc override for YDP03X (external lib preferred, else embedded).
    if (replaceQmlResources(makeResourceOverride())) {
        spdlog::info("Full merged YDP03X qrc tree has been registered");
    }

    origin(self);
#endif
}
