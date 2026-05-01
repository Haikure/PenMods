// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "chatbot/Backend.h"

#include "common/Event.h"
#include "common/service/Logger.h"
#include "mod/Config.h"
#include "mod/Mod.h"
#include "common/Utils.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QQmlContext>
#include <QRegularExpression>
#include <QTimer>
#include <QTextDocument>
#include <QUrl>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QDir>

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
  m_model("deepseek-chat"),
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

// 检查功能是否可用：API 密钥已设置
bool ChatBot::isAvailable() {
    bool available = !m_apiKey.isEmpty();
    // debug("ChatBot 可用性: {}", available ? "是" : "否");
    return available;
}

// 发送消息（使用内部历史记录）
void ChatBot::sendMessage(const QString& message) {
    // debug("发送消息: {}", message.toStdString());

    // 构建消息数组，首先添加系统消息
    QJsonArray messages;

    // 添加系统消息
    QJsonObject systemMsg;
    systemMsg["role"]    = "system";
    systemMsg["content"] = m_defaultPrompt; // 使用用户设置的系统提示词
    messages.append(systemMsg);

    // 添加历史消息
    for (const auto& pair : m_conversationHistory) {
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

    // 添加用户消息到历史
    m_conversationHistory.enqueue(qMakePair(QString("user"), message));
    if (m_conversationHistory.size() > MAX_HISTORY_SIZE) {
        m_conversationHistory.dequeue();
    }
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
                m_conversationHistory.enqueue(qMakePair(QString("assistant"), m_currentStreamBuffer));
                if (m_conversationHistory.size() > MAX_HISTORY_SIZE) {
                    m_conversationHistory.dequeue();
                }
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

                                // 添加 AI 回复到历史
                                m_conversationHistory.enqueue(qMakePair(QString("assistant"), content));
                                if (m_conversationHistory.size() > MAX_HISTORY_SIZE) {
                                    m_conversationHistory.dequeue();
                                }
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
        QString detailMsg;
        if (!responseBody.isEmpty()) {
            QJsonDocument errDoc = QJsonDocument::fromJson(responseBody);
            if (errDoc.isObject()) {
                QJsonObject errObj = errDoc.object();
                if (errObj.contains("error") && errObj["error"].isObject()) {
                    QJsonObject errorDetail = errObj["error"].toObject();
                    if (errorDetail.contains("message"))
                        detailMsg = errorDetail["message"].toString();
                }
            }
            if (detailMsg.isEmpty())
                detailMsg = QString::fromUtf8(responseBody).left(200);
        }

        // 根据错误类型给出建议
        QString suggestion;
        if (httpStatus == 401 || httpStatus == 403) {
            suggestion = "API 密钥无效或已过期，请在设置中检查密钥";
        } else if (httpStatus == 429) {
            suggestion = "请求频率过高，请稍后再试";
        } else if (httpStatus >= 500) {
            suggestion = "AI 服务端异常，请稍后重试";
        } else if (reply->error() == QNetworkReply::ConnectionRefusedError ||
                   reply->error() == QNetworkReply::HostNotFoundError ||
                   reply->error() == QNetworkReply::TimeoutError) {
            suggestion = "无法连接到 AI 服务，请检查网络连接";
        } else {
            suggestion = errorString;
        }

        // 构建详细错误信息
        QString fullError = "API 请求失败";
        QStringList parts;
        if (httpStatus > 0)
            parts << QString("状态码: %1").arg(httpStatus);
        if (!detailMsg.isEmpty())
            parts << detailMsg;
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
    if (index < 0 || index >= m_conversationHistory.size()) {
        return; // 索引超出范围，直接返回
    }

    // 替换指定索引的消息内容
    // 获取当前角色（user 或 assistant）
    QString currentRole = m_conversationHistory[index].first;
    // 替换为新的内容
    m_conversationHistory[index] = qMakePair(currentRole, newContent);

    // 然后截断该索引之后的所有消息
    while (m_conversationHistory.size() > index + 1) {
        m_conversationHistory.removeLast();
    }

    emit messagesChanged();

    // 如果编辑的是用户消息，且它是当前最后一条消息，则自动重新发起AI请求
    if (currentRole == "user" && index == m_conversationHistory.size() - 1) {
        // 构建消息数组，首先添加系统消息
        QJsonArray messages;

        // 添加系统消息
        QJsonObject systemMsg;
        systemMsg["role"]    = "system";
        systemMsg["content"] = m_defaultPrompt; // 使用用户设置的系统提示词
        messages.append(systemMsg);

        // 添加历史消息
        for (const auto& pair : m_conversationHistory) {
            QJsonObject msg;
            msg["role"]    = pair.first; // "user" 或 "assistant"
            msg["content"] = pair.second;
            messages.append(msg);
        }

        makeApiRequest(messages);
    }
}

// 截断历史记录到指定索引
void ChatBot::truncateHistory(int index) {
    if (index < 0 || index >= m_conversationHistory.size()) {
        return; // 索引超出范围，直接返回
    }

    // 保留从 0 到 index -1 的消息，移除从 index 开始的所有消息
    while (m_conversationHistory.size() > index) {
        m_conversationHistory.removeLast();
    }

    emit messagesChanged();
}

// 删除指定索引的单条消息
void ChatBot::deleteMessage(int index) {
    if (index < 0 || index >= m_conversationHistory.size()) {
        return; // 索引超出范围，直接返回
    }

    // 由于 QQueue 没有 remove 方法，我们需要将其转换为可随机访问的容器
    // 将队列内容复制到 QVector 中进行操作
    QVector<QPair<QString, QString>> tempVector;
    for (int i = 0; i < m_conversationHistory.size(); ++i) {
        tempVector.append(m_conversationHistory[i]);
    }

    // 从 QVector 中删除指定索引
    if (index >= 0 && index < tempVector.size()) {
        tempVector.remove(index);
    }

    // 清空原队列并重新填充
    m_conversationHistory.clear();
    for (const auto& pair : tempVector) {
        m_conversationHistory.enqueue(pair);
    }

    emit messagesChanged();
}

// 重新生成指定索引的 AI 消息
void ChatBot::regenerateMessage(int index) {
    if (index < 0 || index >= m_conversationHistory.size()) {
        return; // 索引超出范围，直接返回
    }

    // 检查指定索引的消息是否为 AI 消息
    if (m_conversationHistory[index].first != "assistant") {
        error("只能重新生成 AI 消息，索引 {} 处的消息不是 AI 消息", index);
        return;
    }

    // 获取 AI 消息对应之前的用户消息
    int userMessageIndex = -1;
    for (int i = index - 1; i >= 0; i--) {
        if (m_conversationHistory[i].first == "user") {
            userMessageIndex = i;
            break;
        }
    }

    if (userMessageIndex == -1) {
        error("找不到对应于 AI 消息的用户消息，索引: {}", index);
        return;
    }

    // 截断从用户消息开始之后的所有消息
    while (m_conversationHistory.size() > userMessageIndex + 1) {
        m_conversationHistory.removeLast();
    }

    // 通知 UI 更新消息列表
    emit messagesChanged();

    // 重新发送用户消息以获取新的 AI 回复
    QString userMessage = m_conversationHistory[userMessageIndex].second;

    // 构建消息数组，首先添加系统消息
    QJsonArray messages;

    // 添加系统消息
    QJsonObject systemMsg;
    systemMsg["role"]    = "system";
    systemMsg["content"] = m_defaultPrompt; // 使用用户设置的系统提示词
    messages.append(systemMsg);

    // 添加历史消息
    for (const auto& pair : m_conversationHistory) {
        QJsonObject msg;
        msg["role"]    = pair.first; // "user" 或 "assistant"
        msg["content"] = pair.second;
        messages.append(msg);
    }

    makeApiRequest(messages);
}

// 清除历史记录
void ChatBot::clearHistory() {
    // debug("清除聊天历史记录");

    // 终止所有活动的网络请求
    for (QNetworkReply* reply : m_activeReplies) {
        if (reply && !reply->isFinished()) {
            // debug("终止活动请求");
            reply->abort(); // 中止请求
        }
    }
    m_activeReplies.clear(); // 清空活动请求列表

    // 清除历史记录
    m_conversationHistory.clear();
    emit messagesChanged();
}

// 保存聊天记录
void ChatBot::saveMessages() {
    // 构造默认保存目录
    QString saveDir = "/userdisk/Music/AI/Saved";
    QDir dir(saveDir);
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

    // 遍历历史记录并写入
    for (const auto& pair : m_conversationHistory) {
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
    for (const auto& pair : m_conversationHistory) {
        QVariantMap message;
        message["role"]    = pair.first;
        message["content"] = pair.second;
        messageList.append(message);
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

} // namespace mod::chatbot