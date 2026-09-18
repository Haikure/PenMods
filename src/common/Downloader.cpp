// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "common/Downloader.h"

#include "base/YPointer.h"

namespace mod {

Downloader::TaskId Downloader::createTask(
    const QString&          url,
    const QString&          savePath,
    const ProgressCallback& pCb,
    const FinishedCallback& fCb
) {
#if PL_BUILD_YDP02X
    auto  taskId = PEN_CALL(TaskId, "_ZN7YGlobal12createTaskIDEv", void*)(YPointer<YGlobal>::getInstance());
#else
    // 三代（YDP03X）无 YGlobal::createTaskID，taskId 由无 this 的 YIdGenerator::nextSequenceId() 生成。
    auto  taskId = PEN_CALL(TaskId, "_ZN12YIdGenerator14nextSequenceIdEv")();
#endif
    auto* rtn =
        PEN_CALL(void*, "_ZN11YDownloader12downloadFileEyRK7QStringS2_bi", void*, TaskId, QString const&, QString const&, bool, int)(
            YPointer<YDownloader>::getInstance(),
            taskId,
            url,
            savePath,
            false,
            3600000
        );
    if (!rtn) {
        fCb(taskId, savePath, ResultStatus::ERROR_FAIL_TO_START);
        return 0;
    }
    Task task;
    task.mFinishedCallback = fCb;
    task.mProgressCallback = pCb;
    task.mId               = taskId;
    task.mSavePath         = savePath;
    mTasks[taskId]         = task;
    return taskId;
}

bool Downloader::stopTask(TaskId id) {
    if (!mTasks.contains(id)) {
        return false;
    }
    auto task = mTasks[id];
    if (task.mFinishedCallback) {
        task.mFinishedCallback(id, task.mSavePath, ResultStatus::ERROR_STOPPED);
    }
    PEN_CALL(void*, "_ZN11YDownloader18cancelDownloadTaskEy", void*, TaskId)(YPointer<YDownloader>::getInstance(), id);
    _clearTask(id);
    return true;
}

void Downloader::handleDownloadFinished(TaskId id, bool isOk, int stat) {
    if (!mTasks.contains(id)) {
        return;
    }
    auto task   = mTasks[id];
    auto result = ResultStatus::SUCCEED;
    switch (stat) {
    case 1:
        result = ResultStatus::ERROR_FAIL_STORAGE_INVALID;
        break;
    case 2:
        result = ResultStatus::ERROR_FAIL_OPEN_FILE;
        break;
    default:
        break;
    }
    if (task.mFinishedCallback) {
        task.mFinishedCallback(id, task.mSavePath, result);
    }
    _clearTask(id);
}

void Downloader::handleDownloadProgress(TaskId id, int prog) {
    if (!mTasks.contains(id) || !mTasks[id].mProgressCallback) {
        return;
    }
    mTasks[id].mProgressCallback(id, prog);
}

void Downloader::_clearTask(TaskId id) { mTasks.remove(id); }

} // namespace mod

// 二代 downloadFinish 枚举为 YEnumWrapper::Download_Error_Type；三代换为
// YBaseEnum::DownloadResult 且末尾多一个 QString 参数。
#if PL_BUILD_YDP02X
PEN_HOOK(
    uint64,
    _ZN11YDownloader14downloadFinishEybN12YEnumWrapper19Download_Error_TypeE,
    uint64                  a1,
    mod::Downloader::TaskId id,
    bool                    isOk,
    int                     errType,
    void*                   a5
) {
    // errType
    // 0 == No error.
    // 1 == Storage Invalid.
    // 2 == Can't open file.
    mod::Downloader::getInstance().handleDownloadFinished(id, isOk, errType);
    return origin(a1, id, isOk, errType, a5);
}
#else
PEN_HOOK(
    uint64,
    _ZN11YDownloader14downloadFinishEybN9YBaseEnum14DownloadResultERK7QString,
    uint64                  a1,
    mod::Downloader::TaskId id,
    bool                    isOk,
    int                     errType,
    void*                   a5
) {
    // errType
    // 0 == No error.
    // 1 == Storage Invalid.
    // 2 == Can't open file.
    mod::Downloader::getInstance().handleDownloadFinished(id, isOk, errType);
    return origin(a1, id, isOk, errType, a5);
}
#endif

PEN_HOOK(
    uint64,
    _ZN11YDownloader16downloadProgressEyixx,
    uint64                  self,
    mod::Downloader::TaskId id,
    int                     prog,
    void*                   a4,
    void*                   a5
) {
    mod::Downloader::getInstance().handleDownloadProgress(id, prog);
    return origin(self, id, prog, a4, a5);
}
