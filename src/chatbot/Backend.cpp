// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "chatbot/Backend.h"

#include "common/Event.h"
#include "common/Utils.h"
#include "common/service/Logger.h"
#include "common/util/System.h"
#include "mod/Config.h"
#include "mod/Mod.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QQmlContext>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace mod::chatbot {

// 将 Markdown 文本转换为 HTML
QString ChatBot::markdownToHtml(const QString& markdown) {
    QTextDocument doc;

    // 设置默认字体以匹配 UI
    QFont defaultFont;
    defaultFont.setPixelSize(12);
    defaultFont.setFamily("OPPOSans, OPPOSans M, Microsoft YaHei, SimSun, Arial");
    doc.setDefaultFont(defaultFont);

    // 设置默认样式表以调整行高
    doc.setDefaultStyleSheet("body { line-height: 1; }");

    doc.setMarkdown(markdown);
    return doc.toHtml();
}


// 构造函数：初始化日志器、网络管理器，注册 QML 绑定供前端使用
ChatBot::ChatBot()
: Logger("ChatBot"),
  m_networkManager(new QNetworkAccessManager(this)),
  m_apiKey(""),
  m_apiEndpoint("https://api.deepseek.com/v1/chat/completions"),
  m_model("deepseek-v4-flash"),
  m_temperature(0.7),
  m_defaultPrompt("你是一个有用的助手，使用中文回复用户的问题。"),
  m_isStreaming(true),
  m_currentStreamBuffer(""),
  m_responseBuffer("") {
    info("ChatBot 初始化完成");

    // 从配置加载设置
    auto& config = mod::Config::getInstance();
    json  aiCfg  = config.read("ai");
    if (!aiCfg.is_null() && aiCfg.contains("chatbot")) {
        json chatbotCfg = aiCfg["chatbot"];
        if (chatbotCfg.contains("api_key")) m_apiKey = QString::fromStdString(chatbotCfg["api_key"]);
        if (chatbotCfg.contains("api_endpoint")) m_apiEndpoint = QString::fromStdString(chatbotCfg["api_endpoint"]);
        if (chatbotCfg.contains("model")) m_model = QString::fromStdString(chatbotCfg["model"]);
        if (chatbotCfg.contains("temperature")) m_temperature = chatbotCfg["temperature"];
        if (chatbotCfg.contains("default_prompt"))
            m_defaultPrompt = QString::fromStdString(chatbotCfg["default_prompt"]);
        if (chatbotCfg.contains("streaming")) m_isStreaming = chatbotCfg["streaming"];
    }

    // 连接到 UI 初始化事件，将实例注册到 QML 上下文
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("chatbot", this);
    });

    // 初始化多模型管理（从独立 models.json 加载）
    initModels();
    // 初始化多提示词管理（从独立 prompts.json 加载）
    initPrompts();
    // 初始化多会话管理（从独立 sessions.json 加载）
    initSessions();
}

// 重载配置
void ChatBot::reloadConfig() {
    // debug("重载配置");

    auto& config = mod::Config::getInstance();
    json  aiCfg  = config.read("ai");
    if (!aiCfg.is_null() && aiCfg.contains("chatbot")) {
        json chatbotCfg = aiCfg["chatbot"];

        // 重载各项配置
        if (chatbotCfg.contains("api_key")) {
            QString oldKey = m_apiKey;
            m_apiKey       = QString::fromStdString(chatbotCfg["api_key"]);
            if (oldKey != m_apiKey) emit apiKeyChanged();
        }
        if (chatbotCfg.contains("api_endpoint")) {
            QString oldEndpoint = m_apiEndpoint;
            m_apiEndpoint       = QString::fromStdString(chatbotCfg["api_endpoint"]);
            if (oldEndpoint != m_apiEndpoint) emit apiEndpointChanged();
        }
        if (chatbotCfg.contains("model")) {
            QString oldModel = m_model;
            m_model          = QString::fromStdString(chatbotCfg["model"]);
            if (oldModel != m_model) emit modelChanged();
        }
        if (chatbotCfg.contains("temperature")) {
            qreal oldTemp = m_temperature;
            m_temperature = chatbotCfg["temperature"];
            if (oldTemp != m_temperature) emit temperatureChanged();
        }
        if (chatbotCfg.contains("default_prompt")) {
            QString oldPrompt = m_defaultPrompt;
            m_defaultPrompt   = QString::fromStdString(chatbotCfg["default_prompt"]);
            if (oldPrompt != m_defaultPrompt) emit defaultPromptChanged();
        }
        if (chatbotCfg.contains("streaming")) {
            bool oldStreaming = m_isStreaming;
            m_isStreaming     = chatbotCfg["streaming"];
            if (oldStreaming != m_isStreaming) emit isStreamingChanged();
        }
    }
}

// 获取当前会话消息的引用（可读写）
QVector<QPair<QString, QString>>& ChatBot::currentMessages() { return m_sessions[m_currentSessionId].messages; }

// 获取当前会话消息的引用（只读）
const QVector<QPair<QString, QString>>& ChatBot::currentMessages() const {
    return m_sessions[m_currentSessionId].messages;
}

// 确保存在当前会话
void ChatBot::ensureCurrentSession() {
    if (m_currentSessionId.isEmpty() || !m_sessions.contains(m_currentSessionId)) {
        if (m_sessions.isEmpty()) {
            // 没有任何会话，创建一个默认的
            createSession("新对话");
        } else {
            // 选择第一个会话作为当前
            m_currentSessionId = m_sessions.firstKey();
        }
    }
}

// 获取 sessions.json 文件路径
QString ChatBot::sessionsFilePath() {
    if (m_sessionsPath.isEmpty()) {
        m_sessionsPath =
            QString::fromStdString((mod::util::getModuleFileInfo().absolutePath() + "sessions.json").toStdString());
    }
    return m_sessionsPath;
}

