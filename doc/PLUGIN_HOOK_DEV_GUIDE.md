# PenMods 插件 Hook API 开发指南

## 概览

PenMods 为外部插件提供了完整的 Hook 机制，允许插件开发者在自己的 C++ 代码中创建 Hook，拦截和修改系统函数的行为。

## 快速开始

### 1. 获取 SDK

将 `PluginSDK.h` 文件复制到你的插件项目中。

### 2. 编写插件代码

```cpp
#include "PluginSDK.h"
#include <cstdio>

// 全局 Hook API 指针（必须声明）
PluginHookAPI* g_hook_api = NULL;

namespace my_plugin {

// 定义原始函数的函数指针类型
typedef uint64_t (*OCRStartFunc)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
OCRStartFunc original_ocrStart = NULL;

// 自定义的 Detour 函数
uint64_t detour_ocrStart(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    printf("[MyPlugin] OCR is starting!\n");
    
    // 执行你的自定义逻辑
    // ...
    
    // 调用原始函数
    return original_ocrStart(self, a2, a3, a4, a5);
}

}  // namespace my_plugin

extern "C" {
    // 可选：基础初始化函数（如果需要在 Hook 之前初始化）
    void init_plugin() {
        printf("[MyPlugin] Basic initialization\n");
    }

    // 必须导出：Hook API 初始化入口
    // PluginManager 会在加载插件时调用此函数，并注入 Hook API
    void init_plugin_with_hook_api(PluginHookAPI* hook_api) {
        if (!hook_api) {
            printf("[MyPlugin] ERROR: hook_api is NULL\n");
            return;
        }

        // 保存全局 Hook API 指针
        g_hook_api = hook_api;
        printf("[MyPlugin] Hook API initialized\n");

        // 查询目标函数的符号地址
        void* ocrStartAddr = hook_api->querySymbol("_ZN11YSystemBase8ocrStartEv");
        if (!ocrStartAddr) {
            printf("[MyPlugin] ERROR: Could not find OCR symbol\n");
            return;
        }

        printf("[MyPlugin] OCR symbol found at %p\n", ocrStartAddr);

        // 注册 Hook
        int result = hook_api->hookFunction(
            ocrStartAddr,
            (void*)my_plugin::detour_ocrStart,
            (void**)&my_plugin::original_ocrStart
        );

        if (result == 0) {
            printf("[MyPlugin] Hook registered successfully!\n");
        } else {
            printf("[MyPlugin] ERROR: Failed to register hook\n");
        }
    }

    // 可选：清理函数（插件卸载时调用）
    void destroy_plugin() {
        printf("[MyPlugin] Plugin destroyed\n");
    }
}
```

### 3. 编译插件

确保在编译时可以找到 `PluginSDK.h`。

### 4. 安装插件

将编译后的 `.so` 文件放到 `/userdisk/PenMods/plugins/my_plugin/` 目录中，并提供 `metadata.json`。

## Hook API 参考

### PluginHookAPI 结构体

```cpp
typedef struct {
    void* (*querySymbol)(const char* symbolName);
    int (*hookFunction)(void* targetAddr, void* detourFunc, void** originalFunc);
} PluginHookAPI;
```

### querySymbol - 查询符号地址

**功能：** 查询目标函数在内存中的地址

**参数：**
- `symbolName` (const char*): 目标函数的符号名称（C++ mangled name）

**返回值：**
- 成功：符号地址（void*）
- 失败：NULL

**示例：**
```cpp
void* addr = hook_api->querySymbol("_ZN11YSystemBase8ocrStartEv");
if (addr == NULL) {
    printf("Symbol not found!\n");
    return;
}
```

### hookFunction - 注册 Hook

**功能：** 在指定地址注册一个 Hook

**参数：**
- `targetAddr` (void*): 目标函数的地址（由 querySymbol 获取）
- `detourFunc` (void*): 你自定义的 detour 函数指针
- `originalFunc` (void**): 输出参数，接收原始函数指针

**返回值：**
- 0：成功
- 非 0：失败

**示例：**
```cpp
typedef uint64_t (*OriginalFunc)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
OriginalFunc original = NULL;

uint64_t detour(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    printf("Hook called!\n");
    return original(self, a2, a3, a4, a5);
}

int result = hook_api->hookFunction(addr, (void*)detour, (void**)&original);
if (result != 0) {
    printf("Failed to hook!\n");
}
```

