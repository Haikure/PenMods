// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "system/sound/AudioDaemon.h"

#include "base/Hook.h"
#include "base/YPointer.h"
#include "common/Event.h"
#include "common/Utils.h"

namespace mod {

AudioDaemon::AudioDaemon() : Logger("AudioDaemon") {
    // 延迟关闭定时器（单次触发）
    mCloseDelayTimer = new QTimer(this);
    mCloseDelayTimer->setSingleShot(true);
    connect(mCloseDelayTimer, &QTimer::timeout, this, &AudioDaemon::_onCloseTimeout);

    // 状态轮询定时器（持续触发）
    mPollTimer = new QTimer(this);
    connect(mPollTimer, &QTimer::timeout, this, &AudioDaemon::_onPollTick);

    // 读取配置（如果存在）
    json cfg = Config::getInstance().read("AudioDaemon");
    if (cfg.contains("enabled"))        mEnabled      = cfg["enabled"].get<bool>();
    if (cfg.contains("closeDelayMs"))   mCloseDelayMs = cfg["closeDelayMs"].get<int>();

    // 连接 UI 初始化完成事件
    connect(&Event::getInstance(), &Event::uiCompleted, this, [this]() {
        debug("AudioDaemon 就绪。closeDelayMs={}, refCount={}", mCloseDelayMs, mRefCount);
    });
}

int AudioDaemon::acquire(AudioSource source) {
    if (!mEnabled) {
        debug("acquire({}) 忽略（daemon 未启用）", static_cast<int>(source));
        return mRefCount;
    }

    mRefCount++;
    debug("acquire({}) → refCount={}", static_cast<int>(source), mRefCount);
    emit refCountChanged(mRefCount);
    return mRefCount;
}

int AudioDaemon::release(AudioSource source) {
    if (!mEnabled) {
        debug("release({}) 忽略（daemon 未启用）", static_cast<int>(source));
        return mRefCount;
    }

    if (mRefCount <= 0) {
        warn("release({}) 调用时 refCount 已为 0！", static_cast<int>(source));
        return 0;
    }

    mRefCount--;
    debug("release({}) → refCount={}", static_cast<int>(source), mRefCount);
    emit refCountChanged(mRefCount);
    return mRefCount;
}

// ========== 决策接口（供 detour 调用） ==========

bool AudioDaemon::shouldOpenAudioOutput() {
    if (!mEnabled) return true;  // 不启用时，直接放行

    switch (mState) {
    case AudioDaemonState::IDLE:
        // 需要打开
        _transitionTo(AudioDaemonState::PLAYING);
        return true;

    case AudioDaemonState::PLAYING:
        // 已经在播放中，无需重复打开
        debug("抑制 openAudioOutput（已在 PLAYING 状态）");
        return false;

    case AudioDaemonState::CLOSE_DELAY:
        // 正在等待关闭，取消延迟定时器，回到 PLAYING
        mCloseDelayTimer->stop();
        debug("取消延迟关闭，恢复为 PLAYING");
        _transitionTo(AudioDaemonState::PLAYING);
        return false;  // 音频设备应该还开着，无需再 open
    }
    return true;
}

void AudioDaemon::notifyOpenDone() {
    mTotalOpenCalls++;
    debug("openAudioOutput 完成（累计={}）", mTotalOpenCalls);
    // 确保状态正确
    if (mState != AudioDaemonState::PLAYING) {
        _transitionTo(AudioDaemonState::PLAYING);
    }
}

bool AudioDaemon::shouldCloseAudioOutput() {
    if (!mEnabled) return true;

    // 检查是否有待处理的强制关闭请求（延迟超时后需要真正关闭）
    if (mPendingForceClose) {
        mPendingForceClose = false;
        debug("强制执行 closeAudioOutput（延迟关闭超时后放行）");
        return true;
    }

    // 如果 ALSA 设备仍有外部程序在播放，不关闭输出
    if (mAlsaActive) {
        debug("抑制 closeAudioOutput（ALSA 设备仍活跃→外部程序正在播放）");
        return false;
    }

    switch (mState) {
    case AudioDaemonState::IDLE:
        // 已经是空闲，无需关闭
        debug("抑制 closeAudioOutput（已在 IDLE 状态）");
        return false;

    case AudioDaemonState::PLAYING:
        // 判断引用计数是否为 0
        if (mRefCount == 0) {
            // 引用计数为 0，进入延迟关闭
            _transitionTo(AudioDaemonState::CLOSE_DELAY);
            return false;
        }
        // 还有引用占用，无需关闭
        debug("抑制 closeAudioOutput（refCount={} > 0）", mRefCount);
        return false;

    case AudioDaemonState::CLOSE_DELAY:
        // 已经在延迟关闭中，由定时器决定
        return false;
    }
    return true;
}

void AudioDaemon::notifyCloseDone() {
    mTotalCloseCalls++;
    debug("closeAudioOutput 完成（累计={}）", mTotalCloseCalls);
    _transitionTo(AudioDaemonState::IDLE);
}

// ========== 状态机 ==========

void AudioDaemon::_transitionTo(AudioDaemonState newState) {
    if (mState == newState) return;

    debug("状态: {} → {}",
          static_cast<int>(mState),
          static_cast<int>(newState));
    mState = newState;
    emit stateChanged(mState);

    switch (newState) {
    case AudioDaemonState::IDLE:
        mPollTimer->stop();
        mCloseDelayTimer->stop();
        break;

    case AudioDaemonState::PLAYING:
        if (!mPollTimer->isActive()) {
            mPollTimer->start(mPollIntervalMs);
        }
        break;

    case AudioDaemonState::CLOSE_DELAY:
        mCloseDelayTimer->start(mCloseDelayMs);
        break;
    }
}

// ========== 定时器回调 ==========

void AudioDaemon::_onCloseTimeout() {
    debug("延迟关闭超时已到期（refCount={}），设置 mPendingForceClose 等待系统 close 调用", mRefCount);
    // 无法直接调用 origin，因此设置 mPendingForceClose 标志
    // 当系统下一次调用 closeAudioOutput 时，shouldCloseAudioOutput 会返回 true
    mPendingForceClose = true;
    // 切换到 IDLE，以便系统正常执行 close
    _transitionTo(AudioDaemonState::IDLE);
}

void AudioDaemon::_onPollTick() {
    // 检查 ALSA 设备状态（检测外部程序的音频播放）
    bool nowActive = _checkAlsaPcmStatus();

    if (nowActive && !mAlsaActive) {
        // 外部程序开始播放，标记 ALSA 活跃
        mAlsaActive = true;
        debug("ALSA 设备检测到音频流 → 外部程序正在播放，保持输出打开");

        // 如果当前不在 PLAYING 状态，需要确保输出打开
        if (mState == AudioDaemonState::IDLE) {
            // 需要打开音频输出——但这里不能调 origin，只能靠下一次 openAudioOutput detour
            // 不过我们已经在 IDLE，外部播放器应该已经通过 ALSA 直接播放了
            // 我们只需要阻止 close 即可
        } else if (mState == AudioDaemonState::CLOSE_DELAY) {
            // 取消延迟关闭，回到 PLAYING
            mCloseDelayTimer->stop();
            _transitionTo(AudioDaemonState::PLAYING);
        }
    }

    if (!nowActive && mAlsaActive) {
        // 外部程序停止播放
        mAlsaActive = false;
        debug("ALSA 设备进入空闲 → 外部程序已停止");
        // 如果 refCount 也为 0，进入延迟关闭
        if (mRefCount == 0 && mState == AudioDaemonState::PLAYING) {
            _transitionTo(AudioDaemonState::CLOSE_DELAY);
        }
    }

    // 引用计数 + ALSA 均为空闲，进入延迟关闭
    if (mState == AudioDaemonState::PLAYING && mRefCount == 0 && !mAlsaActive) {
        debug("轮询检测到 refCount=0 且 ALSA 空闲，进入延迟关闭");
        _transitionTo(AudioDaemonState::CLOSE_DELAY);
    }
}

bool AudioDaemon::_checkAlsaPcmStatus() {
    // 检查常见 PCM 播放设备的状态文件
    // 根据 ASound.cpp 中的 ALSA 配置：
    //   hw:0,0   → card0/pcm0p (主声卡播放)
    //   hw:7,0,0 → card7/pcm0p (dmixer 环路设备)
    //   hw:1,0   → card1/pcm0p (数字耳机)
    //   hw:0,0   → card0/pcm0p (模拟耳机 playback path 也走 card0)
    static const char* pcmStatusPaths[] = {
        "/proc/asound/card0/pcm0p/sub0/status",
        "/proc/asound/card0/pcm0p/sub1/status",
        "/proc/asound/card1/pcm0p/sub0/status",
        "/proc/asound/card7/pcm0p/sub0/status",
    };

    for (auto path : pcmStatusPaths) {
        std::ifstream f(path);
        if (!f.good()) continue;
        std::string content;
        std::getline(f, content);  // 第一行通常是 "state: RUNNING"
        if (content.find("RUNNING") != std::string::npos ||
            content.find("DRAINING") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace mod

// ========== PEN_HOOK 定义 ==========

// Hook: openAudioOutput — 所有系统打开音频输出的请求都经由此处
PEN_HOOK(uint64, _ZN12YSoundCenter15openAudioOutputEv, void* self) {
    auto& daemon = mod::AudioDaemon::getInstance();

    // 询问 AudioDaemon 是否应该调用原始函数
    if (daemon.shouldOpenAudioOutput()) {
        auto result = origin(self);
        daemon.notifyOpenDone();
        return result;
    }

    // 被抑制，返回 0（表示成功，但未执行实际操作）
    return 0;
}

// Hook: closeAudioOutput — 所有系统关闭音频输出的请求都经由此处
PEN_HOOK(uint64, _ZN12YSoundCenter16closeAudioOutputEv, void* self) {
    auto& daemon = mod::AudioDaemon::getInstance();

    // 询问 AudioDaemon 是否应该调用原始函数
    if (daemon.shouldCloseAudioOutput()) {
        auto result = origin(self);
        daemon.notifyCloseDone();
        return result;
    }

    // 被抑制，返回 0
    return 0;
}