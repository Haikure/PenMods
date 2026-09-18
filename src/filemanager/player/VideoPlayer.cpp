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

// YDP03X: video playback is routed to the external player /userdisk/VideoPlayer
// (mirroring PenMods3). No ydubus_sender_send_event dependency.

#include <QProcess>

#include <spdlog/spdlog.h>

namespace mod::filemanager {

VideoPlayer::VideoPlayer() {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("videoPlayer", this);
    });
}

void VideoPlayer::open(QString dir) {
    mOpeningFileName = std::move(dir);
    const QString path = FileManager::getInstance().getCurrentPath().absoluteFilePath(mOpeningFileName);
    spdlog::info("external player open video: {}", path.toStdString());
    QProcess::startDetached(QStringLiteral("/userdisk/VideoPlayer"), {path});
}

QString VideoPlayer::getOpeningPath() {
    return "file://" + FileManager::getInstance().getCurrentPath().absoluteFilePath(mOpeningFileName);
}

void VideoPlayer::onStatusChanged(const QString& status) {
    // 外部播放器为独立进程，无原生播放器状态事件；保留接口兼容。
    spdlog::info("video status: {}", status.toStdString());
}

} // namespace mod::filemanager

#endif