// 保存所有会话到 sessions.json
void ChatBot::saveSessions() {
    json root;
    root["activeSessionId"] = m_currentSessionId.toStdString();
    json sessionsArr        = json::array();

    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        const SessionData& session = it.value();
        json               sessionObj;
        sessionObj["id"]        = session.id.toStdString();
        sessionObj["title"]     = session.title.toStdString();
        sessionObj["createdAt"] = session.createdAt.toStdString();
        sessionObj["updatedAt"] = session.updatedAt.toStdString();

        json messagesArr = json::array();
        for (const auto& msg : session.messages) {
            json msgObj;
            msgObj["role"]    = msg.first.toStdString();
            msgObj["content"] = msg.second.toStdString();
            messagesArr.push_back(msgObj);
        }
        sessionObj["messages"] = messagesArr;
        sessionsArr.push_back(sessionObj);
    }
    root["sessions"] = sessionsArr;

    QFile file(sessionsFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(root.dump(4).c_str());
        file.close();
    } else {
        error("无法写入 sessions.json: {}", sessionsFilePath().toStdString());
    }
}

// 初始化会话数据（从 sessions.json 加载或创建默认）
void ChatBot::initSessions() {
    auto  path = sessionsFilePath();
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        try {
            json root = json::parse(file.readAll().toStdString());
            file.close();

            if (root.contains("sessions") && root["sessions"].is_array()) {
                for (const auto& sessionObj : root["sessions"]) {
                    SessionData session;
                    session.id        = QString::fromStdString(sessionObj.value("id", ""));
                    session.title     = QString::fromStdString(sessionObj.value("title", "未命名对话"));
                    session.createdAt = QString::fromStdString(sessionObj.value("createdAt", ""));
                    session.updatedAt = QString::fromStdString(sessionObj.value("updatedAt", ""));

                    if (sessionObj.contains("messages") && sessionObj["messages"].is_array()) {
                        for (const auto& msgObj : sessionObj["messages"]) {
                            QString role    = QString::fromStdString(msgObj.value("role", ""));
                            QString content = QString::fromStdString(msgObj.value("content", ""));
                            session.messages.append(qMakePair(role, content));
                        }
                    }

                    if (!session.id.isEmpty()) {
                        m_sessions.insert(session.id, session);
                    }
                }
            }

            if (root.contains("activeSessionId")) {
                QString activeId = QString::fromStdString(root["activeSessionId"]);
                if (m_sessions.contains(activeId)) {
                    m_currentSessionId = activeId;
                }
            }

            info("已加载 {} 个会话", m_sessions.size());
        } catch (const std::exception& e) {
            warn("sessions.json 解析失败: {}, 使用默认配置", e.what());
        }
    }

    ensureCurrentSession();
    if (!m_sessions.contains(m_currentSessionId)) {
        // 创建默认会话
        m_currentSessionId = createSession("新对话");
    }
}
bool ChatBot::isAvailable() {
    bool available = !m_apiKey.isEmpty();
    // debug("ChatBot 可用性: {}", available ? "是" : "否");
    return available;
}

// 发送消息（使用内部历史记录）
void ChatBot::sendMessage(const QString& message, const QString& fileRefs) {
    // debug("发送消息: {}", message.toStdString());

    // 构建消息数组，首先添加系统消息
    QJsonArray messages;

    // 添加系统消息
    QJsonObject systemMsg;
    systemMsg["role"]    = "system";
    systemMsg["content"] = m_defaultPrompt; // 使用用户设置的系统提示词
    messages.append(systemMsg);

    // 解析文件引用并构建结构化上下文（在系统消息和历史消息之间插入）
    if (!fileRefs.isEmpty()) {
        QJsonDocument doc = QJsonDocument::fromJson(fileRefs.toUtf8());
        if (doc.isArray() && !doc.array().isEmpty()) {
            QJsonArray files       = doc.array();
            QString    contextText = "用户引用了以下文件作为代码审查/分析的上下文：\n\n";

            for (int i = 0; i < files.size(); ++i) {
                QJsonObject file    = files[i].toObject();
                QString     path    = file["path"].toString();
                QString     content = file["content"].toString();
                QString     lang    = file["language"].toString();

                contextText += QString("---\n## 文件: %1\n```%2\n%3\n```\n\n").arg(path, lang, content);
            }

            QJsonObject fileCtxMsg;
            fileCtxMsg["role"]    = "system";
            fileCtxMsg["content"] = contextText;
            messages.append(fileCtxMsg);

            debug("已附加 {} 个文件作为上下文", files.size());
        }
    }

    // 添加历史消息
    for (const auto& pair : currentMessages()) {
        QJsonObject msg;
        msg["role"]    = pair.first; // "user" 或 "assistant"
        msg["content"] = pair.second;
        messages.append(msg);
    }

    // 添加当前消息
    QJsonObject currentUserMsg;
    currentUserMsg["role"]    = "user";
    currentUserMsg["content"] = message;
    messages.append(currentUserMsg);

    // debug("消息总数: {}", messages.size());

    // 添加用户消息到当前会话历史
    currentMessages().append(qMakePair(QString("user"), message));
    if (currentMessages().size() > MAX_HISTORY_SIZE) {
        currentMessages().removeFirst();
    }

    // 更新会话时间戳
    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();

    makeApiRequest(messages);
}

