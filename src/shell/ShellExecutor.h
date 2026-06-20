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
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QTimer>

namespace mod {

class ShellExecutor : public QObject, public Singleton<ShellExecutor>, private Logger {
    Q_OBJECT
    QML_SINGLETON
    QML_NAMED_ELEMENT(Shell)
    Q_PROPERTY(int timeout     READ getTimeout   WRITE setTimeout  NOTIFY timeoutChanged)
    Q_PROPERTY(int activeCount READ activeCount                    NOTIFY activeCountChanged)

public:
    explicit ShellExecutor(QObject* parent = nullptr);
    ~ShellExecutor();

    Q_INVOKABLE QString     exec(const QString& command);
    Q_INVOKABLE QJsonObject execWithResult(const QString& command);

    // 返回 task ID，可传入 kill(taskId) 精确终止
    Q_INVOKABLE int  execAsync(const QString& command, QJSValue callback);
    Q_INVOKABLE void startDetached(const QString& command);

    Q_INVOKABLE void kill();            // 终止所有任务
    Q_INVOKABLE void kill(int taskId);  // 终止指定任务

    void setTimeout(int ms);
    int  getTimeout() const;
    int  activeCount() const;

signals:
    void started(int taskId, const QString& command);
    void finished(int taskId, const QString& command, int exitCode);
    void stdoutReady(int taskId, const QString& command, const QString& data);
    void stderrReady(int taskId, const QString& command, const QString& data);
    void errorOccurred(int taskId, const QString& command, const QString& errorMessage);
    void timeoutChanged(int ms);
    void activeCountChanged(int count);

private:
    enum class SyncResult { Ok, FailedToStart, TimedOut };

    struct AsyncTask {
        int      id;
        QString  command;
        QJSValue callback;
        QString  stdoutBuf;
        QString  stderrBuf;
        QProcess* process = nullptr;
        QTimer*   timer   = nullptr;
    };

    SyncResult  runSync(QProcess& process, const QString& command);
    QJsonObject buildResult(QProcess& process, SyncResult sr) const;

    // 完成一个任务：构建结果、发信号、调回调、清理
    void finishTask(int taskId, bool timedOut);
    void cleanupTask(int taskId);

    int  m_timeoutMs   = 5000;
    int  m_nextTaskId  = 1;

    QMap<int, AsyncTask*> m_tasks;

    friend class Singleton<ShellExecutor>;
};

} // namespace mod
