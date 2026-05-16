// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <QVector>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "common/service/Logger.h"

namespace mod::chatbot {

// 单个会话的完整数据
struct SessionData {
    QString                          id;
    QString                          title;
    QString                          createdAt;
    QString                          updatedAt;
    QVector<QPair<QString, QString>> messages; // {role, content}
};

class ChatBot : public QObject, public Singleton<ChatBot>, private Logger {
    Q_OBJECT

    // QML 属性：API 设置
    Q_PROPERTY(QString apiKey READ getApiKey WRITE setApiKey NOTIFY apiKeyChanged)
    Q_PROPERTY(QString apiEndpoint READ getApiEndpoint WRITE setApiEndpoint NOTIFY apiEndpointChanged)
    Q_PROPERTY(QString model READ getModel WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(qreal temperature READ getTemperature WRITE setTemperature NOTIFY temperatureChanged)
    Q_PROPERTY(QString defaultPrompt READ getDefaultPrompt WRITE setDefaultPrompt NOTIFY defaultPromptChanged)
    Q_PROPERTY(bool isStreaming READ getIsStreaming WRITE setIsStreaming NOTIFY isStreamingChanged)
    Q_PROPERTY(bool isAvailable READ isAvailable CONSTANT)
    Q_PROPERTY(QVariantList messages READ getMessages NOTIFY messagesChanged)
    Q_PROPERTY(QString currentSessionId READ getCurrentSessionId NOTIFY sessionSwitched)

public:
    // 可供 QML 调用的方法
    Q_INVOKABLE void    sendMessage(const QString& message, const QString& fileRefs = QString());
    Q_INVOKABLE bool    isAvailable();                                     // 检查是否已配置必要设置
    Q_INVOKABLE void    reloadConfig();                                    // 重载配置
    Q_INVOKABLE void    clearHistory();                                    // 清除历史记录
    Q_INVOKABLE void    saveMessages();                                    // 保存聊天记录
    Q_INVOKABLE QString markdownToHtml(const QString& markdown);           // 将 Markdown 文本转换为 HTML
    Q_INVOKABLE void    truncateHistory(int index);                        // 截断历史记录到指定索引
    Q_INVOKABLE void    editMessage(int index, const QString& newContent); // 编辑指定索引的消息
    Q_INVOKABLE void    deleteMessage(int index);                          // 删除指定索引的单条消息
    Q_INVOKABLE void    regenerateMessage(int index);                      // 重新生成指定索引的 AI 消息

    // --- 多会话管理接口 ---
    Q_INVOKABLE QString      getSessions();         // 返回所有会话的 JSON 字符串（不含消息内容，仅摘要）
    Q_INVOKABLE QString      getCurrentSessionId(); // 返回当前活动会话的 ID
    Q_INVOKABLE bool         switchSession(const QString& sessionId);         // 切换到指定会话
    Q_INVOKABLE QString      createSession(const QString& title = QString()); // 创建新会话，返回新会话 ID
    Q_INVOKABLE bool         deleteSession(const QString& sessionId);         // 删除指定会话
    Q_INVOKABLE bool         renameSession(const QString& sessionId, const QString& newTitle); // 重命名会话
    Q_INVOKABLE QVariantList getSessionMessages(const QString& sessionId);                     // 获取指定会话的消息列表

    // --- 多模型管理接口 (ChatBox 风格) ---
    Q_INVOKABLE QString getModels();                            // 返回所有模型的 JSON 字符串
    Q_INVOKABLE bool    addModel(const QString& modelJson);     // 添加/更新模型
    Q_INVOKABLE bool    removeModel(const QString& modelId);    // 删除指定模型
    Q_INVOKABLE bool    setActiveModel(const QString& modelId); // 切换活动模型
    Q_INVOKABLE QString getActiveModel();                       // 返回当前活动模型信息的 JSON 字符串
    Q_INVOKABLE bool    loadModelsFile();                       // 从磁盘重新加载 models.json

    // --- 多提示词管理接口 ---
    Q_INVOKABLE QString getPrompts();                             // 返回所有提示词的 JSON 字符串
    Q_INVOKABLE bool    addPrompt(const QString& promptJson);     // 添加/更新提示词
    Q_INVOKABLE bool    removePrompt(const QString& promptId);    // 删除指定提示词
    Q_INVOKABLE bool    setActivePrompt(const QString& promptId); // 切换活动提示词
    Q_INVOKABLE QString getActivePrompt();                        // 返回当前活动提示词信息的 JSON 字符串

    // 设置获取器
    QString      getApiKey() const;
    QString      getApiEndpoint() const;
    QString      getModel() const;
    qreal        getTemperature() const;
    QString      getDefaultPrompt() const;
    bool         getIsStreaming() const;
    QVariantList getMessages() const;

    // 设置器（带信号）
    void setApiKey(const QString& key);
    void setApiEndpoint(const QString& endpoint);
    void setModel(const QString& model);
    void setTemperature(qreal temp);
    void setDefaultPrompt(const QString& prompt);
    void setIsStreaming(bool streaming);

signals:
    // QML 通信信号
    void messageReceived(const QString& content, bool isComplete = true);
    void streamStart();
    void streamChunk(const QString& content);
    void streamEnd();
    void errorOccurred(const QString& error);
    void apiKeyChanged();
    void apiEndpointChanged();
    void modelChanged();
    void temperatureChanged();
    void defaultPromptChanged();
    void isStreamingChanged();
    void messagesChanged();

    // 模型管理信号
    void modelsChanged();

    // 提示词管理信号
    void promptsChanged();

    // 会话管理信号
    void sessionsChanged();
    void sessionSwitched(const QString& sessionId);

private:
    friend Singleton<ChatBot>;
    explicit ChatBot(); // 构造函数

    // 网络基础设施
    QNetworkAccessManager* m_networkManager;
    QList<QNetworkReply*>  m_activeReplies; // 跟踪活动的网络请求

    // 配置
    QString m_apiKey;
    QString m_apiEndpoint;
    QString m_model;
    qreal   m_temperature;
    QString m_defaultPrompt;
    bool    m_isStreaming;

    // 对话历史 - 当前会话的消息
    QVector<QPair<QString, QString>>&       currentMessages();
    const QVector<QPair<QString, QString>>& currentMessages() const;
    static const int                        MAX_HISTORY_SIZE = 100; // 每个会话最多保留 100 条消息对

    // 多会话管理
    QMap<QString, SessionData> m_sessions;         // 所有会话，key 为 sessionId
    QString                    m_currentSessionId; // 当前活动会话 ID
    QString                    m_sessionsPath;     // sessions.json 路径

    void    initSessions();         // 初始化会话数据（加载或创建默认）
    void    saveSessions();         // 保存 sessions.json 到磁盘
    QString sessionsFilePath();     // 获取 sessions.json 路径
    void    ensureCurrentSession(); // 确保存在一个当前会话

    // 网络请求方法
    void makeApiRequest(const QJsonArray& messages);
    void handleNetworkReply(QNetworkReply* reply, bool isStream);

    // 流式传输支持
    QString m_currentStreamBuffer;
    QString m_responseBuffer; // 用于累积响应数据，处理不完整的 SSE 行

    // 多模型管理
    json    m_modelsData; // 完整的 models.json 数据
    QString m_modelsPath; // models.json 的完整路径

    void    initModels();                           // 初始化模型数据（加载或创建默认）
    void    saveModels();                           // 保存 models.json 到磁盘
    QString modelsFilePath();                       // 获取 models.json 路径
    void    applyModelConfig(const json& modelObj); // 将模型配置应用到当前会话

    // 多提示词管理
    json    m_promptsData; // 完整的 prompts.json 数据
    QString m_promptsPath; // prompts.json 的完整路径

    void    initPrompts();     // 初始化提示词数据（加载或创建默认）
    void    savePrompts();     // 保存 prompts.json 到磁盘
    QString promptsFilePath(); // 获取 prompts.json 路径
};

} // namespace mod::chatbot