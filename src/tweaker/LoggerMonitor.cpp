// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/LoggerMonitor.h"

#include "common/Event.h"

#include <QQmlContext>

#include <cctype>

namespace mod {

LoggerMonitor::LoggerMonitor() {

    mCfg = Config::getInstance().read(mClassName);

    mNoUploadUserAction = mCfg["no_upload_user_action"];
    mNoUploadRawScanImg = mCfg["no_upload_raw_scan_img"];
    mNoUploadHttplog    = mCfg["no_upload_httplog"];

    // 读取过滤标签列表
    if (mCfg.contains("filtered_tags") && mCfg["filtered_tags"].is_array()) {
        for (auto& tag : mCfg["filtered_tags"]) {
            mFilteredTags.insert(tag.get<std::string>());
        }
    }

    // 为原生日志拦截器启用彩色输出
    mReplacer->set_pattern("[%H:%M:%S.%e] [%n] [%^%l%$] %^%v%$");

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("loggerMonitor", this);
    });
}

std::shared_ptr<spdlog::logger>& LoggerMonitor::getLogging() { return mReplacer; }

bool LoggerMonitor::getNoUploadUserAction() const { return mNoUploadUserAction; }

bool LoggerMonitor::getNoUploadRawScanImg() const { return mNoUploadRawScanImg; }

bool LoggerMonitor::getNoUploadHttplog() const { return mNoUploadHttplog; }

void LoggerMonitor::setNoUploadUserAction(bool val) {
    if (mNoUploadUserAction != val) {
        mNoUploadUserAction           = val;
        mCfg["no_upload_user_action"] = val;
        WRITE_CFG;
        emit noUploadUserActionChanged();
    }
}

void LoggerMonitor::setNoUploadRawScanImg(bool val) {
    if (mNoUploadRawScanImg != val) {
        mNoUploadRawScanImg            = val;
        mCfg["no_upload_raw_scan_img"] = val;
        WRITE_CFG;
        emit noUploadRawScanImgChanged();
    }
}

void LoggerMonitor::setNoUploadHttplog(bool val) {
    if (mNoUploadHttplog != val) {
        mNoUploadHttplog          = val;
        mCfg["no_upload_httplog"] = val;
        WRITE_CFG;
        emit noUploadHttplogChanged();
    }
}

bool LoggerMonitor::isTagFiltered(const std::string& message) {
    if (mFilteredTags.empty()) return false;

    // 原始消息格式示例: "[YDownloader] download finished. task 10143..."
    // 或: "[queue] sem_timedwait() timed out"
    // 只需提取第一个 [...] 中的内容作为标签名进行匹配
    auto openBracket = message.find('[');
    if (openBracket == std::string::npos) return false;

    auto closeBracket = message.find(']', openBracket);
    if (closeBracket == std::string::npos) return false;

    if (closeBracket <= openBracket + 1) return false;  // 空括号

    std::string tag = message.substr(openBracket + 1, closeBracket - openBracket - 1);
    return mFilteredTags.find(tag) != mFilteredTags.end();
}

} // namespace mod

// Local Logging

PEN_HOOK(void, runtime_log, int priority, const char* format, ...) {
    char    buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, 1024, format, args);
    va_end(args);
    auto len = strlen(buffer);
    if (len > 0) {
        if (buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        if (!mod::LoggerMonitor::getInstance().isTagFiltered(buffer)) {
            mod::LoggerMonitor::getInstance().getLogging()->debug(buffer);
        }
    }
}

PEN_HOOK(void, DictPen_log, int priority, const char* format, ...) {
    char    buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, 1024, format, args);
    va_end(args);
    auto len = strlen(buffer);
    if (len > 0) {
        if (buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        if (!mod::LoggerMonitor::getInstance().isTagFiltered(buffer)) {
            mod::LoggerMonitor::getInstance().getLogging()->debug(buffer);
        }
    }
}

PEN_HOOK(
    int,
    hal_logger_print,
    uint32      a1,
    const char* fileName,
    void*       a3,
    const char* funcName,
    void*       a5,
    void*       a6,
    const char* format,
    ...
) {
    char    buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, 1024, format, args);
    va_end(args);
    auto len = strlen(buffer);
    if (len > 0) {
        if (buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        if (!mod::LoggerMonitor::getInstance().isTagFiltered(buffer)) {
            mod::LoggerMonitor::getInstance().getLogging()->debug(buffer);
        }
    }
    return 1;
}

PEN_HOOK(void, printf, const char* format, ...) {
    char    buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, 1024, format, args);
    va_end(args);
    auto len = strlen(buffer);
    if (len > 0) {
        if (buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        if (!mod::LoggerMonitor::getInstance().isTagFiltered(buffer)) {
            mod::LoggerMonitor::getInstance().getLogging()->debug(buffer);
        }
    }
}

PEN_HOOK(void, fprintf, FILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    if ((void*)stream == PEN_SYM("__bss_start__")) {
        vfprintf(stream, format, args);
    } else {
        char buffer[1024];
        vsnprintf(buffer, 1024, format, args);
        auto len = strlen(buffer);
        if (len > 0) {
            if (buffer[len - 1] == '\n') {
                buffer[len - 1] = '\0';
            }
            if (!mod::LoggerMonitor::getInstance().isTagFiltered(buffer)) {
                mod::LoggerMonitor::getInstance().getLogging()->debug(buffer);
            }
        }
    }
    va_end(args);
}

// User Action

PEN_HOOK(void, _ZN11YLogManager19reportUserActionLogERK7QString, uint64 self, uint64 a2) {
    if (!mod::LoggerMonitor::getInstance().getNoUploadUserAction()) {
        origin(self, a2);
    }
}

PEN_HOOK(void, _ZN11YLogManager19uploadUserActionLogEv, uint64 self) {
    if (!mod::LoggerMonitor::getInstance().getNoUploadUserAction()) {
        origin(self);
    }
}

// Raw Scan Img.

#if PL_BUILD_YDP02X
PEN_HOOK(uint64, _ZN11YLogManager22doUploadUserRawScanImgEb, uint64 self, bool a2) {
    bool rtn = false;
    if (!mod::LoggerMonitor::getInstance().getNoUploadRawScanImg()) {
        rtn = origin(self, a2);
    }
    return rtn;
}

PEN_HOOK(void, _ZN11YLogManager20uploadUserRawScanImgEb, uint64 self, bool a2) {
    if (!mod::LoggerMonitor::getInstance().getNoUploadRawScanImg()) {
        origin(self, a2);
    }
}
#endif

// Http Log

PEN_HOOK(void, _ZN11YLogManager11sendHttpLogERK7QStringS2_, uint64 self, uint64 a2, uint64 a3) {
    if (!mod::LoggerMonitor::getInstance().getNoUploadHttplog()) {
        origin(self, a2, a3);
    }
}