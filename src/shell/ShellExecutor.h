// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Singleton.h"
#include "common/service/Logger.h"

#include <QJSValue>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QTimer>

namespace mod {

/**
 * @brief Shell 执行器，暴露给 QML 层
 *
 * 通过 QProcess 提供 Shell 命令的同步/异步执行能力。
 * 在 QML 上下文中注册为 "shell"，QML 可直接调用。
 *
 * 使用示例（QML）：
 *   // 同步执行
 *   var output = shell.exec("ls -la /tmp");
 *
 *   // 同步执行，获取完整结果
 *   var result = shell.execWithResult("cat /proc/cpuinfo");
 *   console.log("exitCode:", result.exitCode);
 *
 *   // 异步执行
 *   shell.execAsync("ping -c 4 8.8.8.8", function(result) {
 *       console.log("output:", result.stdout);
 *   });
 *
 *   // 分离执行（不等待结果）
 *   shell.startDetached("reboot");
 */
class ShellExecutor : public QObject, public Singleton<ShellExecutor>, private Logger {
    Q_OBJECT
    QML_SINGLETON
    QML_NAMED_ELEMENT(Shell)

public:
    explicit ShellExecutor(QObject* parent = nullptr);

    /**
     * @brief 同步执行 Shell 命令，返回 stdout 输出
     * @param command 要执行的 Shell 命令字符串
     * @return 命令的标准输出（stdout），如果执行失败返回空字符串
     *
     * 注意：此方法会阻塞调用线程直到命令执行完毕或超时。
     * 不建议在 UI 线程中执行耗时命令，请使用 execAsync() 代替。
     */
    Q_INVOKABLE QString exec(const QString& command);

    /**
     * @brief 同步执行 Shell 命令，返回完整结果对象
     * @param command 要执行的 Shell 命令字符串
     * @return 包含 stdout / stderr / exitCode 字段的 JSON 对象
     *
     * JSON 对象结构：
     *   {
     *       "stdout":  "命令标准输出",
     *       "stderr":  "命令错误输出",
     *       "exitCode": 0,
     *       "timedOut": false
     *   }
     */
    Q_INVOKABLE QJsonObject execWithResult(const QString& command);

    /**
     * @brief 异步执行 Shell 命令，通过回调返回结果
     * @param command  要执行的 Shell 命令字符串
     * @param callback JS 回调函数，接收一个结果对象参数
     *
     * 回调函数接收的 JSON 对象结构与 execWithResult() 相同。
     * 如果命令执行超时或发生错误，exitCode 为 -1 并携带错误信息。
     *
     * 示例：
     *   shell.execAsync("sleep 1 && echo done", function(result) {
     *       console.log("exitCode:", result.exitCode);
     *       console.log("stdout:", result.stdout);
     *   });
     */
    Q_INVOKABLE void execAsync(const QString& command, QJSValue callback);

    /**
     * @brief 分离执行 Shell 命令（不等待结果）
     * @param command 要执行的 Shell 命令字符串
     *
     * 命令在后台独立运行，与当前进程完全分离。
     * 适用于长期运行或无需关注结果的命令。
     */
    Q_INVOKABLE void startDetached(const QString& command);

    /**
     * @brief 设置默认命令超时时间
     * @param ms 超时毫秒数（默认 5000ms）。设为 0 表示不限时。
     */
    Q_INVOKABLE void setTimeout(int ms);

    /**
     * @brief 获取当前超时设置
     * @return 超时毫秒数
     */
    Q_INVOKABLE int getTimeout() const;

    /**
     * @brief 终止当前正在执行的异步命令
     *
     * 如果当前有正在运行的异步命令，强制终止。
     * 同步执行的命令不受此方法影响。
     */
    Q_INVOKABLE void kill();

signals:
    /** 异步命令开始执行时触发 */
    void started(const QString& command);

    /** 异步命令执行完毕时触发（无论成功或失败） */
    void finished(const QString& command, int exitCode);

    /** 异步命令输出新数据时触发 */
    void stdoutReady(const QString& data);

    /** 异步命令错误输出新数据时触发 */
    void stderrReady(const QString& data);

    /** 异步命令执行出错时触发 */
    void errorOccurred(const QString& command, const QString& errorMessage);

private:
    int m_timeoutMs = 5000; ///< 默认超时时间（毫秒）

    /// 执行单个同步命令的内部实现
    bool runSync(QProcess& process, const QString& command);

    /// 从 QProcess 构建结果 JSON 对象
    QJsonObject buildResult(QProcess& process, bool timedOut) const;

    /// 执行异步命令的进程实例（同一时间只允许一个异步命令）
    QProcess*    m_asyncProcess = nullptr;
    QTimer*      m_asyncTimer   = nullptr;
    QJSValue     m_asyncCallback;
    QString      m_asyncCommand;

    /// 异步进程的 stdout/stderr 缓冲区
    QString m_asyncStdout;
    QString m_asyncStderr;

    /// 清理异步资源（断开信号、删除进程和定时器）
    void cleanupAsync();

    /// 自增的异步命令世代号，用于防止陈旧信号干扰新命令
    int m_asyncGeneration = 0;

    friend class Singleton<ShellExecutor>;
};

} // namespace mod