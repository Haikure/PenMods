// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tts/TtsWrapper.h"

#include "base/Hook.h"
#include "base/YPointer.h"
#include "common/Event.h"

#include <QQmlContext>
#include <QQuickView>

namespace mod {

TtsWrapper::TtsWrapper(QObject* parent) : QObject(parent), Logger("TtsWrapper") {
    // 在 UI 初始化前将自身注册到 QML 上下文
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("tts", this);
        info("TtsWrapper 已在 QML 上下文中注册为 'tts'.");
    });
}

void* TtsWrapper::soundCenter() {
    if (m_soundCenter) return m_soundCenter;

    auto instanceFn = reinterpret_cast<void*(*)()>(
        PEN_SYM("_ZN10YSingletonI12YSoundCenterE8instanceEv"));
    if (!instanceFn) {
        error("无法解析 YSoundCenter 实例符号");
        return nullptr;
    }

    m_soundCenter = instanceFn();
    if (!m_soundCenter) {
        error("YSoundCenter 实例为空");
        return nullptr;
    }

    m_playFn = reinterpret_cast<PlayFn>(
        PEN_SYM("_ZN12YSoundCenter4playERK7QStringS2_S2_i"));
    m_stopFn = reinterpret_cast<StopFn>(
        PEN_SYM("_ZN12YSoundCenter4stopEv"));
    m_isPlayingFn = reinterpret_cast<IsPlayingFn>(
        PEN_SYM("_ZN12YSoundCenter9isPlayingEv"));

    // 把 YSoundCenter::soundEnd() 转发为 speakFinished()
    connect(reinterpret_cast<QObject*>(m_soundCenter), SIGNAL(soundEnd()),
            this, SIGNAL(speakFinished()));

    info("YSoundCenter 实例已缓存，speakFinished 信号已连接。");
    return m_soundCenter;
}

void TtsWrapper::speak(const QString& word, const QString& lang, const QString& phonetic, int type) {
    if (word.isEmpty()) {
        warn("speak() 收到空文本，跳过。");
        return;
    }

    void* sc = soundCenter();
    if (!sc) return;

    if (!m_playFn) {
        error("无法解析 YSoundCenter::play 符号");
        return;
    }

    debug("TTS speak: word='{}', lang='{}', phonetic='{}', type={}",
          word.toStdString(), lang.toStdString(), phonetic.toStdString(), type);

    m_playFn(sc, word, lang, phonetic, type);
    info("TTS 已朗读: {}", word.toStdString());
}

void TtsWrapper::speakEnglish(const QString& word) {
    speak(word, "en-US", QString(), 0x0E);
}

void TtsWrapper::speakChinese(const QString& word) {
    speak(word, "zh-CHS", QString(), 0);
}

void TtsWrapper::stop() {
    void* sc = soundCenter();
    if (!sc || !m_stopFn) return;
    m_stopFn(sc);
    info("TTS 已停止");
}

bool TtsWrapper::isPlaying() {
    void* sc = soundCenter();
    if (!sc || !m_isPlayingFn) return false;
    return m_isPlayingFn(sc);
}

} // namespace mod

// ========== 静态初始化：确保 TtsWrapper 实例在 UI 初始化前被创建 ==========
// 不依赖任何外部代码主动调用 getInstance()
static auto& s_ensureTtsInit = mod::TtsWrapper::getInstance();