// 执行 API 请求
void ChatBot::makeApiRequest(const QJsonArray& messages) {
    if (m_apiKey.isEmpty()) {
        // debug("API 密钥未设置");
        emit errorOccurred("API 密钥未设置\n请进入「设置」页面配置有效的 API 密钥后重试");
        return;
    }

    // 取消所有进行中的旧请求，避免多个流同时进行
    for (QNetworkReply* reply : m_activeReplies) {
        if (reply && !reply->isFinished()) {
            reply->abort();
        }
    }
    m_activeReplies.clear();

    // debug("开始发送 API 请求到: {}", m_apiEndpoint.toStdString());
    // debug("使用模型: {}", m_model.toStdString());
    // debug("流式传输: {}", m_isStreaming ? "是" : "否");

    // 构建请求体
    QJsonObject requestBody;
    requestBody["model"]       = m_model;
    requestBody["messages"]    = messages;
    requestBody["temperature"] = m_temperature;
    requestBody["stream"]      = m_isStreaming;

    QJsonDocument requestDoc(requestBody);
    QByteArray    requestData = requestDoc.toJson(QJsonDocument::Compact);

    debug("请求数据: {}", QString(requestData).toStdString());

    // 创建网络请求
    QNetworkRequest request;
    QUrl            url(m_apiEndpoint);
    request.setUrl(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_apiKey).toUtf8());
    request.setRawHeader("Content-Type", "application/json");
    request.setRawHeader("User-Agent", "PenMods ChatBot/1.0");

    // 发送请求
    QNetworkReply* reply = m_networkManager->post(request, requestData);

    // 将请求添加到活动列表中
    m_activeReplies.append(reply);

    // 如果是流式传输，发出开始信号
    if (m_isStreaming) {
        m_currentStreamBuffer.clear();
        m_responseBuffer.clear();
        debug("开始流式传输");
        emit streamStart();
    } else {
        debug("使用完整响应模式");
    }

    // 处理响应
    connect(reply, &QNetworkReply::finished, [this, reply]() {
        debug("网络请求完成，状态码: {}", static_cast<int>(reply->error()));
        if (reply->error() != QNetworkReply::NoError) {
            debug("网络错误: {}", reply->errorString().toStdString());
        }

        // 从活动列表中移除已完成的请求
        m_activeReplies.removeAll(reply);

        handleNetworkReply(reply, m_isStreaming);
    });

    // 如果是流式传输，还需要处理 readyRead 信号
    if (m_isStreaming) {
        connect(reply, &QNetworkReply::readyRead, [this, reply]() {
            QByteArray data = reply->readAll();
            if (!data.isEmpty()) {
                debug("接收到流式数据，长度: {}", data.size());
                m_responseBuffer += QString::fromUtf8(data);

                // 按行分割响应
                QStringList lines     = m_responseBuffer.split("\n", Qt::SkipEmptyParts);
                QString     remaining = "";

                for (const QString& line : lines) {
                    QString trimmedLine = line.trimmed();

                    // debug("处理行: {}", trimmedLine.toStdString());

                    if (trimmedLine.startsWith("data: ")) {
                        QString jsonData = trimmedLine.mid(6); // 移除 "data: " 前缀

                        // 检查是否是结束标记
                        if (jsonData.trimmed() == "[DONE]") {
                            // debug("收到流结束标记");
                            emit streamEnd();
                            continue;
                        }

                        // 解析 JSON 数据
                        QJsonDocument doc = QJsonDocument::fromJson(jsonData.toUtf8());
                        if (doc.isObject()) {
                            // debug("解析 JSON 数据成功");
                            QJsonObject obj = doc.object();
                            if (obj.contains("choices") && obj["choices"].isArray()) {
                                QJsonArray choices = obj["choices"].toArray();
                                if (!choices.isEmpty()) {
                                    QJsonObject choice = choices[0].toObject();
                                    if (choice.contains("delta") && choice["delta"].isObject()) {
                                        QJsonObject delta = choice["delta"].toObject();
                                        if (delta.contains("content") && delta["content"].isString()) {
                                            QString content = delta["content"].toString();
                                            if (!content.isEmpty()) {
                                                emit streamChunk(content);
                                                m_currentStreamBuffer += content;
                                                // debug("接收到流片段: {}", content.toStdString());
                                            }
                                        }
                                    }
                                }
                            }
                        } else {
                            debug("JSON 解析失败: {}", jsonData.toStdString());
                        }
                    } else if (!trimmedLine.isEmpty() && !trimmedLine.startsWith(":")) {
                        // 非注释行且非数据行，可能是未完成的数据，保留在缓冲区
                        remaining = line;
                    }
                }

                // 保留未完整处理的行
                if (m_responseBuffer.endsWith("\n")) {
                    m_responseBuffer = "";
                } else {
                    // 查找最后一个完整的行数据
                    int lastNewline = m_responseBuffer.lastIndexOf("\n");
                    if (lastNewline != -1 && lastNewline < m_responseBuffer.length() - 1) {
                        // 保留最后未完成的一行
                        m_responseBuffer = m_responseBuffer.mid(lastNewline + 1);
                    } else {
                        m_responseBuffer = remaining; // 保留最后一行
                    }
                }
            }
        });
    }
}

