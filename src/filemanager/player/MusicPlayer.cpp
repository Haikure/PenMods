// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "filemanager/player/MusicPlayer.h"

#include "base/YPointer.h"

#include "common/Event.h"
#include "common/Utils.h"

#include "mod/Config.h"

#include "system/input/ScreenManager.h"
#include "system/sound/AudioDaemon.h"

#include <QFile>
#include <QQmlContext>
#include <QRandomGenerator>

#define PLAYER_FAKE_COLUMN_ID ("fake_column_hsxjsbw")

namespace mod::filemanager {

namespace {
bool showNativeAudioPlayer() {
#if PL_BUILD_YDP03X
    auto  instance  = PEN_CALL(QObject*, "_ZN10YSingletonI20YBaseSignalAggregateE8instanceEv");
    auto* aggregate = instance ? instance() : nullptr;
    return aggregate && QMetaObject::invokeMethod(aggregate, "showAudioPlayer", Qt::AutoConnection);
#else
    auto  show   = PEN_CALL(void, "_ZN7YGlobal15showAudioPlayerEv", void*);
    auto* global = YPointer<YGlobal>::getInstance();
    if (!show || !global) return false;
    show(global);
    return true;
#endif
}

uint32& playbackSequence(void* manager) {
    // YDP03X onSoundEnd at 0x51c514: d_ptr at +0x28, sequence at d_ptr+0x64.
    // YDP02X has the same sequence field with d_ptr at +0x20.
#if PL_BUILD_YDP03X
    constexpr size_t privateOffset = 0x28;
#else
    constexpr size_t privateOffset = 0x20;
#endif
    auto* data = *reinterpret_cast<char**>(static_cast<char*>(manager) + privateOffset);
    return *reinterpret_cast<uint32*>(data + 0x64);
}

void stopHostPlayer(void* manager) {
    PEN_CALL(void, "_ZN19YMediaPlayerManager14onClickedPauseEv", void*)(manager);
    playbackSequence(manager) = 0;
    const auto state          = PlayState::STOPPED;
#if PL_BUILD_YDP03X
    PEN_CALL(void, "_ZN19YMediaPlayerManager12setPlayStateERKN9YBaseEnum11PlayerStateE", void*, const PlayState&)
    (manager, state);
#else
    PEN_CALL(void, "_ZN19YMediaPlayerManager12setPlayStateERKN12YEnumWrapper10Play_StateE", void*, const PlayState&)
    (manager, state);
#endif
}
} // namespace

bool MusicPlayer::mIsTakeOver{false};

MusicPlayer::MusicPlayer() : Logger("MusicPlayer") {
    const auto cfg       = Config::getInstance().read("fm");
    mPauseOnScan         = cfg.value("pause_on_scan", false);
    mHideFloatingWindow  = cfg.value("hide_floating_window", false);
    mQuickPanelMusicView = cfg.value("quick_panel_music_view", false);

    connect(&Event::getInstance(), &Event::ocrStarted, this, &MusicPlayer::onOcrStarted);
    mShutdownTimer.setSingleShot(true);
    connect(&mShutdownTimer, &QTimer::timeout, this, [this]() {
        // 倒计时结束：定时关机；"播放结束关机" 模式不使用该定时器（见 setShutdownAfterPlaylist）。
        mShutdownTimerMinutes = 0;
        emit shutdownTimerChanged();
        ScreenManager::getInstance().requestPowerOff(true);
    });
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("musicPlayer", this);
    });
}

enum class LrcState {
    BILINGUAL,
    ORIGINAL,
    TRANS,
    HIDE,
};

enum class DownloadState { NOT, SUCCEED, ING, FAILURE, CANCEL, PAUSE };

struct YMediaEntity {
    YMediaEntity() = delete;
    char          unk[10];
    QString       mMediaId;         // 0x10
    QString       mOwnerId;         // 0x18
    QString       mTitle;           // 0x20
    int           mDuration;        // 0x28
    DownloadState mDownloadState;   // 0x2C
    QString       mUrl;             // 0x30
    QString       mLocalFile;       // 0x38
    QString       mLrcFile;         // 0x40
    LrcState      mLrcState;        // 0x48
    bool          mSrcAudioVisible; // 0x4C
};

