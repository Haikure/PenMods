// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

/**
 * @brief PenMods Plugin SDK
 *
 * 这是为外部插件开发者提供的 SDK
 * 将此文件下载到你的插件项目中使用
 *
 * 使用方法：
 *   在你的插件项目中 #include "PluginSDK.h"
 *
 *   extern "C" {
 *       void init_plugin() {
 *           // 可选的基础初始化
 *       }
 *
 *       void init_plugin_with_hook_api(PluginHookAPI* hook_api) {
 *           // 设置全局 Hook API 指针
 *           g_hook_api = hook_api;
 *
 *           // 使用 hook_api->querySymbol 和 hook_api->hookFunction
 *           // 来创建你自己的 Hook
 *       }
 *   }
 */

#include <cstdint>

/**
 * @brief Hook API 接口 - 供外部插件使用
 *
 * PluginManager 会在调用插件的 init_plugin_with_hook_api 时注入此接口
 * 插件可以通过这个接口查询符号地址和注册 Hook
 */
typedef struct {
    /**
     * @brief 查询符号地址
     * @param symbolName 符号名称（C++ mangled name）
     * @return 符号地址，失败返回 NULL
     *
     * 示例：
     *   void* addr = hook_api->querySymbol("_ZN11YSystemBase8ocrStartEv");
     */
    void* (*querySymbol)(const char* symbolName);

    /**
     * @brief Hook 一个函数
     * @param targetAddr 目标函数的地址（由 querySymbol 获取）
     * @param detourFunc Detour 函数指针（你自定义的函数）
     * @param originalFunc 输出参数，接收原始函数指针的地址
     * @return 0 表示成功，非 0 表示失败
     *
     * 示例：
     *   typedef uint64_t (*OCRStartFunc)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
     *   OCRStartFunc original_ocrStart = NULL;
     *
     *   uint64_t detour_ocrStart(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
     *       printf("OCR starting!\\n");
     *       return original_ocrStart(self, a2, a3, a4, a5);
     *   }
     *
     *   hook_api->hookFunction(addr, (void*)detour_ocrStart, (void**)&original_ocrStart);
     */
    int (*hookFunction)(void* targetAddr, void* detourFunc, void** originalFunc);
} PluginHookAPI;

// ==================== 便利宏定义（供插件使用） ====================

/**
 * @brief 全局 Hook API 指针（插件应该在 init_plugin_with_hook_api 中初始化）
 *
 * 示例：
 *   extern PluginHookAPI* g_hook_api;
 *
 *   extern "C" {
 *       void init_plugin_with_hook_api(PluginHookAPI* hook_api) {
 *           g_hook_api = hook_api;
 *       }
 *   }
 */
extern PluginHookAPI* g_hook_api;

/**
 * @brief 查询符号地址的便利宏
 *
 * 示例：
 *   void* addr = PLUGIN_SYM("_ZN11YSystemBase8ocrStartEv");
 */
#define PLUGIN_SYM(sym) (g_hook_api ? g_hook_api->querySymbol(sym) : NULL)

/**
 * @brief 注册 Hook 的便利宏
 *
 * 示例：
 *   typedef uint64_t (*OriginalFunc)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
 *   OriginalFunc original = NULL;
 *
 *   uint64_t detour(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
 *       return original(self, a2, a3, a4, a5);
 *   }
 *
 *   PLUGIN_HOOK(addr, detour, original);
 */
#define PLUGIN_HOOK(target, detour, original) \
    (g_hook_api ? g_hook_api->hookFunction(target, (void*)(detour), (void**)&(original)) : -1)