// 处理网络响应
void ChatBot::handleNetworkReply(QNetworkReply* reply, bool isStream) {
    if (reply->error() == QNetworkReply::NoError) {
        // debug("网络请求成功完成");
        if (isStream) {
            // 流式传输：在 readyRead 中处理，这里只需要处理可能的结束逻辑
            if (!m_currentStreamBuffer.isEmpty()) {
                // debug("流式传输完成，将最终消息存入历史: {}", m_currentStreamBuffer.toStdString());
                currentMessages().append(qMakePair(QString("assistant"), m_currentStreamBuffer));
                if (currentMessages().size() > MAX_HISTORY_SIZE) {
                    currentMessages().removeFirst();
                }
                m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
                saveSessions();
                emit messagesChanged();
            } else {
                // debug("流传输结束，但没有内容");
            }
        } else {
            // 完整响应处理
            QByteArray response = reply->readAll();
            // debug("完整响应数据: {}", QString(response).toStdString());
            QJsonDocument doc = QJsonDocument::fromJson(response);
            if (doc.isObject()) {
                QJsonObject obj = doc.object();
                if (obj.contains("choices") && obj["choices"].isArray()) {
                    QJsonArray choices = obj["choices"].toArray();
                    if (!choices.isEmpty()) {
                        QJsonObject choice = choices[0].toObject();
                        if (choice.contains("message") && choice["message"].isObject()) {
                            QJsonObject message = choice["message"].toObject();
                            if (message.contains("content") && message["content"].isString()) {
                                QString content = message["content"].toString();
                                // debug("解析到完整响应内容: {}", content.toStdString());
                                emit messageReceived(content, true);

                                // 添加 AI 回复到当前会话历史
                                currentMessages().append(qMakePair(QString("assistant"), content));
                                if (currentMessages().size() > MAX_HISTORY_SIZE) {
                                    currentMessages().removeFirst();
                                }
                                m_sessions[m_currentSessionId].updatedAt =
                                    QDateTime::currentDateTime().toString(Qt::ISODate);
                                saveSessions();
                                emit messagesChanged();
                            }
                        }
                    }
                } else {
                    // debug("响应中没有找到 choices 字段");
                }
            } else {
                debug("响应 JSON 解析失败");
            }
        }
    } else {
        QString errorString = reply->errorString();
        debug("API 请求失败: {}", errorString.toStdString());

        // 如果是流模式，也需要发出结束信号
        if (isStream) {
            emit streamEnd();
        }

        // 尝试读取 HTTP 状态码
        int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // 尝试读取响应体中的错误信息
        QByteArray responseBody = reply->readAll();
        QString    detailMsg;
        if (!responseBody.isEmpty()) {
            QJsonDocument errDoc = QJsonDocument::fromJson(responseBody);
            if (errDoc.isObject()) {
                QJsonObject errObj = errDoc.object();
                if (errObj.contains("error") && errObj["error"].isObject()) {
                    QJsonObject errorDetail = errObj["error"].toObject();
                    if (errorDetail.contains("message")) detailMsg = errorDetail["message"].toString();
                }
            }
            if (detailMsg.isEmpty()) detailMsg = QString::fromUtf8(responseBody).left(200);
        }

        // 根据错误类型给出建议
        QString suggestion;
        if (httpStatus == 401 || httpStatus == 403) {
            suggestion = "API 密钥无效或已过期，请在设置中检查密钥";
        } else if (httpStatus == 429) {
            suggestion = "请求频率过高，请稍后再试";
        } else if (httpStatus >= 500) {
            suggestion = "AI 服务端异常，请稍后重试";
        } else if (
            reply->error() == QNetworkReply::ConnectionRefusedError
            || reply->error() == QNetworkReply::HostNotFoundError || reply->error() == QNetworkReply::TimeoutError
        ) {
            suggestion = "无法连接到 AI 服务，请检查网络连接";
        } else {
            suggestion = errorString;
        }

        // 构建详细错误信息
        QString     fullError = "API 请求失败";
        QStringList parts;
        if (httpStatus > 0) parts << QString("状态码: %1").arg(httpStatus);
        if (!detailMsg.isEmpty()) parts << detailMsg;
        parts << suggestion;
        fullError = fullError + "\n" + parts.join("\n");

        emit errorOccurred(fullError);
    }

    // 从活动列表中移除已完成的请求
    m_activeReplies.removeAll(reply);

    reply->deleteLater();
}

// 编辑指定索引的消息
void ChatBot::editMessage(int index, const QString& newContent) {
    auto& msgs = currentMessages();
    if (index < 0 || index >= msgs.size()) {
        return; // 索引超出范围，直接返回
    }

    // 替换指定索引的消息内容
    QString currentRole = msgs[index].first;
    msgs[index]         = qMakePair(currentRole, newContent);

    // 截断该索引之后的所有消息
    msgs.resize(index + 1);

    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();

    // 如果编辑的是用户消息，且它是当前最后一条消息，则自动重新发起AI请求
    if (currentRole == "user" && index == msgs.size() - 1) {
        // 构建消息数组，首先添加系统消息
        QJsonArray messages;

        // 添加系统消息
        QJsonObject systemMsg;
        systemMsg["role"]    = "system";
        systemMsg["content"] = m_defaultPrompt;
        messages.append(systemMsg);

        // 添加历史消息
        for (const auto& pair : msgs) {
            QJsonObject msg;
            msg["role"]    = pair.first;
            msg["content"] = pair.second;
            messages.append(msg);
        }

        makeApiRequest(messages);
    }
}

// 截断历史记录到指定索引
void ChatBot::truncateHistory(int index) {
    auto& msgs = currentMessages();
    if (index < 0 || index >= msgs.size()) {
        return; // 索引超出范围，直接返回
    }

    // 保留从 0 到 index -1 的消息，移除从 index 开始的所有消息
    msgs.resize(index);

    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();
}

// 删除指定索引的单条消息
void ChatBot::deleteMessage(int index) {
    auto& msgs = currentMessages();
    if (index < 0 || index >= msgs.size()) {
        return; // 索引超出范围，直接返回
    }

    msgs.remove(index);

    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();
}

