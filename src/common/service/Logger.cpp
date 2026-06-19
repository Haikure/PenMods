// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "common/service/Logger.h"

#include "mod/Config.h"

namespace mod {

// 静态成员定义
std::atomic<bool> Logger::s_configLoaded{false};

void Logger::_applyLevelFromConfig(const std::string& name) {
    // Config 未完成初始化时不尝试读取，保持默认 debug 级别
    if (!s_configLoaded) {
        return;
    }

    auto loggerCfg = Config::getInstance().read("logger");
    if (loggerCfg.is_null() || !loggerCfg.contains("levels")) {
        return;
    }
    auto& levels = loggerCfg["levels"];
    if (!levels.contains(name)) {
        return;
    }
    std::string levelStr = levels[name].get<std::string>();
    auto level = spdlog::level::from_str(levelStr);
    if (level == spdlog::level::off && levelStr != "off") {
        // from_str returns off for unknown strings, but "off" is valid
        return;
    }
    set_level(level);
}

} // namespace mod