struct YColumnMediaEntity : public YMediaEntity {
    int     mId;       // 0x50, repeat of mMediaId;
    QString mColumnId; // 0x58, repeat of mOwnerId;
    int     mProgress; // 0x60
    bool    mIsDir;    // 0x64
};

static_assert(sizeof(YColumnMediaEntity) == 0x68);

bool MusicPlayer::play(size_t idx) {
    if (idx >= mPlayList.size()) return false;
    auto file = mPlayList.at(idx);
    file->refresh();
    if (!file->exists() || !file->isFile() || !file->isReadable()) {
        showToast("音频文件不存在或无法读取", "#E9900C");
        return false;
    }
    auto  state   = PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", void*);
    auto* manager = YPointer<YMediaPlayerManager>::getInstance();
    if (mIsTakeOver && !mCurrentPlaying.mIsEnd && mCurrentPlaying.mFile
        && mCurrentPlaying.mFile->absoluteFilePath() == file->absoluteFilePath() && manager && state
        && state(manager) != PlayState::STOPPED) {
        if (!showNativeAudioPlayer()) showToast("无法打开系统音乐播放器", "#E9900C");
        return true;
    }
    if (!_play(file)) return false;
    mCurrentPlaying.setPlaying(idx, file);
    return true;
}

bool MusicPlayer::_play(const std::shared_ptr<QFileInfo>& file) {
#if PL_BUILD_YDP03X
    // Native ctor/dtor and setters are verified against the YDP03X binary.
    // Construct directly: the first imported track must also work before Qt
    // registers YColumnMediaEntity, and enum writes must use the native ABI.
    auto construct    = PEN_CALL(void, "_ZN18YColumnMediaEntityC2EP7QObject", void*, QObject*);
    auto destroy      = PEN_CALL(void, "_ZN18YColumnMediaEntityD1Ev", void*);
    auto setId        = PEN_CALL(void, "_ZN18YColumnMediaEntity5setIdERKi", void*, const int&);
    auto setColumnId  = PEN_CALL(void, "_ZN18YColumnMediaEntity11setColumnIdERK7QString", void*, const QString&);
    auto setIsDir     = PEN_CALL(void, "_ZN18YColumnMediaEntity8setIsDirERKb", void*, const bool&);
    auto setTitle     = PEN_CALL(void, "_ZN12YMediaEntity8setTitleERK7QString", void*, const QString&);
    auto setLocalFile = PEN_CALL(void, "_ZN12YMediaEntity12setLocalFileERK7QString", void*, const QString&);
    auto setLrcFile   = PEN_CALL(void, "_ZN12YMediaEntity10setLrcFileERK7QString", void*, const QString&);
    auto setDownloaded =
        PEN_CALL(void, "_ZN12YMediaEntity16setDownloadStateERKN9YBaseEnum13DownloadStateE", void*, const DownloadState&);
    auto  wipe      = PEN_CALL(void, "_ZN19YMediaPlayerManager8wipeDataEv", void*);
    auto  setColumn = PEN_CALL(void, "_ZN7YGlobal23setAudioPlayingColomnIdERK7QString", void*, const QString&);
    auto  playAudio = PEN_CALL(void, "_ZN13YMediaManager9playAudioERK18YColumnMediaEntityb", void*, const void*, bool);
    auto  state     = PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", void*);
    auto  setHasLrc = PEN_CALL(void, "_ZN19YMediaPlayerManager9setHasLrcEb", void*, bool);
    auto* manager   = YPointer<YMediaPlayerManager>::getInstance();
    auto* mediaManager = YPointer<YMediaManager>::getInstance();
    auto* global       = YPointer<YGlobal>::getInstance();
    if (!construct || !destroy || !setId || !setColumnId || !setIsDir || !setTitle || !setLocalFile || !setLrcFile
        || !setDownloaded || !wipe || !setColumn || !playAudio || !state || !setHasLrc || !manager || !mediaManager
        || !global) {
        showToast("系统音乐播放器尚未就绪", "#E9900C");
        return false;
    }
    QString lrc;
    cleanupTempSymlinks();
    const QString       audio = createTempSymlinks(file, lrc);
    alignas(void*) char entity[0x68];
    construct(entity, nullptr);
    static int mediaId    = 0;
    const int  id         = --mediaId;
    const bool isDir      = false;
    const auto downloaded = DownloadState::SUCCEED;
    setId(entity, id);                                   // Also sets mediaId in the native implementation.
    setColumnId(entity, QString(PLAYER_FAKE_COLUMN_ID)); // Also sets ownerId.
    setIsDir(entity, isDir);
    setTitle(entity, file->fileName());
    setLocalFile(entity, audio);
    setLrcFile(entity, lrc);
    setDownloaded(entity, downloaded);
    releaseAudio();
    mIsTakeOver = true;
    AudioDaemon::getInstance().acquire(AudioSource::MUSIC);
    wipe(manager);
    setColumn(global, QStringLiteral("myimport"));
    playAudio(mediaManager, entity, true); // Native doPlayAudio starts playback once.
    destroy(entity);                       // The native manager owns its own copies.
    setHasLrc(manager, !lrc.isEmpty());
    if (state(manager) != PlayState::PLAYING) {
        mIsTakeOver            = false;
        mCurrentPlaying.mIsEnd = true;
        releaseAudio();
        showToast("无法播放该音频，请检查文件格式", "#E9900C");
        return false;
    }
    if (!showNativeAudioPlayer()) showToast("无法打开系统音乐播放器", "#E9900C");
#else
    mIsTakeOver                = true;
    mPausedByScan              = false;
    mPlaybackResumedDuringScan = false;
    mScanResultClosed          = false;
    // 播放新曲目前先释放之前的 MUSIC 引用，防止该 source 的引用累积。
    if (mod::AudioDaemon::getInstance().sourceRefCount(AudioSource::MUSIC) > 0) {
        mod::AudioDaemon::getInstance().release(AudioSource::MUSIC);
    }
    // 通知音频守护进程：音乐播放器正在使用音频输出
    mod::AudioDaemon::getInstance().acquire(AudioSource::MUSIC);
    PEN_CALL(void, "_ZN19YMediaPlayerManager8wipeDataEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
    PEN_CALL(bool, "_ZN7YGlobal23setAudioPlayingColomnIdERK7QString", void*, QString const&)
    (YPointer<YGlobal>::getInstance(), "myimport");
    auto memory = new char[sizeof(YColumnMediaEntity)];
    PEN_CALL(void, "_ZN18YColumnMediaEntityC2EP7QObject", void*, void*)(memory, nullptr);
    auto       entity  = reinterpret_cast<YColumnMediaEntity*>(memory);
    bool       hasLrc  = false;
    static int mediaId = 0;
    mediaId--;
    entity->mId            = mediaId;
    entity->mMediaId       = QString::number(mediaId);
    entity->mOwnerId       = PLAYER_FAKE_COLUMN_ID;
    entity->mColumnId      = PLAYER_FAKE_COLUMN_ID;
    entity->mIsDir         = false;
    entity->mDownloadState = DownloadState::SUCCEED;
    entity->mTitle         = file->fileName();
    // 清理上次的临时软链接
    cleanupTempSymlinks();
    QString lrcPath;
    entity->mLocalFile = createTempSymlinks(file, lrcPath);
    if (!lrcPath.isEmpty()) {
        entity->mLrcFile = lrcPath;
        hasLrc           = true;
    }
    PEN_CALL(void*, "_ZN13YMediaManager9playAudioERK18YColumnMediaEntityb", void*, YColumnMediaEntity*, bool)
    (YPointer<YMediaManager>::getInstance(), entity, true);
    showNativeAudioPlayer();
    delete entity;
    entity = nullptr; // entity is copied.
    if (PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", void*)(YPointer<YMediaPlayerManager>::getInstance()
        )
        != PlayState::PLAYING) {
        PEN_CALL(void, "_ZN19YMediaPlayerManager13onClickedPlayEv", void*)
        (YPointer<YMediaPlayerManager>::getInstance());
        PEN_CALL(void, "_ZN19YMediaPlayerManager9setHasLrcEb", void*, bool)
        (YPointer<YMediaPlayerManager>::getInstance(), hasLrc);
    }
#endif
    return true;
}