## 便利宏

PluginSDK.h 提供了便利宏来简化代码：

### PLUGIN_SYM - 查询符号

```cpp
void* addr = PLUGIN_SYM("_ZN11YSystemBase8ocrStartEv");
```

### PLUGIN_HOOK - 注册 Hook

```cpp
PLUGIN_HOOK(addr, detour_func, original_func);
```

## 常见任务

### 拦截按钮事件

```cpp
// 按钮事件的原始函数签名
typedef uint64_t (*ButtonPressFunc)(uint64_t self, uint32_t buttonId, int a3, int a4, int a5);
ButtonPressFunc original_button_press = NULL;

uint64_t detour_button_press(uint64_t self, uint32_t buttonId, int a3, int a4, int a5) {
    printf("Button %u pressed!\n", buttonId);
    return original_button_press(self, buttonId, a3, a4, a5);
}

void init_plugin_with_hook_api(PluginHookAPI* hook_api) {
    g_hook_api = hook_api;
    
    void* addr = hook_api->querySymbol("_ZN14YButtonMonitor16_do_button_pressE11button_id_tiii");
    if (addr) {
        hook_api->hookFunction(addr, (void*)detour_button_press, (void**)&original_button_press);
    }
}
```

### 拦截扫描结果

```cpp
typedef void (*ScanFinishFunc)(uint64_t self, const QString& content, int scanType);
ScanFinishFunc original_scan_finish = NULL;

void detour_scan_finish(uint64_t self, const QString& content, int scanType) {
    printf("Scan finished: %s\n", content.toStdString().c_str());
    return original_scan_finish(self, content, scanType);
}

void init_plugin_with_hook_api(PluginHookAPI* hook_api) {
    g_hook_api = hook_api;
    
    void* addr = hook_api->querySymbol("_ZN11YSystemBase12onScanFinishERK7QStringi");
    if (addr) {
        hook_api->hookFunction(addr, (void*)detour_scan_finish, (void**)&original_scan_finish);
    }
}
```

## 最佳实践

1. **总是检查指针有效性**
   ```cpp
   if (!hook_api || !hook_api->querySymbol || !hook_api->hookFunction) {
       printf("ERROR: Invalid hook_api\n");
       return;
   }
   ```

2. **使用 try-catch 保护回调函数**
   ```cpp
   uint64_t detour(...) {
       try {
           // 你的代码
       } catch (const std::exception& e) {
           printf("Exception: %s\n", e.what());
       }
       return original(...);
   }
   ```

3. **使用全局指针存储原始函数**
   ```cpp
   // 不要在回调中每次都获取原始函数
   typedef MyFunc OriginalFuncType;
   OriginalFuncType* g_original_func = NULL;  // 全局存储
   ```

4. **记录 Hook 状态**
   ```cpp
   printf("[MyPlugin] Hooks initialized: %d\n", hook_count);
   ```

## 常见符号

以下是词典笔主程序中的部分符号名称（C++ mangled name），其他符号可通过 nm、readelf 或 IDA、Ghidra 等工具获取：

| 功能 | 符号 |
|------|------|
| OCR 启动 | `_ZN11YSystemBase8ocrStartEv` |
| OCR 停止 | `_ZN11YSystemBase7ocrStopEi` |
| 扫描完成 | `_ZN11YSystemBase12onScanFinishERK7QStringi` |
| 按钮按下 | `_ZN14YButtonMonitor16_do_button_pressE11button_id_tiii` |
| 页面切换 | `_ZN7YGlobal23currentPageIndexChangedEv` |

## 故障排除

### Symbol not found

**问题：** querySymbol 返回 NULL

**解决方案：**
1. 检查符号名称是否正确
2. 确保目标函数确实存在于可执行文件中
3. 检查 SymDB 是否正确初始化

### Hook registration failed

**问题：** hookFunction 返回非 0

**解决方案：**
1. 检查目标地址是否有效
2. 确保 detour 函数签名正确
3. 检查目标地址是否已经被 Hook

### Plugin crashes after loading

**问题：** 加载插件后系统崩溃

**解决方案：**
1. 检查 Detour 函数的签名是否正确
2. 确保不会从 Detour 中抛出异常
3. 检查是否正确调用了原始函数
4. 使用 try-catch 保护 Hook 代码
