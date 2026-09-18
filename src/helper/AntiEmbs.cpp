// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "AntiEmbs.h"

#include "base/YPointer.h"

#include "common/Event.h"
#include "common/Utils.h"

#include "mod/Mod.h"

#include "filemanager/FileManager.h"
#include "locker/Locker.h"

#include "system/sound/ASound.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QQmlContext>

namespace mod {

AntiEmbs::AntiEmbs() {

    mCfg = Config::getInstance().read(mClassName);

    mAutoMute       = mCfg["auto_mute"];
    mAutoPronLocked = mCfg["no_auto_pron"];
    mFastHide       = mCfg["fast_hide_music"];
    mLowVoiceMode   = mCfg["low_voice"];
    mFastMute       = mCfg["fast_mute"];

    connect(this, &AntiEmbs::lowVoiceModeChanged, [&]() {
        mLowVoiceMode ? ASound::getInstance().setDb({-80.0, -20.0}) : ASound::getInstance().setDb({-50.0, 0.0});
        exec("killall SoundPlayer");
    });

    connect(this, &AntiEmbs::autoPronLockChanged, [&]() {
        if (mAutoPronLocked) {
            PEN_CALL(void*, "_ZN15YSettingManager16setAutoPronounceEb", void*, bool)
            (YPointer<YSettingManager>::getInstance(), false);
            PEN_CALL(void*, "_ZN15YSettingManager20setIsWbAutoPronounceEb", void*, bool)
            (YPointer<YSettingManager>::getInstance(), false);
        }
    });

    connect(&Event::getInstance(), &Event::uiCompleted, this, &AntiEmbs::onUiCompleted);
    connect(&Event::getInstance(), &Event::homeButtonPressed, this, &AntiEmbs::onHomeButtonPress);

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("antiEmbs", this);
    });
}

void AntiEmbs::onUiCompleted() {
    emit lowVoiceModeChanged();
    emit autoPronLockChanged();
}

void AntiEmbs::onHomeButtonPress() {
    if (!getFastHide()) {
        return;
    }
    auto ts = QDateTime::currentDateTime().toSecsSinceEpoch();
    mHomePressTimes.emplace_back(ts);
    if (mHomePressTimes.size() < 4) {
        return;
    }
    while (mHomePressTimes.size() > 4) {
        mHomePressTimes.erase(mHomePressTimes.begin());
    }
    for (auto i : mHomePressTimes) {
        if (ts - i > 3) {
            return;
        }
    }
    mHomePressTimes.clear();
    if (isActive() && Locker::getInstance().getScene("antiembs_deactivate")) {
        emit deactivationUnlockRequested();
        return;
    }
    showToast("操作执行完毕");
    filemanager::FileManager::getInstance().negateHiddenAll();
    emit activeChanged();
}

bool AntiEmbs::deactivate(const QString& password) {
    if (!isActive()) {
        return true;
    }
    auto& locker = Locker::getInstance();
    if (locker.getScene("antiembs_deactivate") && password != locker.getPassword()) {
        return false;
    }
    showToast("操作执行完毕");
    filemanager::FileManager::getInstance().negateHiddenAll();
    emit activeChanged();
    return true;
}

bool AntiEmbs::getAutoMute() const { return mAutoMute; }

bool AntiEmbs::getLowVoiceMode() const { return mLowVoiceMode; }

bool AntiEmbs::getAutoPronLocked() const { return mAutoPronLocked; }

bool AntiEmbs::getFastHide() const { return mFastHide && Mod::getInstance().isTrustedDevice(); }

bool AntiEmbs::isActive() const {
    return Mod::getInstance().isTrustedDevice() && filemanager::FileManager::getInstance().shouldHiddenAll();
}

bool AntiEmbs::getFastMute() const { return mFastMute; }

void AntiEmbs::setLowVoiceMode(bool val) {
    if (mLowVoiceMode != val) {
        mLowVoiceMode = val;
        UPDATE_CFG("low_voice", val);
        emit lowVoiceModeChanged();
    }
}

void AntiEmbs::setAutoMute(bool val) {
    if (mAutoMute != val) {
        mAutoMute = val;
        UPDATE_CFG("auto_mute", val);
        emit autoMuteChanged();
    }
}

void AntiEmbs::setFastMute(bool val) {
    if (mFastMute != val) {
        mFastMute = val;
        UPDATE_CFG("fast_mute", val);
        emit fastMuteChanged();
    }
}

void AntiEmbs::setAutoPronLocked(bool val) {
    if (mAutoPronLocked != val) {
        mAutoPronLocked = val;
        UPDATE_CFG("no_auto_pron", val);
        emit autoPronLockChanged();
    }
}

void AntiEmbs::setFastHide(bool val) {
    if (mFastHide != val) {
        mFastHide = val;
        UPDATE_CFG("fast_hide_music", val);
        emit fastHideChanged();
    }
}

} // namespace mod

// 二代蓝牙状态回调为 bool 参数；三代为携带 JSON 字符串（method/state/profile）的 QString，
// 蓝牙关闭（"method":"close"）时若开启防尴尬自动静音则把音量归零。
// 三代 setSpkVolume 为 (int, int) 双参签名。
#if PL_BUILD_YDP02X
PEN_HOOK(uint64, _ZN17YBlueToothManager17onBtStatusChangedEb, uint64 self, bool a2) {
    if (!a2 && mod::AntiEmbs::getInstance().getAutoMute()) {
        PEN_CALL(void*, "_ZN15YSettingManager12setSpkVolumeEi", void*, int)
        (mod::YPointer<YSettingManager>::getInstance(), 0);
    }
    return origin(self, a2);
}
#else
PEN_HOOK(uint64, _ZN17YBlueToothManager17onBtStatusChangedE7QString, uint64 self, const QString& status) {
    if (mod::AntiEmbs::getInstance().getAutoMute()
        && status.contains(QStringLiteral("\"method\":\"close\""))) {
        PEN_CALL(void*, "_ZN15YSettingManager12setSpkVolumeEii", void*, int, int)
        (mod::YPointer<YSettingManager>::getInstance(), 0, 0);
    }
    return origin(self, status);
}
#endif

PEN_HOOK(uint64, _ZN15YSettingManager16setAutoPronounceEb, uint64 self, bool a2) {
    if (mod::AntiEmbs::getInstance().getAutoPronLocked()) {
        a2 = false;
    }
    return origin(self, a2);
}

PEN_HOOK(uint64, _ZN15YSettingManager20setIsWbAutoPronounceEb, uint64 self, bool a2) {
    if (mod::AntiEmbs::getInstance().getAutoPronLocked()) {
        a2 = false;
    }
    return origin(self, a2);
}