QString MusicPlayer::createTempSymlinks(const PlayFile& file, QString& outLrcPath) {
    outLrcPath.clear();
    const QString matchName = file->completeBaseName();
    double        bestScore = 0.0;
    for (const auto& entry : file->absoluteDir().entryInfoList(QDir::Files | QDir::Readable)) {
        if (entry.suffix().toLower() != "lrc") continue;
        if (entry.completeBaseName() == matchName) {
            outLrcPath = entry.absoluteFilePath();
            break;
        }
        const double score = fuzzyLrcMatch(matchName, entry.completeBaseName());
        if (score >= 0.35 && score > bestScore) {
            bestScore  = score;
            outLrcPath = entry.absoluteFilePath();
        }
    }
    const QString suffix = file->suffix().toLower();
    // 如果已经是 .mp3，无需创建软链接，直接返回原路径
    if (suffix == "mp3") {
        return file->absoluteFilePath();
    }
    // 生成唯一的临时文件名（基于原文件绝对路径的哈希）
    const QString tmpAudioPath =
        QStringLiteral("/tmp/PenMods-audio-%1.mp3").arg(qHash(file->absoluteFilePath()), 0, 16);
    // 创建音频文件的 .mp3 软链接
    QFile::remove(tmpAudioPath);
    if (QFile::link(file->absoluteFilePath(), tmpAudioPath)) {
        mTempAudioLink = tmpAudioPath;
        debug(
            "Created temp audio symlink: {} -> {}",
            tmpAudioPath.toStdString(),
            file->absoluteFilePath().toStdString()
        );
    } else {
        error("Failed to create temp audio symlink: {}", tmpAudioPath.toStdString());
        return file->absoluteFilePath();
    }
    return mTempAudioLink;
}

