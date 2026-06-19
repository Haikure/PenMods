// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "mod/Config.h"

#include "common/Utils.h"

#include "common/util/System.h"

#include "Version.h"

#include <set>

namespace fs = std::filesystem;

namespace mod {

std::string get_config_path() { return (util::getModuleFileInfo().absolutePath() + "config.json").toStdString(); }

Config::Config() : Logger("Config") {

    // clang-format off

    mData = {
        {"version", VERSION_CONFIG},
        {"column_db", {
             {"patch", true}
        }},
        {"dev", {
            {"offline_rm", true}
        }},
        {"net", {
            {"proxy_enabled", false},
            {"proxy_type", 0},
            {"proxy_hostname", "127.0.0.1"},
            {"proxy_port", 1270},
            {"proxy_username", ""},
            {"proxy_password", ""},
        }},
        {"logger", {
            {"no_upload_user_action", true},
            {"no_upload_raw_scan_img", true},
            {"no_upload_httplog", true}
        }},
        {"query", {
            {"lower_scan", false},
            {"type_by_hand", true}
        }},
        {"wordbook", {
            {"phrase_tab", true},
            {"nocase_sensitive", true}
        }},
        {"screen", {
            {"sleep_duration", 30},
            {"intel_sleep", false}
        }},
        {"battery", {
            {"suspend_duration", 600},
            {"performance_mode", 0}
        }},
        {"locker", {
            {"enabled", false},
            {"password", "abcd"},
            {"scene", {
                {"screen_on", false},
                {"restart", true},
                {"reset_page", true},
                {"dev_setting", false},
                {"filemanager",false}
            }}
        }},
        {"antiembs", {
            {"auto_mute", false},
            {"low_voice", false},
            {"no_auto_pron", false},
            {"fast_hide_music", false},
            {"fast_mute", false}
        }},
        {"serv", {
            {"ssh_autorun", false},
            {"adb_autorun", false},
            {"adb_skip_verification", false}
        }},
        {"fm", {
            {"order", {
                {"basic", 0},
                {"reversed", false}
            }},
            {"hide_paired_lyrics", false},
            {"show_hidden_files", false}
        }},
        {"wallpaper", {
            {"mode", 0},
            {"custom_image_path", ""},
            {"wallpaper_folder", ""},
            {"cycle_interval", 300},
            {"last_wallpaper", ""}
        }},
        {"ai", {
            {"speech_assistant", false},
            {"chatbot", {
                {"api_key", ""},
                {"api_endpoint", "https://api.deepseek.com/v1/chat/completions"},
                {"model", "deepseek-chat"},
                {"temperature", 0.7},
                {"default_prompt", "你是一个有用的助手，使用中文回复用户的问题。"},
                {"streaming", true}
            }}
        }}
    };

    // clang-format on

    _load();
}

json Config::read(const std::string& name) {
    if (!mData.contains(name)) {
        return {};
    }
    return mData.at(name);
}

bool Config::write(const std::string& name, json content, bool saveImmediately) {
    if (mData.find(name) == mData.end()) {
        return false;
    }
    mData[name] = std::move(content);
    if (saveImmediately) {
        _save();
    }
    return true;
}

bool Config::_update(json& data) {
    if (!data.contains("version") && data.at("version") == VERSION_CONFIG) {
        return false;
    }
    info("Configuration file is being updated...");

    try {

        // v100 -> v110
        if (data["version"] < 110) {
            data["fm"] = {
                {"order", {{"basic", 0}, {"reversed", false}}}
            };
            data["antiembs"]["fast_mute"] = false;
            data["version"]               = 110;
        }

        // v110 -> v116
        if (data["version"] < 116) {
            data["locker"]["scene"]["dev_setting"] = false;
            data["version"]                        = 116;
        }

        // v116 -> v117
        if (data["version"] < 117) {
            data["fm"]["hide_paird_lyrics"]     = false;
            data["battery"]["performance_mode"] = 0;
            data["version"]                     = 117;
        }

        // v117 -> v118
        if (data["version"] < 118) {
            data["wordbook"].erase("mod_exporter");
            data["version"] = 118;
        }

        // v118 -> v120
        if (data["version"] < 120) {
            data["ai"] = {
                {"bing", {{"enabled", false}, {"request_address", ""}, {"chathub_address", ""}}}
            };
            data["version"] = 120;
        }

        // v120 -> v130
        if (data["version"] < 130) {
            data["ai"]["speech_assistant"]   = false;
            data["fm"]["hide_paired_lyrics"] = data["fm"]["hide_paird_lyrics"];
            data["fm"].erase("hide_paird_lyrics");
            data["dev"].erase("wifi_page_show_ip");
            data["column_db"].erase("limit");
            data["column_db"]["patch"] = true;
            data["version"]            = 130;
        }

        // v130 -> v131
        // TODO.

    } catch (...) {
        return false;
    }

    return true;
}

bool has_same_recursive_keys(const json& j1, const json& j2) {
    // 1. 检查它们是否具有相同的 JSON 类型
    if (j1.type() != j2.type()) {
        return false;
    }

    // 2. 如果是对象类型，进行键集比较和递归
    if (j1.is_object() && j2.is_object()) {

        // 快速检查键的数量是否相同
        if (j1.size() != j2.size()) {
            return false;
        }

        // 提取 j1 的键到 std::set
        std::set<std::string> keys1;
        for (auto it = j1.begin(); it != j1.end(); ++it) {
            keys1.insert(it.key());
        }

        // 遍历 j2 的键，并进行递归检查
        for (auto it = j2.begin(); it != j2.end(); ++it) {
            const auto& key = it.key();

            // 检查 j2 的键是否在 j1 中存在
            if (keys1.find(key) == keys1.end()) {
                // j2 包含一个 j1 不包含的键
                return false;
            }

            // 递归地检查对应的值（如果是对象或数组）
            const auto& val1 = j1.at(key);
            const auto& val2 = j2.at(key);

            if (!has_same_recursive_keys(val1, val2)) {
                return false;
            }
        }

        // 如果所有键都存在且所有子结构递归检查通过，则键结构一致
        return true;

    }
    // 3. 如果是数组类型，递归检查数组中的每个元素
    else if (j1.is_array() && j2.is_array()) {
        // 对于数组，我们通常只要求它们的长度相同，并递归检查每个位置上的键结构
        // 如果你的需求是比较 **数组元素中的对象键结构**，则需要这样做
        if (j1.size() != j2.size()) {
            return false;
        }

        // 遍历所有元素进行递归比较
        for (size_t i = 0; i < j1.size(); ++i) {
            if (!has_same_recursive_keys(j1.at(i), j2.at(i))) {
                return false;
            }
        }
        return true;
    }
    // 对于其他基本类型（如 null, string, number, boolean），我们只比较它们的类型。
    // 因为我们只关心键结构，所以它们是键比较的 “叶子节点”，类型一致即可。
    else {
        return true;
    }
}

bool Config::_load() {
    info("Loading configuration...");
    auto path = get_config_path();
    if (!fs::exists(path)) {
        warn("Configuration not found, creating...");
        return _save() ? _load() : false;
    }
    json tmp_default_config = mData; // 先临时保存默认值
    json tmp;
    try {
        tmp = json::parse(readFile(path.c_str()));
    } catch (...) {}
    if (tmp.empty() || !tmp.contains("version")) {
        warn("Configuration error, being repaired...");
        return _save() ? _load() : false;
    }
    if (tmp["version"] != VERSION_CONFIG) {
        if (!_update(tmp)) {
            return false;
        }
        info("Saving configuration...");
        mData = tmp;
        _save();
    }
    info("Successfully loaded configuration.");
    _clean_and_merge(tmp_default_config, tmp); // 将更新后的临时数据覆盖到临时保存的默认值 JSON 结构中
    mData = tmp_default_config;                // 更新覆盖后的结果
    if (!has_same_recursive_keys(tmp_default_config, tmp)) {
        info("Has some incompatible keys, reparing...");
        _save(); // 若发现有键不存在，保存以添加缺失的键，值为默认值
    }
    return true;
}

bool Config::_save() {
    std::ofstream ofile;
    ofile.open(get_config_path());
    if (ofile.good()) {
        ofile << mData.dump(4);
        return true;
    }
    return false;
}

// 使用加载或更新后的 JSON 覆盖默认值，避免更新未处理时存在键值对缺失的情况
void Config::_clean_and_merge(json& defaults, const json& loaded) {
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto& key = it.key();

        if (loaded.contains(key)) {
            const auto& loaded_value = loaded.at(key);

            if (it->is_object() && loaded_value.is_object()) {
                _clean_and_merge(*it, loaded_value);
            } else {
                *it = loaded_value;
            }
        }
    }
}

} // namespace mod