// 重新生成指定索引的 AI 消息
void ChatBot::regenerateMessage(int index) {
    auto& msgs = currentMessages();
    if (index < 0 || index >= msgs.size()) {
        return; // 索引超出范围，直接返回
    }

    // 检查指定索引的消息是否为 AI 消息
    if (msgs[index].first != "assistant") {
        error("只能重新生成 AI 消息，索引 {} 处的消息不是 AI 消息", index);
        return;
    }

    // 获取 AI 消息对应之前的用户消息
    int userMessageIndex = -1;
    for (int i = index - 1; i >= 0; i--) {
        if (msgs[i].first == "user") {
            userMessageIndex = i;
            break;
        }
    }

    if (userMessageIndex == -1) {
        error("找不到对应于 AI 消息的用户消息，索引: {}", index);
        return;
    }

    // 截断从用户消息开始之后的所有消息
    msgs.resize(userMessageIndex + 1);

    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();

    // 通知 UI 更新消息列表
    emit messagesChanged();

    // 重新发送用户消息以获取新的 AI 回复
    QString userMessage = msgs[userMessageIndex].second;

    // 构建消息数组
    QJsonArray messages;

    QJsonObject systemMsg;
    systemMsg["role"]    = "system";
    systemMsg["content"] = m_defaultPrompt;
    messages.append(systemMsg);

    for (const auto& pair : msgs) {
        QJsonObject msg;
        msg["role"]    = pair.first;
        msg["content"] = pair.second;
        messages.append(msg);
    }

    makeApiRequest(messages);
}

// 清除历史记录
void ChatBot::clearHistory() {
    // 终止所有活动的网络请求
    for (QNetworkReply* reply : m_activeReplies) {
        if (reply && !reply->isFinished()) {
            reply->abort();
        }
    }
    m_activeReplies.clear();

    // 清除当前会话的消息
    currentMessages().clear();
    m_sessions[m_currentSessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit messagesChanged();
}

// 保存聊天记录
void ChatBot::saveMessages() {
    // 构造默认保存目录
    QString saveDir = "/userdisk/Music/AI/Saved";
    QDir    dir(saveDir);
    if (!dir.exists()) {
        dir.mkpath(saveDir); // 创建目录（如果不存在）
    }

    // 生成带时间戳的文件名
    QString fileName = "chat_" + QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".md";
    QString savePath = saveDir + "/" + fileName;

    // debug("保存聊天记录到: {}", savePath.toStdString());

    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error("无法打开文件进行写入: {}", savePath.toStdString());
        emit errorOccurred("保存聊天记录失败\n路径: " + savePath + "\n请检查磁盘空间或目录权限");
        return;
    }

    QTextStream out(&file);
    out.setCodec("UTF-8");

    // 写入 Markdown 头部
    out << tr("# AI 聊天记录\n");
    out << QDateTime::currentDateTime().toString("保存时间: yyyy-MM-dd hh:mm:ss") << tr("\n\n");

    // 遍历当前会话历史记录并写入
    for (const auto& pair : currentMessages()) {
        if (pair.first == "user") {
            out << tr("我：\n") << pair.second << tr("\n\n");
        } else if (pair.first == "assistant") {
            out << tr("AI：\n") << pair.second << tr("\n\n");
        }
    }

    file.close();

    showToast("保存成功");

    // info("聊天记录已成功保存到: {}", savePath.toStdString());
}

// API 密钥相关的 getter 和 setter
QString ChatBot::getApiKey() const { return m_apiKey; }

void ChatBot::setApiKey(const QString& key) {
    if (m_apiKey != key) {
        debug("设置 API 密钥");
        m_apiKey = key;

        // 保存到配置
        auto& config = mod::Config::getInstance();
        json  aiCfg  = config.read("ai");
        if (aiCfg.is_null()) aiCfg = json::object();

        if (!aiCfg.contains("chatbot")) {
            aiCfg["chatbot"] = json::object();
        }
        aiCfg["chatbot"]["api_key"] = key.toStdString();
        config.write("ai", aiCfg, true);

        emit apiKeyChanged();
    }
}

// API 端点相关的 getter 和 setter
QString ChatBot::getApiEndpoint() const { return m_apiEndpoint; }

void ChatBot::setApiEndpoint(const QString& endpoint) {
    if (m_apiEndpoint != endpoint) {
        // debug("设置 API 端点: {}", endpoint.toStdString());
        m_apiEndpoint = endpoint;

        // 保存到配置
        auto& config = mod::Config::getInstance();
        json  aiCfg  = config.read("ai");
        if (aiCfg.is_null()) aiCfg = json::object();

        if (!aiCfg.contains("chatbot")) {
            aiCfg["chatbot"] = json::object();
        }
        aiCfg["chatbot"]["api_endpoint"] = endpoint.toStdString();
        config.write("ai", aiCfg, true);

        emit apiEndpointChanged();
    }
}

// 模型相关的 getter 和 setter
QString ChatBot::getModel() const { return m_model; }

void ChatBot::setModel(const QString& model) {
    if (m_model != model) {
        // debug("设置模型: {}", model.toStdString());
        m_model = model;

        // 保存到配置
        auto& config = mod::Config::getInstance();
        json  aiCfg  = config.read("ai");
        if (aiCfg.is_null()) aiCfg = json::object();

        if (!aiCfg.contains("chatbot")) {
            aiCfg["chatbot"] = json::object();
        }
        aiCfg["chatbot"]["model"] = model.toStdString();
        config.write("ai", aiCfg, true);

        emit modelChanged();
    }
}

// 温度相关的 getter 和 setter
qreal ChatBot::getTemperature() const { return m_temperature; }

void ChatBot::setTemperature(qreal temp) {
    if (m_temperature != temp) {
        // debug("设置温度: {}", temp);
        m_temperature = temp;

        // 保存到配置
        auto& config = mod::Config::getInstance();
        json  aiCfg  = config.read("ai");
        if (aiCfg.is_null()) aiCfg = json::object();

        if (!aiCfg.contains("chatbot")) {
            aiCfg["chatbot"] = json::object();
        }
        aiCfg["chatbot"]["temperature"] = temp;
        config.write("ai", aiCfg, true);

        emit temperatureChanged();
    }
}

