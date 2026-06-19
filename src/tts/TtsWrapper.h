// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Singleton.h"
#include "common/service/Logger.h"

#include <QObject>
#include <QString>
#include <QQmlEngine>

namespace mod {

/**
 * @brief TTS 封装类，暴露给 QML 层
 *
 * 通过调用原始固件的 YSoundCenter::play() 方法实现文本转语音。
 * 在 QML 上下文中注册为 "tts"，QML 可直接调用。
 */
class TtsWrapper : public QObject, public Singleton<TtsWrapper>, private Logger {
    Q_OBJECT
    QML_SINGLETON
    QML_NAMED_ELEMENT(Tts)

public:
    explicit TtsWrapper(QObject* parent = nullptr);

    /**
     * @brief 播放 TTS 语音
     * @param word    要朗读的文本
     * @param lang    语言代码（如 "en"、"zh-CHS"）
     * @param phonetic 音标（可选，可为空字符串）
     * @param type    TTS 引擎类型（0 = 默认）
     */
    Q_INVOKABLE void speak(const QString& word, const QString& lang = "en", const QString& phonetic = QString(), int type = 0);

    /**
     * @brief 用美式英语朗读（便捷方法）
     */
    Q_INVOKABLE void speakEnglish(const QString& word);

    /**
     * @brief 用中文朗读（便捷方法）
     */
    Q_INVOKABLE void speakChinese(const QString& word);

signals:
    // 通知 QML 层 TTS 播放结束
    void speakFinished();

private:
    friend class Singleton<TtsWrapper>;
};

} // namespace mod