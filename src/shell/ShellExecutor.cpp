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

ShellExecutor::ShellExecutor(QObject* parent)
    : QObject(parent), Logger("ShellExecutor") {
    connect(&Event::getInstance(), &Event::beforeUiInitialization,
            [this](QQuickView& view, QQmlContext* context) {
                context->setContextProperty("shell", this);
                info("ShellExecutor 已在 QML 上下文中注册为 'shell'.");
            });
}

ShellExecutor::~ShellExecutor() {
    for (int id : m_tasks.keys())
        cleanupTask(id);
}

// ============================================================
// 同步执行
// ============================================================

ShellExecutor::SyncResult ShellExecutor::runSync(QProcess& process, const QString& command) {
    process.setProgram("/bin/sh");
    process.setArguments({"-c", command});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();

    if (!process.waitForStarted(3000)) {
        error("命令启动失败: {}", command.toStdString());
        return SyncResult::FailedToStart;
    }

    if (m_timeoutMs > 0) {
        if (!process.waitForFinished(m_timeoutMs)) {
            warn("命令执行超时，已终止: {}", command.toStdString());
            process.kill();
            process.waitForFinished(1000);
            return SyncResult::TimedOut;
        }
    } else {
        process.waitForFinished(-1);
    }

    return SyncResult::Ok;
}

QString ShellExecutor::exec(const QString& command) {
    QProcess process;
    if (runSync(process, command) != SyncResult::Ok)
        return QString();
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

QJsonObject ShellExecutor::execWithResult(const QString& command) {
    QProcess   process;
    SyncResult sr = runSync(process, command);
    return buildResult(process, sr);
}

QJsonObject ShellExecutor::buildResult(QProcess& process, SyncResult sr) const {
    QJsonObject result;
    result["stdout"] = QString::fromUtf8(process.readAllStandardOutput());
    result["stderr"] = QString::fromUtf8(process.readAllStandardError());

    switch (sr) {
    case SyncResult::Ok:
        result["exitCode"] = process.exitCode();
        result["timedOut"] = false;
        if (process.exitCode() != 0)
            result["error"] = QString("Command failed with exit code %1").arg(process.exitCode());
        break;
    case SyncResult::TimedOut:
        result["exitCode"] = -1;
        result["timedOut"] = true;
        result["error"]    = "Command timed out";
        break;
    case SyncResult::FailedToStart:
        result["exitCode"] = -1;
        result["timedOut"] = false;
        result["error"]    = "Failed to start process";
        break;
    }

    return result;
}

// ============================================================
// 异步执行
// ============================================================

int ShellExecutor::execAsync(const QString& command, QJSValue callback) {
    if (!callback.isCallable()) {
        error("execAsync 的回调参数不是可调用的函数");
        return -1;
    }

    const int taskId = m_nextTaskId++;

    auto* task      = new AsyncTask;
    task->id        = taskId;
    task->command   = command;
    task->callback  = callback;
    task->process   = new QProcess(this);

    task->process->setProgram("/bin/sh");
    task->process->setArguments({"-c", command});
    task->process->setProcessChannelMode(QProcess::SeparateChannels);

    m_tasks.insert(taskId, task);
    emit activeCountChanged(m_tasks.size());

    connect(task->process, &QProcess::started, this, [this, taskId]() {
        auto* t = m_tasks.value(taskId);
        if (!t) return;
        info("异步命令已启动 [{}]: {}", taskId, t->command.toStdString());
        emit started(taskId, t->command);
    });

    connect(task->process, &QProcess::readyReadStandardOutput, this, [this, taskId]() {
        auto* t = m_tasks.value(taskId);
        if (!t) return;
        QString data = QString::fromUtf8(t->process->readAllStandardOutput());
        t->stdoutBuf += data;
        emit stdoutReady(taskId, t->command, data);
    });

    connect(task->process, &QProcess::readyReadStandardError, this, [this, taskId]() {
        auto* t = m_tasks.value(taskId);
        if (!t) return;
        QString data = QString::fromUtf8(t->process->readAllStandardError());
        t->stderrBuf += data;
        emit stderrReady(taskId, t->command, data);
    });

    connect(task->process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, taskId](int, QProcess::ExitStatus) {
        finishTask(taskId, false);
    });

    connect(task->process, &QProcess::errorOccurred, this, [this, taskId](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart) return;
        auto* t = m_tasks.value(taskId);
        if (!t) return;
        error("异步命令启动失败 [{}]: {}", taskId, t->command.toStdString());

        QJsonObject result;
        result["stdout"]   = QString();
        result["stderr"]   = QString();
        result["exitCode"] = -1;
        result["timedOut"] = false;
        result["error"]    = "Failed to start process";

        QJSValue cb      = t->callback;
        QString  cmd     = t->command;
        cleanupTask(taskId);

        emit finished(taskId, cmd, -1);
        emit errorOccurred(taskId, cmd, "Failed to start process");

        if (cb.isCallable()) {
            QJSValueList args;
            args << cb.engine()->toScriptValue(result);
            cb.call(args);
        }
    });

    if (m_timeoutMs > 0) {
        task->timer = new QTimer(this);
        task->timer->setSingleShot(true);
        connect(task->timer, &QTimer::timeout, this, [this, taskId]() {
            finishTask(taskId, true);
        });
        task->timer->start(m_timeoutMs);
    }

    task->process->start();
    info("异步命令已发起 [taskId={}]: {}", taskId, command.toStdString());
    return taskId;
}