// 默认提示词相关的 getter 和 setter
QString ChatBot::getDefaultPrompt() const { return m_defaultPrompt; }

void ChatBot::setDefaultPrompt(const QString& prompt) {
    if (m_defaultPrompt != prompt) {
        // debug("设置默认提示词: {}", prompt.toStdString());
        m_defaultPrompt = prompt;

        // 保存到配置
        auto& config = mod::Config::getInstance();
        json  aiCfg  = config.read("ai");
        if (aiCfg.is_null()) aiCfg = json::object();

        if (!aiCfg.contains("chatbot")) {
            aiCfg["chatbot"] = json::object();
        }
        aiCfg["chatbot"]["default_prompt"] = prompt.toStdString();
        config.write("ai", aiCfg, true);

        emit defaultPromptChanged();
    }
}

// 流式传输相关的 getter 和 setter
bool ChatBot::getIsStreaming() const { return m_isStreaming; }

// 获取消息列表
QVariantList ChatBot::getMessages() const {
    QVariantList messageList;
    if (m_sessions.contains(m_currentSessionId)) {
        const auto& msgs = m_sessions[m_currentSessionId].messages;
        for (const auto& pair : msgs) {
            QVariantMap message;
            message["role"]    = pair.first;
            message["content"] = pair.second;
            messageList.append(message);
        }
    }
    return messageList;
}

void ChatBot::setIsStreaming(bool streaming) {
    if (m_isStreaming != streaming) {
        // debug("设置流式传输: {}", streaming ? "是" : "否");
        m_isStreaming = streaming;

        // 保存到配置
        auto& config = mod::Config::getInstance();
        json  aiCfg  = config.read("ai");
        if (aiCfg.is_null()) aiCfg = json::object();

        if (!aiCfg.contains("chatbot")) {
            aiCfg["chatbot"] = json::object();
        }
        aiCfg["chatbot"]["streaming"] = streaming;
        config.write("ai", aiCfg, true);

        emit isStreamingChanged();
    }
}

// ==============================================================
// 多模型管理（独立 models.json 文件，ChatBox 风格）
// ==============================================================

QString ChatBot::modelsFilePath() {
    if (m_modelsPath.isEmpty()) {
        m_modelsPath =
            QString::fromStdString((mod::util::getModuleFileInfo().absolutePath() + "models.json").toStdString());
    }
    return m_modelsPath;
}

void ChatBot::initModels() {
    auto  path = modelsFilePath();
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        try {
            m_modelsData = json::parse(file.readAll().toStdString());
            file.close();
        } catch (...) {
            warn("models.json 解析失败，使用默认配置");
            m_modelsData = json::object();
        }
    }

    // 验证数据结构，缺失则创建默认
    if (!m_modelsData.contains("models") || !m_modelsData["models"].is_array() || m_modelsData["models"].empty()) {
        info("创建默认模型配置");
        m_modelsData["models"] = json::array();
        // 将当前 config.json 中的设置导入为第一个模型
        json defaultModel;
        defaultModel["id"]          = m_model.toStdString();
        defaultModel["name"]        = "DeepSeek Chat";
        defaultModel["provider"]    = "DeepSeek";
        defaultModel["endpoint"]    = m_apiEndpoint.toStdString();
        defaultModel["apiKey"]      = m_apiKey.toStdString();
        defaultModel["modelId"]     = m_model.toStdString();
        defaultModel["temperature"] = m_temperature;
        m_modelsData["models"].push_back(defaultModel);
        m_modelsData["activeModelId"] = m_model.toStdString();
        saveModels();
    }

    // 应用活动模型到当前会话
    if (m_modelsData.contains("activeModelId")) {
        std::string activeId = m_modelsData["activeModelId"];
        for (const auto& model : m_modelsData["models"]) {
            if (model.contains("id") && model["id"] == activeId) {
                applyModelConfig(model);
                break;
            }
        }
    }
}

void ChatBot::saveModels() {
    QFile file(modelsFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(m_modelsData.dump(4).c_str());
        file.close();
    } else {
        error("无法写入 models.json: {}", modelsFilePath().toStdString());
    }
}

void ChatBot::applyModelConfig(const json& modelObj) {
    if (modelObj.contains("endpoint")) m_apiEndpoint = QString::fromStdString(modelObj["endpoint"]);
    if (modelObj.contains("apiKey")) m_apiKey = QString::fromStdString(modelObj["apiKey"]);
    if (modelObj.contains("modelId")) m_model = QString::fromStdString(modelObj["modelId"]);
    if (modelObj.contains("temperature") && modelObj["temperature"].is_number())
        m_temperature = modelObj["temperature"];
}

QString ChatBot::getModels() { return QString::fromStdString(m_modelsData.dump(2)); }

bool ChatBot::addModel(const QString& modelJson) {
    QJsonDocument doc = QJsonDocument::fromJson(modelJson.toUtf8());
    if (!doc.isObject()) return false;

    QJsonObject input    = doc.object();
    std::string id       = input["id"].toString().toStdString();
    std::string modelId  = input["modelId"].toString().toStdString();
    std::string endpoint = input["endpoint"].toString().toStdString();

    if (id.empty() || modelId.empty() || endpoint.empty()) {
        warn("添加模型失败：缺少必填字段 (id, modelId, endpoint)");
        return false;
    }

    // 构建 json 对象
    json newModel;
    newModel["id"]          = id;
    newModel["name"]        = input["name"].toString().toStdString();
    newModel["provider"]    = input["provider"].toString().toStdString();
    newModel["endpoint"]    = endpoint;
    newModel["apiKey"]      = input["apiKey"].toString().toStdString();
    newModel["modelId"]     = modelId;
    newModel["temperature"] = input.contains("temperature") ? input["temperature"].toDouble() : 0.7;

    // 查找是否已存在同 id，更新或追加
    bool updated = false;
    for (auto& model : m_modelsData["models"]) {
        if (model["id"] == id) {
            model   = newModel;
            updated = true;
            break;
        }
    }
    if (!updated) {
        m_modelsData["models"].push_back(newModel);
    }

    saveModels();
    emit modelsChanged();
    info("模型已{}: {}", updated ? "更新" : "添加", id);
    return true;
}

