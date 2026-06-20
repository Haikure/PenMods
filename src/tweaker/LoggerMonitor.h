// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "mod/Config.h"

#include <unordered_set>

namespace mod {

class LoggerMonitor : public QObject, public Singleton<LoggerMonitor> {
    Q_OBJECT

    Q_PROPERTY(bool noUploadUserAction READ getNoUploadUserAction WRITE setNoUploadUserAction NOTIFY
                   noUploadUserActionChanged);
    Q_PROPERTY(bool noUploadRawScanImg READ getNoUploadRawScanImg WRITE setNoUploadRawScanImg NOTIFY
                   noUploadRawScanImgChanged);
    Q_PROPERTY(bool noUploadHttplog READ getNoUploadHttplog WRITE setNoUploadHttplog NOTIFY noUploadHttplogChanged);

public:
    [[nodiscard]] bool getNoUploadUserAction() const;

    [[nodiscard]] bool getNoUploadRawScanImg() const;

    [[nodiscard]] bool getNoUploadHttplog() const;

    void setNoUploadUserAction(bool);

    void setNoUploadRawScanImg(bool);

    void setNoUploadHttplog(bool);

    std::shared_ptr<spdlog::logger>& getLogging();

    bool isNativeLoggingDisabled();

    /// 检查日志消息中的分类标签是否被过滤
    /// 解析格式: [timestamp] [native] [level] [tag] ... → 匹配 [tag] 是否在过滤列表中
    bool isTagFiltered(const std::string& message);

signals:

    void noUploadUserActionChanged();

    void noUploadRawScanImgChanged();

    void noUploadHttplogChanged();

private:
    friend Singleton<LoggerMonitor>;
    explicit LoggerMonitor();

    std::string mClassName = "logger";
    json        mCfg;

    bool mNoUploadUserAction;
    bool mNoUploadRawScanImg;
    bool mNoUploadHttplog;

    std::shared_ptr<spdlog::logger> mReplacer = spdlog::stdout_color_mt("native");

    /// 被过滤的标签列表（如 "YDownloader", "queue" 等）
    std::unordered_set<std::string> mFilteredTags;
};

} // namespace mod