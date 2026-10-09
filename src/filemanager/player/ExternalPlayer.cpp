// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "filemanager/player/ExternalPlayer.h"

#include "base/YPointer.h"
#include "filemanager/FileManager.h"

#include "common/Event.h"

#include <QFileInfo>
#include <QProcess>
#include <QQmlContext>
#include <QUrl>

#include <cmath>

namespace mod::filemanager {

namespace {

bool isLeftHandMode() {
    auto* settings = YPointer<YSettingManager>::getInstance();
    auto  getter   = reinterpret_cast<bool (*)(void*)>(PEN_SYM("_ZNK15YSettingManager15isRightHandModeEv"));
    return settings && getter && !getter(settings);
}

} // namespace

ExternalPlayer::ExternalPlayer() {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("externalPlayer", this);
    });
}

void ExternalPlayer::open(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    const QUrl    url(path);
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : path;
    // Snapshot the absolute path: browsing another directory must not change a pending request.
    mOpeningPath = QDir::cleanPath(FileManager::getInstance().getCurrentPath().absoluteFilePath(localPath));
    if (++mRequestId == 0) {
        ++mRequestId;
    }
    mOpen              = true;
    mFallbackAttempted = false;
    mFallbackActive    = false;
    mFallbackError.clear();
    emit mediaChanged();
    emit fallbackChanged();

    if (!hasEmbeddedPlayer()) {
        fallbackToMpv(mRequestId, QStringLiteral("Embedded player is not installed"));
    }
}

bool ExternalPlayer::hasEmbeddedPlayer() const {
#if PL_BUILD_YDP03X
    const QDir      module(QStringLiteral("/userdisk/PenMods/qml/FFmpegPlayer"));
    const QFileInfo library(module.filePath(QStringLiteral("libffmpegplayerplugin.so")));
    const QFileInfo manifest(module.filePath(QStringLiteral("qmldir")));
    return library.isFile() && library.isReadable() && manifest.isFile() && manifest.isReadable();
#else
    // The embedded playback page is supplied by the YDP03X resource overlay.
    return false;
#endif
}

bool ExternalPlayer::fallbackToMpv(uint requestId, const QString& reason, double position) {
    // Loader errors, decoder errors and the startup watchdog may arrive together.
    // A late callback from a closed/replaced page must never launch another player.
    if (!mOpen || requestId != mRequestId) {
        return false;
    }
    if (mFallbackAttempted) {
        return mFallbackActive;
    }
    mFallbackAttempted = true;

    QStringList args;
    if (isLeftHandMode()) {
        args << QStringLiteral("--video-rotate=180");
    }
    if (std::isfinite(position) && position > 0) {
        args << QStringLiteral("--start=%1").arg(position, 0, 'f', 3);
    }
    args << QStringLiteral("--") << mOpeningPath;

    QProcess process;
    process.setProgram(QStringLiteral("/userdisk/VideoPlayer"));
    process.setArguments(args);
    mFallbackActive = process.startDetached();
    if (mFallbackActive) {
        spdlog::warn("Embedded player request {} fell back to MPV: {}", requestId, reason.toStdString());
    } else {
        mFallbackError = QStringLiteral("无法启动 MPV：%1").arg(process.errorString());
        spdlog::error("MPV fallback failed for request {}: {}", requestId, process.errorString().toStdString());
    }
    emit fallbackChanged();
    return mFallbackActive;
}

void ExternalPlayer::close(uint requestId) {
    if (requestId == mRequestId) {
        mOpen = false;
    }
}

QString ExternalPlayer::getOpeningPath() const {
    return mOpeningPath.isEmpty() ? QString() : QUrl::fromLocalFile(mOpeningPath).toString(QUrl::FullyEncoded);
}

QString ExternalPlayer::getTitle() const { return QFileInfo(mOpeningPath).completeBaseName(); }

QString ExternalPlayer::getSubtitlePath() const {
    const QFileInfo video(mOpeningPath);
    for (const auto* suffix : {".ass", ".lrc"}) {
        const QFileInfo subtitle(video.absoluteDir().filePath(video.completeBaseName() + suffix));
        if (subtitle.isFile() && subtitle.isReadable()) {
            return QUrl::fromLocalFile(subtitle.absoluteFilePath()).toString(QUrl::FullyEncoded);
        }
    }
    return {};
}

} // namespace mod::filemanager