bool ChatBot::removeModel(const QString& modelId) {
    std::string id     = modelId.toStdString();
    auto&       models = m_modelsData["models"];
    for (auto it = models.begin(); it != models.end(); ++it) {
        if ((*it)["id"] == id) {
            models.erase(it);
            // 如果删除的是活动模型，将活动模型切换为第一个可用模型
            if (m_modelsData["activeModelId"] == id && !models.empty()) {
                m_modelsData["activeModelId"] = models[0]["id"];
                applyModelConfig(models[0]);
                emit apiEndpointChanged();
                emit apiKeyChanged();
                emit modelChanged();
                emit temperatureChanged();
            }
            saveModels();
            emit modelsChanged();
            info("模型已删除: {}", id);
            return true;
        }
    }
    warn("删除模型失败：未找到 id={}", id);
    return false;
}

bool ChatBot::setActiveModel(const QString& modelId) {
    std::string id = modelId.toStdString();
    for (const auto& model : m_modelsData["models"]) {
        if (model["id"] == id) {
            m_modelsData["activeModelId"] = id;
            applyModelConfig(model);
            saveModels();
            emit modelsChanged();
            emit apiEndpointChanged();
            emit apiKeyChanged();
            emit modelChanged();
            emit temperatureChanged();
            info("活动模型已切换为: {}", id);
            return true;
        }
    }
    warn("切换模型失败：未找到 id={}", id);
    return false;
}

QString ChatBot::getActiveModel() {
    std::string activeId = m_modelsData.value("activeModelId", "");
    for (const auto& model : m_modelsData["models"]) {
        if (model["id"] == activeId) {
            json result        = model;
            result["isActive"] = true;
            return QString::fromStdString(result.dump(2));
        }
    }
    return "{}";
}

bool ChatBot::loadModelsFile() {
    m_modelsPath.clear();
    initModels();
    emit modelsChanged();
    return true;
}

// ==============================================================
// 多提示词管理（独立 prompts.json 文件）
// ==============================================================

QString ChatBot::promptsFilePath() {
    if (m_promptsPath.isEmpty()) {
        m_promptsPath =
            QString::fromStdString((mod::util::getModuleFileInfo().absolutePath() + "prompts.json").toStdString());
    }
    return m_promptsPath;
}

void ChatBot::initPrompts() {
    auto  path = promptsFilePath();
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        try {
            m_promptsData = json::parse(file.readAll().toStdString());
            file.close();
        } catch (...) {
            warn("prompts.json 解析失败，使用默认配置");
            m_promptsData = json::object();
        }
    }

    // 验证数据结构，缺失则创建默认
    if (!m_promptsData.contains("prompts") || !m_promptsData["prompts"].is_array()
        || m_promptsData["prompts"].empty()) {
        info("创建默认提示词配置");
        m_promptsData["prompts"] = json::array();
        // 添加默认提示词
        json defaultPrompt;
        defaultPrompt["id"]      = "default";
        defaultPrompt["name"]    = "通用助手";
        defaultPrompt["content"] = m_defaultPrompt.toStdString();
        m_promptsData["prompts"].push_back(defaultPrompt);
        m_promptsData["activePromptId"] = "default";
        savePrompts();
    }

    // 应用活动提示词到当前会话
    if (m_promptsData.contains("activePromptId")) {
        std::string activeId = m_promptsData["activePromptId"];
        for (const auto& prompt : m_promptsData["prompts"]) {
            if (prompt.contains("id") && prompt["id"] == activeId) {
                QString oldPrompt = m_defaultPrompt;
                m_defaultPrompt   = QString::fromStdString(prompt.value("content", ""));
                if (oldPrompt != m_defaultPrompt) emit defaultPromptChanged();
                break;
            }
        }
    }
}

void ChatBot::savePrompts() {
    QFile file(promptsFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(m_promptsData.dump(4).c_str());
        file.close();
    } else {
        error("无法写入 prompts.json: {}", promptsFilePath().toStdString());
    }
}

QString ChatBot::getPrompts() { return QString::fromStdString(m_promptsData.dump(2)); }

bool ChatBot::addPrompt(const QString& promptJson) {
    QJsonDocument doc = QJsonDocument::fromJson(promptJson.toUtf8());
    if (!doc.isObject()) return false;

    QJsonObject input   = doc.object();
    std::string id      = input["id"].toString().toStdString();
    std::string name    = input["name"].toString().toStdString();
    std::string content = input["content"].toString().toStdString();

    if (id.empty() || name.empty()) {
        warn("添加提示词失败：缺少必填字段 (id, name)");
        return false;
    }

    // 构建 json 对象
    json newPrompt;
    newPrompt["id"]      = id;
    newPrompt["name"]    = name;
    newPrompt["content"] = content;

    // 查找是否已存在同 id，更新或追加
    bool updated = false;
    for (auto& prompt : m_promptsData["prompts"]) {
        if (prompt["id"] == id) {
            prompt  = newPrompt;
            updated = true;
            break;
        }
    }
    if (!updated) {
        m_promptsData["prompts"].push_back(newPrompt);
    }

    savePrompts();
    emit promptsChanged();
    info("提示词已{}: {}", updated ? "更新" : "添加", id);
    return true;
}

