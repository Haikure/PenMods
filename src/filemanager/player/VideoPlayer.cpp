// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "filemanager/player/VideoPlayer.h"

#include "filemanager/FileManager.h"

#include "common/Event.h"
#include "common/Utils.h"

#include <QQmlContext>

#if PL_BUILD_YDP02X
namespace mod::filemanager {

VideoPlayer::VideoPlayer() {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("videoPlayer", this);
    });
} // namespace mod::filemanager

void VideoPlayer::open(QString dir) { mOpeningFileName = std::move(dir); }

QString VideoPlayer::getOpeningPath() {
    return "file://" + FileManager::getInstance().getCurrentPath().absoluteFilePath(mOpeningFileName);
}

void VideoPlayer::onStatusChanged(const QString& status) {
    switch (H(status.toLocal8Bit().data())) {
    case H("playing"):
        PEN_CALL(void*, "ydubus_sender_send_event", int, const char*)(0, R"({"action":"Open"})");
        // InputDaemon>::getInstance().pause();
        break;
    case H("paused"):
    case H("stopped"):
        PEN_CALL(void*, "ydubus_sender_send_event", int, const char*)(0, R"({"action":"Close"})");
        // InputDaemon>::getInstance().resume();
        break;
    default:
        break;
    }
}
} // namespace mod::filemanager

#else

// YDP03X: legacy callers share the embedded player and MPV fallback route.

#include "filemanager/player/ExternalPlayer.h"

#include <spdlog/spdlog.h>

namespace mod::filemanager {

VideoPlayer::VideoPlayer() {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("videoPlayer", this);
    });
}

void VideoPlayer::open(QString dir) {
    mOpeningFileName = std::move(dir);
    ExternalPlayer::getInstance().open(mOpeningFileName);
}

QString VideoPlayer::getOpeningPath() { return ExternalPlayer::getInstance().getOpeningPath(); }

void VideoPlayer::onStatusChanged(const QString& status) {
    // 内嵌播放器由自己的 QML 页面管理状态；保留旧接口兼容。
    spdlog::info("video status: {}", status.toStdString());
}

} // namespace mod::filemanager

#endif
