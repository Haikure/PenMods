// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "shell/ShellExecutor.h"

#include "common/Event.h"

#include <QQmlContext>
#include <QQuickView>

namespace mod {

// ============================================================
// 构造函数与注册
// ============================================================

ShellExecutor::ShellExecutor(QObject* parent)
    : QObject(parent), Logger("ShellExecutor") {
    // 在 UI 初始化前将自身注册到 QML 上下文
    connect(&Event::getInstance(), &Event::beforeUiInitialization,
            [this](QQuickView& view, QQmlContext* context) {
                context->setContextProperty("shell", this);
                info("ShellExecutor 已在 QML 上下文中注册为 'shell'.");
            });
}

// ============================================================
// 同步执行
// ============================================================

bool ShellExecutor::runSync(QProcess& process, const QString& command) {
    process.setProgram("/bin/sh");
    process.setArguments({"-c", command});

    // 设置合并的 stdout/stderr 读取模式
    process.setProcessChannelMode(QProcess::SeparateChannels);

    process.start();

    if (!process.waitForStarted(3000)) {
        error("命令启动失败: {}", command.toStdString());
        return false;
    }

    // 等待完成（带超时）
    if (m_timeoutMs > 0) {
        if (!process.waitForFinished(m_timeoutMs)) {
            warn("命令执行超时，已终止: {}", command.toStdString());
            process.kill();
            process.waitForFinished(1000);
            return false;
        }
    } else {
        process.waitForFinished(-1); // 不限时等待
    }

    return true;
}

QString ShellExecutor::exec(const QString& command) {
    QProcess process;
    if (!runSync(process, command)) {
        return QString();
    }

    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

QJsonObject ShellExecutor::execWithResult(const QString& command) {
    QProcess process;
    bool timedOut = !runSync(process, command);

    return buildResult(process, timedOut);
}

QJsonObject ShellExecutor::buildResult(QProcess& process, bool timedOut) const {
    QJsonObject result;
    result["stdout"]   = QString::fromUtf8(process.readAllStandardOutput());
    result["stderr"]   = QString::fromUtf8(process.readAllStandardError());

    if (timedOut) {
        result["exitCode"] = -1;
        result["timedOut"] = true;
        result["error"]    = "Command timed out";
    } else {
        result["exitCode"] = process.exitCode();
        result["timedOut"] = false;
        if (process.exitCode() != 0) {
            result["error"] = QString("Command failed with exit code %1").arg(process.exitCode());
        }
    }

    return result;
}

// ============================================================
// 异步执行
// ============================================================

void ShellExecutor::execAsync(const QString& command, QJSValue callback) {
    // 如果已有正在执行的异步命令，先清理
    if (m_asyncProcess && m_asyncProcess->state() != QProcess::NotRunning) {
        warn("异步命令正在执行中，终止旧命令: {}", m_asyncCommand.toStdString());
        cleanupAsync();
    }

    if (!callback.isCallable()) {
        error("execAsync 的回调参数不是可调用的函数");
        return;
    }

    m_asyncCommand = command;
    m_asyncCallback = callback;
    m_asyncStdout.clear();
    m_asyncStderr.clear();

    // 创建进程
    m_asyncProcess = new QProcess(this);
    m_asyncProcess->setProgram("/bin/sh");
    m_asyncProcess->setArguments({"-c", command});
    m_asyncProcess->setProcessChannelMode(QProcess::SeparateChannels);

    // 连接信号
    connect(m_asyncProcess, &QProcess::started, this, [this, command]() {
        info("异步命令已启动: {}", command.toStdString());
        emit started(command);
    });

    connect(m_asyncProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        QString data = QString::fromUtf8(m_asyncProcess->readAllStandardOutput());
        m_asyncStdout += data;
        emit stdoutReady(data);
    });

    connect(m_asyncProcess, &QProcess::readyReadStandardError, this, [this]() {
        QString data = QString::fromUtf8(m_asyncProcess->readAllStandardError());
        m_asyncStderr += data;
        emit stderrReady(data);
    });

    connect(m_asyncProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
        // 停止超时定时器
        if (m_asyncTimer) {
            m_asyncTimer->stop();
        }

        // 构建结果
        QJsonObject result;
        result["stdout"]   = m_asyncStdout;
        result["stderr"]   = m_asyncStderr;
        result["exitCode"] = exitCode;
        result["timedOut"] = false;

        if (status != QProcess::NormalExit) {
            result["error"] = "Process crashed";
        } else if (exitCode != 0) {
            result["error"] = QString("Command failed with exit code %1").arg(exitCode);
        }

        info("异步命令已完成: {} (exitCode: {})", m_asyncCommand.toStdString(), exitCode);
        emit finished(m_asyncCommand, exitCode);

        // 调用 JS 回调
        if (m_asyncCallback.isCallable()) {
            QJSValueList args;
            args << m_asyncCallback.engine()->toScriptValue(result);
            m_asyncCallback.call(args);
        }

        // 清理
        cleanupAsync();
    });