void MusicPlayer::cleanupTempSymlinks() {
    if (!mTempAudioLink.isEmpty()) {
        QFile::remove(mTempAudioLink);
        debug("Cleaned up temp audio symlink: {}", mTempAudioLink.toStdString());
        mTempAudioLink.clear();
    }
}

void MusicPlayer::clickNext() {
    int64 pos = 0;
    PEN_CALL(void*, "_ZN19YMediaPlayerManager13setCurrentPosERKx", void*, int64 const&)
    (YPointer<YMediaPlayerManager>::getInstance(), pos);
    PEN_CALL(void*, "_ZN19YMediaPlayerManager11closeRepeatEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
    if (mPlayList.empty()) return;
    auto newIdx = mCurrentPlaying.mIndex + 1;
    if (newIdx > mPlayList.size() - 1) newIdx = 0;
    play(newIdx);
}

void MusicPlayer::clickPrev() {
    int64 pos = 0;
    PEN_CALL(void*, "_ZN19YMediaPlayerManager13setCurrentPosERKx", void*, void*)
    (YPointer<YMediaPlayerManager>::getInstance(), &pos);
    PEN_CALL(void*, "_ZN19YMediaPlayerManager11closeRepeatEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
    if (mPlayList.empty()) return;
    auto newIdx = mCurrentPlaying.mIndex - 1;
    if (newIdx > mPlayList.size() - 1) // overflow
        newIdx = mPlayList.size() - 1;
    play(newIdx); // back <- front
}

void MusicPlayer::clickRand() {
    int64 pos = 0;
    PEN_CALL(void*, "_ZN19YMediaPlayerManager13setCurrentPosERKx", void*, void*)
    (YPointer<YMediaPlayerManager>::getInstance(), &pos);
    PEN_CALL(void*, "_ZN19YMediaPlayerManager11closeRepeatEv", void*)(YPointer<YMediaPlayerManager>::getInstance());
    if (mPlayList.empty()) return;
    std::optional<size_t> newIdx;
    while (!newIdx) {
        uint32 switched = QRandomGenerator::global()->bounded((int)mPlayList.size()); // safe, limited by fm.
        if (mCurrentPlaying.mIndex != switched || mPlayList.size() == 1) {
            newIdx = switched;
            break;
        }
    }
    play(*newIdx);
}

void MusicPlayer::onSoundEnd() {
    mCurrentPlaying.mIsEnd = true;
    if (mShutdownPending && getCurrentAudioSequence() == AudioSequence::ORDER
        && mCurrentPlaying.mIndex + 1 >= mPlayList.size()) {
        mShutdownPending = false;
        ScreenManager::getInstance().requestPowerOff(true);
        emit shutdownTimerChanged();
        return;
    }
    switch (getCurrentAudioSequence()) {
    case AudioSequence::ORDER:
        clickNext();
        break;
    case AudioSequence::RANDOM:
        clickRand();
        break;
    case AudioSequence::SINGLE:
        play(mCurrentPlaying.mIndex);
        break;
    case AudioSequence::SINGLE_SHOT:
        stopHostPlayer(YPointer<YMediaPlayerManager>::getInstance());
        releaseAudio();
        break;
    }
}

AudioSequence MusicPlayer::getCurrentAudioSequence() {
    return PEN_CALL(AudioSequence, "_ZNK15YSettingManager13audioSequenceEv", void*)(
        YPointer<YSettingManager>::getInstance()
    );
}

bool MusicPlayer::getPauseOnScan() const { return mPauseOnScan; }

void MusicPlayer::setPauseOnScan(bool enabled) {
    if (mPauseOnScan == enabled) {
        return;
    }
    mPauseOnScan         = enabled;
    auto cfg             = Config::getInstance().read("fm");
    cfg["pause_on_scan"] = enabled;
    Config::getInstance().write("fm", std::move(cfg));
    emit pauseOnScanChanged();
}

bool MusicPlayer::getHideFloatingWindow() const { return mHideFloatingWindow; }

void MusicPlayer::setHideFloatingWindow(bool hidden) {
    if (mHideFloatingWindow == hidden) {
        return;
    }
    mHideFloatingWindow         = hidden;
    auto cfg                    = Config::getInstance().read("fm");
    cfg["hide_floating_window"] = hidden;
    Config::getInstance().write("fm", std::move(cfg));
    emit hideFloatingWindowChanged();
}

bool MusicPlayer::getQuickPanelMusicView() const { return mQuickPanelMusicView; }

void MusicPlayer::setQuickPanelMusicView(bool showMusicControls) {
    if (mQuickPanelMusicView == showMusicControls) {
        return;
    }
    mQuickPanelMusicView          = showMusicControls;
    auto cfg                      = Config::getInstance().read("fm");
    cfg["quick_panel_music_view"] = showMusicControls;
    Config::getInstance().write("fm", std::move(cfg));
    emit quickPanelMusicViewChanged();
}

void MusicPlayer::setShutdownAfterPlaylist(bool enabled) {
    if (mShutdownAfterPlaylist == enabled) return;
    mShutdownAfterPlaylist = enabled;
    if (enabled) {
        // "播放结束关机" 不是倒计时模式：停止并清空定时器，立即进入等待播放列表结束的状态，
        // 这样顺序播放到最后一首结束时 onSoundEnd() 才会执行关机。
        mShutdownTimer.stop();
        mShutdownTimerMinutes = 0;
        mShutdownPending      = true;
    } else {
        mShutdownPending = false;
    }
    emit shutdownTimerChanged();
}

void MusicPlayer::setShutdownTimerMinutes(int minutes) {
    if (minutes <= 0) {
        cancelShutdownTimer();
        return;
    }
    if (minutes > 1440) return;
    mShutdownAfterPlaylist = false;
    mShutdownPending       = false;
    mShutdownTimerMinutes  = minutes;
    mShutdownTimer.start(minutes * 60 * 1000);
    emit shutdownTimerChanged();
}

void MusicPlayer::cancelShutdownTimer() {
    mShutdownTimer.stop();
    mShutdownAfterPlaylist = false;
    mShutdownPending       = false;
    mShutdownTimerMinutes  = 0;
    emit shutdownTimerChanged();
}

void MusicPlayer::onOcrStarted() {
    if (!mPauseOnScan || !mIsTakeOver) {
        return;
    }
    auto*      manager = YPointer<YMediaPlayerManager>::getInstance();
    const auto state   = PEN_CALL(PlayState, "_ZNK19YMediaPlayerManager9playStateEv", void*)(manager);
    if (state != PlayState::PLAYING) {
        return;
    }

    mScanPausePosition         = PEN_CALL(int64_t, "_ZNK19YMediaPlayerManager10currentPosEv", void*)(manager);
    mPausedByScan              = true;
    mPlaybackResumedDuringScan = false;
    mScanResultClosed          = false;
    PEN_CALL(void, "_ZN19YMediaPlayerManager14onClickedPauseEv", void*)(manager);

    // ocrStart 的同步 QML 处理会切换页面并停止提示音，待这些处理结束后恢复被覆盖的播放位置。
    QTimer::singleShot(0, this, &MusicPlayer::restoreScanPausePosition);
}

void MusicPlayer::restoreScanPausePosition() {
    if (!mPausedByScan) {
        return;
    }
    auto* manager = YPointer<YMediaPlayerManager>::getInstance();
    PEN_CALL(void, "_ZN19YMediaPlayerManager13setCurrentPosERKx", void*, int64_t const&)(manager, mScanPausePosition);
}

int64_t MusicPlayer::normalizePositionDuringScan(int64_t requestedPosition) {
    if (requestedPosition != 0) {
        return requestedPosition;
    }
    if (mPlaybackResumedDuringScan) {
        return PEN_CALL(int64_t, "_ZNK19YMediaPlayerManager10currentPosEv", void*)(
            YPointer<YMediaPlayerManager>::getInstance()
        );
    }
    return mScanPausePosition > 0 ? mScanPausePosition : requestedPosition;
}

void MusicPlayer::onPlaybackResumedAfterScan() {
    if (!mPausedByScan) {
        return;
    }
    mPlaybackResumedDuringScan = true;
    QTimer::singleShot(0, this, [this]() {
        if (!mPausedByScan) {
            return;
        }
        restoreScanPausePosition();
        if (mScanResultClosed) {
            mPausedByScan              = false;
            mPlaybackResumedDuringScan = false;
            mScanResultClosed          = false;
        }
    });
}

bool MusicPlayer::shouldPreserveMusicOnScanResultClose() { return mPausedByScan && mPlaybackResumedDuringScan; }

void MusicPlayer::finishScanPause() {
    if (!mPausedByScan) {
        return;
    }
    mScanResultClosed = true;
    if (mPlaybackResumedDuringScan) {
        // 已恢复的音乐无需再被结果页关闭流程干预，保留它的自然播放进度。
        mPausedByScan              = false;
        mPlaybackResumedDuringScan = false;
        mScanResultClosed          = false;
        return;
    }

    // 仍暂停时允许结果页关闭音效流程完成，但继续保护位置直到用户恢复播放。
    QTimer::singleShot(0, this, &MusicPlayer::restoreScanPausePosition);
}

void MusicPlayer::seekToPosition(qint64 position) {
    auto* manager = YPointer<YMediaPlayerManager>::getInstance();
    if (position < 0) {
        position = 0;
    }

    const auto currentPosition = PEN_CALL(int64_t, "_ZNK19YMediaPlayerManager10currentPosEv", void*)(manager);
    const auto targetPosition  = static_cast<int64_t>(position);
    if (targetPosition > currentPosition) {
        const auto delta = targetPosition - currentPosition;
        PEN_CALL(int64_t, "_ZN19YMediaPlayerManager13onFastForwardEx", void*, int64_t)(manager, delta);
    } else if (targetPosition < currentPosition) {
        const auto delta = currentPosition - targetPosition;
        PEN_CALL(int64_t, "_ZN19YMediaPlayerManager14onFastBackwardEx", void*, int64_t)(manager, delta);
    }
}

void MusicPlayer::stop() {
    emit stopRequested();

    auto* manager = YPointer<YMediaPlayerManager>::getInstance();
    if (!manager) {
        releaseAudio();
        return;
    }

    mCurrentPlaying.mIsEnd = true;
    stopHostPlayer(manager);
    PEN_CALL(void*, "_ZN19YMediaPlayerManager8wipeDataEv", void*)(manager);
    mIsTakeOver = false;
    releaseAudio();
}

void MusicPlayer::releaseAudio() {
    mPausedByScan              = false;
    mPlaybackResumedDuringScan = false;
    mScanResultClosed          = false;
    info("QML 请求释放 MUSIC 引用");
    auto& audioDaemon = mod::AudioDaemon::getInstance();
    while (audioDaemon.sourceRefCount(AudioSource::MUSIC) > 0) {
        audioDaemon.release(AudioSource::MUSIC);
    }
}

void MusicPlayer::releaseAudioAfterHide() {
    if (mPausedByScan) {
        info("扫描暂停期间隐藏播放器，保留 MUSIC 引用");
        return;
    }
    releaseAudio();
}

} // namespace mod::filemanager

// Hooks

using MusicPlayer = mod::filemanager::MusicPlayer;

PEN_HOOK(void*, _ZN13YMediaManager10clickMediaEi, void* a1, void* a2) {
    MusicPlayer::mIsTakeOver = false;
    return origin(a1, a2);
}

PEN_HOOK(void, _ZN19YMediaPlayerManager13setCurrentPosERKx, void* self, int64_t const& position) {
    auto& player = MusicPlayer::getInstance();
    if (MusicPlayer::mIsTakeOver && player.isPausedByScan()) {
        const auto normalized = player.normalizePositionDuringScan(position);
        return origin(self, normalized);
    }
    return origin(self, position);
}

PEN_HOOK(void, _ZN19YMediaPlayerManager13onClickedPlayEv, void* self) {
    if (MusicPlayer::mIsTakeOver && mod::AudioDaemon::getInstance().sourceRefCount(mod::AudioSource::MUSIC) == 0)
        mod::AudioDaemon::getInstance().acquire(mod::AudioSource::MUSIC);
    origin(self);
    if (MusicPlayer::mIsTakeOver) {
        MusicPlayer::getInstance().onPlaybackResumedAfterScan();
    }
}

PEN_HOOK(void, _ZN19YMediaPlayerManager14onClickedPauseEv, void* self) {
    origin(self);
    if (MusicPlayer::mIsTakeOver) {
        auto& audio = mod::AudioDaemon::getInstance();
        while (audio.sourceRefCount(mod::AudioSource::MUSIC) > 0) audio.release(mod::AudioSource::MUSIC);
    }
}

PEN_HOOK(uint64, _ZN19YMediaPlayerManager13onClickedPrevEb, void* self, bool a2) {
    if (!MusicPlayer::mIsTakeOver) return origin(self, a2);
    if (MusicPlayer::getCurrentAudioSequence() == AudioSequence::RANDOM) MusicPlayer::getInstance().clickRand();
    else MusicPlayer::getInstance().clickPrev();
    return 0;
}

PEN_HOOK(uint64, _ZN19YMediaPlayerManager13onClickedNextEb, void* self, bool a2) {
    if (!MusicPlayer::mIsTakeOver) return origin(self, a2);
    if (MusicPlayer::getCurrentAudioSequence() == AudioSequence::RANDOM) MusicPlayer::getInstance().clickRand();
    else MusicPlayer::getInstance().clickNext();
    return 0;
}

PEN_HOOK(void*, _ZN19YMediaPlayerManager10onSoundEndEj, void* self, uint32 a2) {
    if (!MusicPlayer::mIsTakeOver) return origin(self, a2);
    if (mod::filemanager::playbackSequence(self) == a2 // Is current sequence equal?
        && PEN_CALL(PlayState,
                    "_ZNK19YMediaPlayerManager9playStateEv",
                    void*)(self)
               == PlayState::PLAYING) // Is in `Playing` state? To prevent unexcept onSoundEnd...
        MusicPlayer::getInstance().onSoundEnd();
    return self;
}

// 系统清理播放数据时，同步清理临时软链接
PEN_HOOK(void*, _ZN19YMediaPlayerManager8wipeDataEv, void* self) { return origin(self); }
