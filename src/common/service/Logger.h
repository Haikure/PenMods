// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

namespace mod {

class Logger : public spdlog::logger {
public:
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    Logger(const Logger&&)             = delete;
    Logger&& operator=(const Logger&&) = delete;

protected:
    explicit Logger(const std::string& name) : logger(name) {
        sinks().emplace_back(_getLoggingSink());
        // #ifdef PL_DEBUG
        set_level(spdlog::level::debug);
        // #endif
        // %^ 和 %$ 标记颜色范围：stdout_color_sink_mt 内置默认颜色
        //   trace=白色, debug=青色, info=绿色, warn=黄色, err=红色, critical=红底白字加粗
        set_pattern("[%H:%M:%S.%e] [%n] [%^%l%$] %^%v%$");
    }

private:
    static spdlog::sink_ptr _getLoggingSink() {
        static spdlog::sink_ptr sink = nullptr;
        if (!sink) {
            sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        }
        return sink;
    }
};

} // namespace mod