void ShellExecutor::finishTask(int taskId, bool timedOut) {
    auto* t = m_tasks.value(taskId);
    if (!t) return;

    if (timedOut) {
        warn("异步命令超时 [{}]: {}", taskId, t->command.toStdString());
        if (t->process->state() != QProcess::NotRunning) {
            t->process->kill();
            t->process->waitForFinished(1000);
        }
    } else {
        info("异步命令已完成 [{}]: {} (exitCode: {})",
             taskId, t->command.toStdString(), t->process->exitCode());
    }

    QJsonObject result;
    result["taskId"]   = taskId;
    result["stdout"]   = t->stdoutBuf;
    result["stderr"]   = t->stderrBuf;
    result["timedOut"] = timedOut;

    if (timedOut) {
        result["exitCode"] = -1;
        result["error"]    = "Command timed out";
    } else {
        int exitCode = t->process->exitCode();
        result["exitCode"] = exitCode;
        if (t->process->exitStatus() != QProcess::NormalExit)
            result["error"] = "Process crashed";
        else if (exitCode != 0)
            result["error"] = QString("Command failed with exit code %1").arg(exitCode);
    }

    QJSValue cb  = t->callback;
    QString  cmd = t->command;
    int      exit = result["exitCode"].toInt();
    cleanupTask(taskId);

    emit finished(taskId, cmd, exit);
    if (timedOut) emit errorOccurred(taskId, cmd, "Command timed out");

    if (cb.isCallable()) {
        QJSValueList args;
        args << cb.engine()->toScriptValue(result);
        cb.call(args);
    }
}

void ShellExecutor::cleanupTask(int taskId) {
    auto* t = m_tasks.take(taskId);
    if (!t) return;

    if (t->timer) {
        t->timer->stop();
        t->timer->deleteLater();
    }

    if (t->process) {
        t->process->disconnect();
        if (t->process->state() != QProcess::NotRunning) {
            t->process->kill();
            t->process->waitForFinished(1000);
        }
        t->process->deleteLater();
    }

    delete t;
    emit activeCountChanged(m_tasks.size());
}

// ============================================================
// 分离执行
// ============================================================

void ShellExecutor::startDetached(const QString& command) {
    bool ok = QProcess::startDetached("/bin/sh", QStringList{"-c", command});
    if (ok) info("分离执行命令: {}", command.toStdString());
    else    error("分离执行命令失败: {}", command.toStdString());
}

// ============================================================
// Kill
// ============================================================

void ShellExecutor::kill() {
    if (m_tasks.isEmpty()) {
        debug("kill() 被调用，但没有正在运行的异步命令");
        return;
    }
    warn("终止所有异步命令 (共 {} 个)", m_tasks.size());
    for (int id : m_tasks.keys())
        cleanupTask(id);
}

void ShellExecutor::kill(int taskId) {
    if (!m_tasks.contains(taskId)) {
        warn("kill({}) 被调用，但该任务不存在", taskId);
        return;
    }
    warn("终止异步命令 [{}]", taskId);
    cleanupTask(taskId);
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
    emit timeoutChanged(ms);
    info("Shell 命令超时时间已设置为 {} ms", m_timeoutMs);
}

int ShellExecutor::getTimeout() const {
    return m_timeoutMs;
}

int ShellExecutor::activeCount() const {
    return m_tasks.size();
}

} // namespace mod

static auto& s_ensureShellInit = mod::ShellExecutor::getInstance();
