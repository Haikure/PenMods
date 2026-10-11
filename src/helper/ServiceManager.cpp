// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "ServiceManager.h"

#include "common/Event.h"
#include "common/Utils.h"

#include <QFile>
#include <QQmlContext>
#include <QRandomGenerator>
#include <QSaveFile>

namespace mod {

ServiceManager::ServiceManager() {

    mCfg = Config::getInstance().read(mClassName);

    mAdbAutoRun          = mCfg["adb_autorun"];
    mSkipAdbVerification = mCfg["adb_skip_verification"];
    mSshAutoRun          = mCfg["ssh_autorun"];

    connect(&Event::getInstance(), &Event::uiCompleted, this, &ServiceManager::onUiCompleted);
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("serviceManager", this);
    });
}

void ServiceManager::onUiCompleted() {
    if (!getAdbStatus() && getAdbAutoRun()) {
        startAdb(true);
    }
    if (!getSshStatus() && getSshAutoRun()) {
        startSsh(true);
    }
    if (getSkipAdbVerification()) {
        _passAdbVerification();
    }
}

bool ServiceManager::getAdbStatus() const {
    return readFileNoLast("/tmp/.usb_config").find("usb_adb_en") != std::string::npos;
}

bool ServiceManager::getSshStatus() const { return !exec("pidof sshd").empty(); }

void ServiceManager::refresh() {
    emit adbStatusChanged();
    emit sshStatusChanged();
}

bool ServiceManager::startAdb(bool dontShowToast) {
    auto toggle = PEN_CALL(void, "adb_onoff", char);
    if (!toggle) return false;
    toggle(1);
    const bool success = getAdbStatus();
    if (!dontShowToast) {
        showToast(success ? "ADB服务已启用" : "ADB服务启动失败");
    }
    emit adbStatusChanged();
    return success;
}
bool ServiceManager::stopAdb(bool dontShowToast) {
    auto toggle = PEN_CALL(void, "adb_onoff", char);
    if (!toggle) return false;
    toggle(0);
    const bool success = !getAdbStatus();
    if (!dontShowToast) {
        showToast(success ? "ADB服务已停用" : "ADB服务停止失败");
    }
    emit adbStatusChanged();
    return success;
}

bool ServiceManager::startSsh(bool dontShowToast) {
    exec("sshd_sevice start");
    const bool success = getSshStatus();
    if (!dontShowToast) {
        showToast(success ? "SSH服务已启用" : "SSH服务启动失败");
    }
    emit sshStatusChanged();
    return success;
}

bool ServiceManager::stopSsh(bool dontShowToast) {
    exec("sshd_sevice stop");
    const bool success = !getSshStatus();
    if (!dontShowToast) {
        showToast(success ? "SSH服务已停用" : "SSH服务停止失败");
    }
    emit sshStatusChanged();
    return success;
}

bool ServiceManager::getAdbAutoRun() const { return mAdbAutoRun; }

bool ServiceManager::getSshAutoRun() const { return mSshAutoRun; }

bool ServiceManager::getSkipAdbVerification() const { return mSkipAdbVerification; }

void ServiceManager::setSkipAdbVerification(bool val) {
    if (mSkipAdbVerification != val) {
        mSkipAdbVerification          = val;
        mCfg["adb_skip_verification"] = val;
        if (val) {
            _passAdbVerification();
        } else {
            QFile::remove(QStringLiteral("/tmp/.adb_auth_verified"));
        }
        WRITE_CFG;
        emit skipAdbVerificationChanged();
    }
}

void ServiceManager::setAdbAutoRun(bool val) {
    if (mAdbAutoRun != val) {
        mAdbAutoRun         = val;
        mCfg["adb_autorun"] = val;
        WRITE_CFG;
        emit adbAutoRunChanged();
    }
}

void ServiceManager::setSshAutoRun(bool val) {
    if (mSshAutoRun != val) {
        mSshAutoRun         = val;
        mCfg["ssh_autorun"] = val;
        WRITE_CFG;
        emit sshAutoRunChanged();
    }
}

bool ServiceManager::setSshRootPasswd(const QString& val) {
    if (val.isEmpty()) {
        showToast("密码不能为空", "#E9900C");
        return false;
    }
    try {
        QFile shadow("/etc/shadow");
        if (!shadow.open(QIODevice::ReadOnly | QIODevice::Text)) {
            showToast("无法打开影子文件", "#E9900C");
            return false;
        }
        QString tmp;
        bool    isModified = false;
        while (!shadow.atEnd()) {
            auto line = QString(shadow.readLine());
            auto data = line.split(':');
            if (data.length() < 2 || data[0] != "root" || isModified) {
                tmp.append(line);
            } else {
                auto  salt = _getRandomString(6);
                char* crypted =
                    crypt(val.toLocal8Bit().data(), QString("$1$%1$").arg(QString::fromStdString(salt)).toUtf8());
                if (!crypted || crypted[0] == '*') {
                    showToast("无法加密密码", "#E9900C");
                    return false;
                }
                data[1] = QString::fromUtf8(crypted);
                tmp.append(data.join(':'));
                isModified = true;
            }
        }
        shadow.close();
        if (isModified) {
            QSaveFile output(shadow.fileName());
            if (!output.open(QIODevice::WriteOnly)) {
                showToast("无法打开影子文件", "#E9900C");
                return false;
            }
            const auto data = tmp.toUtf8();
            if (output.write(data) != data.size() || !output.commit()) {
                showToast("保存密码失败", "#E9900C");
                return false;
            }
            showToast("密码重设成功");
            return true;
        }
        showToast("Root账户找不到", "#E9900C");
        return false;
    } catch (...) {
        showToast("未知错误", "#E9900C");
        return false;
    }
}

void ServiceManager::_passAdbVerification() { exec("touch /tmp/.adb_auth_verified"); }

std::string ServiceManager::_getRandomString(uint32 length) {
    auto*       generator = QRandomGenerator::global();
    const char* sigs      = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string ret;
    for (uint32 i = 0; i < length; i++) {
        ret += sigs[generator->bounded(26 * 2)];
    }
    return ret;
}

} // namespace mod
