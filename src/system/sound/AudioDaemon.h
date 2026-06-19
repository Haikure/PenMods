// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include "common/service/Singleton.h"
#include "mod/Config.h"

#include <QTimer>

#include <fstream>

namespace mod {

/// 音频源类型，用于引用计数跟踪
enum class AudioSource {
    TTS,        ///< TTS 播放（扫描翻译朗读）
    MUSIC,      ///< 音乐播放（MusicPlayer）
    FILE_PLAY,  ///< 文件音频播放
    SYSTEM,     ///< 系统音频（录音等）
};

/// 音频守护进程运行状态
enum class AudioDaemonState {
    IDLE,           ///< 无音频输出活动
    PLAYING,        ///< 音频输出已激活（引用计数 > 0）
    CLOSE_DELAY,    ///< 所有引用已释放，等待延迟关闭超时
};

/**
 * @brief 音频输出生命周期守护进程
 *
 * 拦截 YSoundCenter::openAudioOutput() 和 closeAudioOutput()，
 * 通过引用计数 + 延迟关闭 + 状态机智能管理音频设备生命周期，
 * 防止频繁开关音频输出导致的功耗问题或瞬态竞态条件。
 *
 * 与原始 open/close 的交互方式：
 * - PEN_HOOK 的 detour 函数是唯一调用 origin (原始函数) 的地方
 * - AudioDaemon 负责状态追踪和决策，但不直接调用原始函数
 * - detour 通过 AudioDaemon 的 onBeforeOpen() / onBeforeClose() 等方法
 *   询问是否应该调用 origin，以及调用前后的状态更新
 */
class AudioDaemon : public QObject, public Singleton<AudioDaemon>, private Logger {
    Q_OBJECT
    Q_PROPERTY(int refCount READ refCount NOTIFY refCountChanged)
    Q_PROPERTY(AudioDaemonState daemonState READ state NOTIFY stateChanged)

public:
    /// 获取音频输出引用。返回新的引用计数。
    /// @param source 请求音频输出的子系统
    int acquire(AudioSource source);

    /// 释放音频输出引用。返回新的引用计数。
    /// @param source 释放音频输出的子系统
    int release(AudioSource source);

    /// 当前引用计数
    int refCount() const { return mRefCount; }

    /// 当前守护进程状态
    AudioDaemonState state() const { return mState; }

    /// 启用/禁用守护进程（用于调试）
    void setEnabled(bool enabled) { mEnabled = enabled; }
    bool isEnabled() const { return mEnabled; }

    // --- hook 决策辅助接口 ---
    // 以下接口供 PEN_HOOK detour 使用，用于决定是否调用 origin

    /// detour 调用此方法询问：是否应该调用原始的 openAudioOutput？
    /// @return true = 需要调用 origin；false = 抑制本次调用
    bool shouldOpenAudioOutput();

    /// detour 调用此方法通知 AudioDaemon：origin (openAudioOutput) 调用已完成
    void notifyOpenDone();

    /// detour 调用此方法询问：是否应该调用原始的 closeAudioOutput？
    /// @return true = 需要调用 origin；false = 抑制本次调用
    bool shouldCloseAudioOutput();

    /// detour 调用此方法通知 AudioDaemon：origin (closeAudioOutput) 调用已完成
    void notifyCloseDone();

signals:
    void refCountChanged(int newRefCount);
    void stateChanged(AudioDaemonState newState);

private:
    friend Singleton<AudioDaemon>;
    explicit AudioDaemon();

    // --- 状态机 ---
    void _transitionTo(AudioDaemonState newState);

    // --- 定时器回调 ---
    void _onCloseTimeout();
    void _onPollTick();

    // --- 状态 ---
    AudioDaemonState mState{AudioDaemonState::IDLE};
    int              mRefCount{0};
    bool             mEnabled{true};
    bool             mPendingForceClose{false};  ///< 延迟关闭超时后，标记下一次 detour 放行

    // --- 配置 ---
    int mCloseDelayMs{3000};   ///< 延迟关闭等待时间（毫秒）
    int mPollIntervalMs{500};  ///< 轮询间隔（毫秒）

    // --- 定时器 ---
    QTimer* mCloseDelayTimer{nullptr};  ///< 延迟关闭计时器
    QTimer* mPollTimer{nullptr};        ///< 状态轮询计时器

    // --- ALSA 外部播放监控 ---
    bool   mAlsaActive{false};      ///< ALSA PCM 设备当前是否有音频流（外部程序播放）
    bool   _checkAlsaPcmStatus();   ///< 检查 /proc/asound 中各 PCM 设备状态

    // --- 统计（调试用）---
    uint64 mTotalOpenCalls{0};    ///< 实际的 open 调用次数
    uint64 mTotalCloseCalls{0};  ///< 实际的 close 调用次数
};

} // namespace mod