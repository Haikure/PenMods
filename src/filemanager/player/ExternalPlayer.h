// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

namespace mod::filemanager {

class ExternalPlayer : public QObject, public Singleton<ExternalPlayer> {
    Q_OBJECT

    Q_PROPERTY(QString path READ getOpeningPath NOTIFY mediaChanged)
    Q_PROPERTY(QString title READ getTitle NOTIFY mediaChanged)
    Q_PROPERTY(QString subtitlePath READ getSubtitlePath NOTIFY mediaChanged)
    Q_PROPERTY(bool hasEmbeddedPlayer READ hasEmbeddedPlayer NOTIFY mediaChanged)
    Q_PROPERTY(uint requestId READ requestId NOTIFY mediaChanged)
    Q_PROPERTY(bool fallbackActive READ fallbackActive NOTIFY fallbackChanged)
    Q_PROPERTY(QString fallbackError READ fallbackError NOTIFY fallbackChanged)

public:
    Q_INVOKABLE void open(const QString& path);
    Q_INVOKABLE bool fallbackToMpv(uint requestId, const QString& reason, double position = 0);
    Q_INVOKABLE void close(uint requestId);

    [[nodiscard]] QString getOpeningPath() const;
    [[nodiscard]] QString getTitle() const;
    [[nodiscard]] QString getSubtitlePath() const;
    [[nodiscard]] bool    hasEmbeddedPlayer() const;
    [[nodiscard]] uint    requestId() const { return mRequestId; }
    [[nodiscard]] bool    fallbackActive() const { return mFallbackActive; }
    [[nodiscard]] QString fallbackError() const { return mFallbackError; }

signals:
    void mediaChanged();
    void fallbackChanged();

private:
    friend Singleton<ExternalPlayer>;
    explicit ExternalPlayer();

    QString mOpeningPath;
    QString mFallbackError;
    uint    mRequestId         = 0;
    bool    mOpen              = false;
    bool    mFallbackAttempted = false;
    bool    mFallbackActive    = false;
};

} // namespace mod::filemanager
