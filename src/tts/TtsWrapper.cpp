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

void TtsWrapper::speak(const QString& word, const QString& lang, const QString& phonetic, int type) {
    if (word.isEmpty()) {
        warn("speak() 收到空文本，跳过。");
        return;
    }

    debug("TTS speak: word='{}', lang='{}', phonetic='{}', type={}",
          word.toStdString(), lang.toStdString(), phonetic.toStdString(), type);

    // 获取 YSoundCenter 单例指针
    // 符号: _ZN10YSingletonI12YSoundCenterE8instanceEv
    auto* soundCenter = ((void*(*)())(PEN_SYM("_ZN10YSingletonI12YSoundCenterE8instanceEv")))();

    if (!soundCenter) {
        error("YSoundCenter 实例为空，无法播放 TTS。");
        return;
    }

    // 调用原始固件的 YSoundCenter::play(QString, QString, QString, int)
    // 符号: _ZN12YSoundCenter4playERK7QStringS2_S2_i
    PEN_CALL(void, "_ZN12YSoundCenter4playERK7QStringS2_S2_i",
             void*, QString const&, QString const&, QString const&, int)
    (soundCenter, word, lang, phonetic, type);

    info("TTS 已朗读: {}", word.toStdString());
}

void TtsWrapper::speakEnglish(const QString& word) {
    if (word.isEmpty()) return;
    // type=0x0E 对应 Microsoft TTS（英语 TTS 引擎）
    speak(word, "en-US", QString(), 0x0E);
}

void TtsWrapper::speakChinese(const QString& word) {
    if (word.isEmpty()) return;
    // type=0 对应默认 TTS 引擎
    speak(word, "zh-CHS", QString(), 0);
}

} // namespace mod

// ========== 静态初始化：确保 TtsWrapper 实例在 UI 初始化前被创建 ==========
// 不依赖任何外部代码主动调用 getInstance()
static auto& s_ensureTtsInit = mod::TtsWrapper::getInstance();