    // 设置超时定时器
    if (m_timeoutMs > 0) {
        m_asyncTimer = new QTimer(this);
        m_asyncTimer->setSingleShot(true);
        connect(m_asyncTimer, &QTimer::timeout, this, [this]() {
            warn("异步命令执行超时，已终止: {}", m_asyncCommand.toStdString());

            if (m_asyncProcess) {
                m_asyncProcess->kill();

                // 构建超时结果
                QJsonObject result;
                result["stdout"]   = m_asyncStdout;
                result["stderr"]   = m_asyncStderr;
                result["exitCode"] = -1;
                result["timedOut"] = true;
                result["error"]    = "Command timed out";

                emit finished(m_asyncCommand, -1);
                emit errorOccurred(m_asyncCommand, "Command timed out");

                // 调用 JS 回调
                if (m_asyncCallback.isCallable()) {
                    QJSValueList args;
                    args << m_asyncCallback.engine()->toScriptValue(result);
                    m_asyncCallback.call(args);
                }

                cleanupAsync();
            }
        });
        m_asyncTimer->start(m_timeoutMs);
    }

    // 启动进程
    m_asyncProcess->start();
}

// ============================================================
// 分离执行
// ============================================================

void ShellExecutor::startDetached(const QString& command) {
    bool ok = QProcess::startDetached("/bin/sh", QStringList{"-c", command});
    if (ok) {
        info("分离执行命令: {}", command.toStdString());
    } else {
        error("分离执行命令失败: {}", command.toStdString());
    }
}

// ============================================================
// 超时管理
// ============================================================

void ShellExecutor::setTimeout(int ms) {
    if (ms < 0) {
        warn("超时时间不能为负数，已忽略: {}", ms);
        return;
    }
    m_timeoutMs = ms;
    info("Shell 命令超时时间已设置为 {} ms", m_timeoutMs);
}

int ShellExecutor::getTimeout() const {
    return m_timeoutMs;
}

// ============================================================
// 终止当前异步命令
// ============================================================

void ShellExecutor::kill() {
    if (m_asyncProcess && m_asyncProcess->state() != QProcess::NotRunning) {
        warn("手动终止异步命令: {}", m_asyncCommand.toStdString());
        m_asyncProcess->kill();
        // cleanup 会在 finished 信号处理中执行
    } else {
        debug("kill() 被调用，但没有正在运行的异步命令");
    }
}

// ============================================================
// 清理
// ============================================================

void ShellExecutor::cleanupAsync() {
    if (m_asyncTimer) {
        m_asyncTimer->stop();
        m_asyncTimer->deleteLater();
        m_asyncTimer = nullptr;
    }

    if (m_asyncProcess) {
        m_asyncProcess->deleteLater();
        m_asyncProcess = nullptr;
    }

    m_asyncCallback = QJSValue();
    m_asyncCommand.clear();
    m_asyncStdout.clear();
    m_asyncStderr.clear();
}

} // namespace mod

// ========== 静态初始化：确保 ShellExecutor 实例在 UI 初始化前被创建 ==========
static auto& s_ensureShellInit = mod::ShellExecutor::getInstance();