bool ChatBot::removePrompt(const QString& promptId) {
    std::string id      = promptId.toStdString();
    auto&       prompts = m_promptsData["prompts"];
    for (auto it = prompts.begin(); it != prompts.end(); ++it) {
        if ((*it)["id"] == id) {
            prompts.erase(it);
            // 如果删除的是活动提示词，切换为第一个可用提示词
            if (m_promptsData["activePromptId"] == id && !prompts.empty()) {
                m_promptsData["activePromptId"] = prompts[0]["id"];
                QString oldPrompt               = m_defaultPrompt;
                m_defaultPrompt                 = QString::fromStdString(prompts[0].value("content", ""));
                if (oldPrompt != m_defaultPrompt) emit defaultPromptChanged();
            }
            savePrompts();
            emit promptsChanged();
            info("提示词已删除: {}", id);
            return true;
        }
    }
    warn("删除提示词失败：未找到 id={}", id);
    return false;
}

bool ChatBot::setActivePrompt(const QString& promptId) {
    std::string id = promptId.toStdString();
    for (const auto& prompt : m_promptsData["prompts"]) {
        if (prompt["id"] == id) {
            m_promptsData["activePromptId"] = id;
            QString oldPrompt               = m_defaultPrompt;
            m_defaultPrompt                 = QString::fromStdString(prompt.value("content", ""));
            if (oldPrompt != m_defaultPrompt) emit defaultPromptChanged();
            savePrompts();
            emit promptsChanged();
            info("活动提示词已切换为: {}", id);
            return true;
        }
    }
    warn("切换提示词失败：未找到 id={}", id);
    return false;
}

QString ChatBot::getActivePrompt() {
    std::string activeId = m_promptsData.value("activePromptId", "");
    for (const auto& prompt : m_promptsData["prompts"]) {
        if (prompt["id"] == activeId) {
            json result        = prompt;
            result["isActive"] = true;
            return QString::fromStdString(result.dump(2));
        }
    }
    return "{}";
}

// ==============================================================
// 多会话管理
// ==============================================================

QString ChatBot::getCurrentSessionId() { return m_currentSessionId; }

QString ChatBot::getSessions() {
    json result;
    result["activeSessionId"] = m_currentSessionId.toStdString();
    json sessionsArr          = json::array();

    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        const SessionData& session = it.value();
        json               sessionObj;
        sessionObj["id"]           = session.id.toStdString();
        sessionObj["title"]        = session.title.toStdString();
        sessionObj["createdAt"]    = session.createdAt.toStdString();
        sessionObj["updatedAt"]    = session.updatedAt.toStdString();
        sessionObj["messageCount"] = session.messages.size();
        sessionsArr.push_back(sessionObj);
    }
    result["sessions"] = sessionsArr;

    return QString::fromStdString(result.dump(2));
}

bool ChatBot::switchSession(const QString& sessionId) {
    if (!m_sessions.contains(sessionId)) {
        warn("切换会话失败：未找到 id={}", sessionId.toStdString());
        return false;
    }

    if (m_currentSessionId == sessionId) {
        return true; // 已经是当前会话
    }

    m_currentSessionId = sessionId;
    saveSessions();
    emit messagesChanged();
    emit sessionSwitched(sessionId);
    info("已切换到会话: {}", sessionId.toStdString());
    return true;
}

QString ChatBot::createSession(const QString& title) {
    SessionData session;
    session.id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.title     = title.isEmpty() ? "新对话" : title;
    session.createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    session.updatedAt = session.createdAt;

    m_sessions.insert(session.id, session);
    m_currentSessionId = session.id;
    saveSessions();
    emit messagesChanged();
    emit sessionsChanged();
    emit sessionSwitched(session.id);
    info("已创建新会话: {} ({})", session.title.toStdString(), session.id.toStdString());
    return session.id;
}

bool ChatBot::deleteSession(const QString& sessionId) {
    if (!m_sessions.contains(sessionId)) {
        warn("删除会话失败：未找到 id={}", sessionId.toStdString());
        return false;
    }

    // 不能删除最后一个会话
    if (m_sessions.size() <= 1) {
        warn("不能删除唯一的会话");
        return false;
    }

    m_sessions.remove(sessionId);

    // 如果删除的是当前会话，切换到第一个可用会话
    if (m_currentSessionId == sessionId) {
        m_currentSessionId = m_sessions.firstKey();
        emit messagesChanged();
        emit sessionSwitched(m_currentSessionId);
    }

    saveSessions();
    emit sessionsChanged();
    info("已删除会话: {}", sessionId.toStdString());
    return true;
}

bool ChatBot::renameSession(const QString& sessionId, const QString& newTitle) {
    if (!m_sessions.contains(sessionId)) {
        warn("重命名会话失败：未找到 id={}", sessionId.toStdString());
        return false;
    }

    m_sessions[sessionId].title     = newTitle;
    m_sessions[sessionId].updatedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    saveSessions();
    emit sessionsChanged();
    info("会话已重命名: {} -> {}", sessionId.toStdString(), newTitle.toStdString());
    return true;
}

QVariantList ChatBot::getSessionMessages(const QString& sessionId) {
    QVariantList messageList;
    if (m_sessions.contains(sessionId)) {
        const auto& msgs = m_sessions[sessionId].messages;
        for (const auto& pair : msgs) {
            QVariantMap message;
            message["role"]    = pair.first;
            message["content"] = pair.second;
            messageList.append(message);
        }
    }
    return messageList;
}

} // namespace mod::chatbot