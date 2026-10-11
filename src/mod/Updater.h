// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"

#include "Version.h"

#include <nlohmann/json.hpp>

#include "base/YEnum.h"

using json = nlohmann::json;

namespace mod {

struct Version {
    bool                  operator==(Version) const;
    bool                  operator>(Version) const;
    bool                  operator<(Version) const;
    [[nodiscard]] QString toString() const;
    [[nodiscard]] uint32  toNumber() const;
    uint32                mMajor;
    uint32                mMinor;
    uint32                mRevision;
};

class Updater : public QObject, public Singleton<Updater>, private Logger {
    Q_OBJECT

    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY stateChanged)
    Q_PROPERTY(QString targetVersion READ targetVersion NOTIFY stateChanged)
    Q_PROPERTY(QString note READ note NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(int progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canDownload READ canDownload NOTIFY stateChanged)
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY stateChanged)

    struct UpdateInfo;

public:
    QString source() const;
    void    setSource(const QString& value);
    QString currentVersion() const { return mInfo.mCurrentStr; }
    QString targetVersion() const { return mInfo.mLatestStr; }
    QString note() const { return mInfo.mUpdateNote; }
    QString statusText() const;
    int     progress() const { return mInfo.mDownloadProgress; }
    bool    busy() const;
    bool    canDownload() const { return mInfo.mOtaStatus == HAS_NEW_VERSION; }
    bool    canInstall() const { return mInfo.mOtaStatus == DOWNLOAD_FINISHED; }

    Q_INVOKABLE void check();

    Q_INVOKABLE void download();

    Q_INVOKABLE void install();

    void onUiCompleted();

    UpdateInfo& getInfo();

signals:
    void stateChanged();

private:
    friend Singleton<Updater>;
    explicit Updater();

    std::string mClassName = "updater";

    // latest.
    json    mLatestObject;
    QString mSource;

    const Version mSelfVersion = {VERSION_MAJOR, VERSION_MINOR, VERSION_ALTER};

    struct UpdateInfo {
        QString      mCurrentStr       = "";
        QString      mLatestStr        = "";
        UpdateStatus mOtaStatus        = UpdateStatus::LATEST_VERSION;
        QString      mUpdateNote       = "";
        double       mPackageSize      = 0.0;
        uint32       mDownloadProgress = 0;
        uint32       mInstallProgress  = 0;
    } mInfo;

    std::string _calcFileMd5(const QString&);

    bool _unzip(QString, QString);

    void _cleanupTemp();

    void _setCurrentVersion(QString);

    void _setLatestVersion(QString);

    void _setUpdateNote(QString);

    void _setUpdatePackSize(double);

    void _setDownloadProgress(int);

    void _setOtaStatus(UpdateStatus);

    void _setInstallProgress(int);
};

} // namespace mod
