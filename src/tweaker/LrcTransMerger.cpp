// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * 本文件属于 PenMods 开源项目。
 *
 * 钩住 YMediaPlayerManager::loadLrcFromFile，将带有隐式翻译对
 * （相同时间戳，原文 + 译文）的 .lrc 文件转换为 readFromFileByJson
 * 原生支持的 JSON 格式。
 *
 * 策略：
 *   不让原始函数直接加载原始 LRC（会产生杂乱的交错文本行），
 *   而是拦截 .lrc 文件，通过匹配时间戳将原文↔译文行配对，
 *   生成结构化的 JSON 文件，然后将调用重定向到该 JSON。
 *   临时 JSON 文件在原始函数返回后被清理。
 *
 * JSON 格式（单位：秒，readFromFileByJson 内部会转换为毫秒）：
 *   [
 *     {"text": "...", "start": <double>, "end": <double>, "trans": "...", "words": [...]},
 *     ...
 *   ]
 *
 * LRC 隐式配对：
 *   [00:14.260]Monday is a rainy day    ← 原文（第一次出现的时间戳）
 *   [00:14.260]周一的雨连绵不断          ← 译文（第二次出现的时间戳）
 *
 * 元数据（如 [00:00.0]作词: Mili）会作为已翻译行保留或适当跳过。
 */

#include "base/Hook.h"

#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QVector>
#include <QMap>
#include <QString>
#include <QUuid>

namespace {

// -------------------------------------------------------------------
// LRC 解析器：提取按时间戳排序的句子对
// -------------------------------------------------------------------
struct LrcSentence {
    double  startTime = 0.0;    // 开始时间（秒）
    QString text;               // 原文歌词文本
    QString trans;              // 关联的译文（可能为空）
};

using LrcSentenceList = QVector<LrcSentence>;

/**
 * 解析 LRC 文件，通过匹配时间戳将原文/译文行配对。
 *
 * LRC 行格式：[mm:ss.xxx]内容 或 [m:ss.x]内容
 *
 * 配对策略：
 *   - 时间戳第一次出现 → 原文（句子）
 *   - 相同时间戳第二次出现 → 前一句的译文
 *
 * 返回按原始 LRC 行顺序排列的句子列表，时间单位为秒。
 */
LrcSentenceList parseLrc(const QString &filePath)
{
    LrcSentenceList result;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;

    QTextStream in(&file);

    // LRC 时间戳：[mm:ss.xx] 或 [mm:ss.xxx] 或 [m:ss.x]
    // 支持 1-2 位分钟、1-2 位秒钟、1-3 位毫秒
    static const QRegularExpression lrcRe(
        QStringLiteral(R"(\[(\d{1,2}):(\d{1,2})\.(\d{1,3})\](.*))"));

    // 追踪：时间戳（秒）→ 在结果中的索引（用于检测重复时间戳）
    QMap<double, int> tsToSentenceIdx;

    while (!in.atEnd()) {
        const QString line    = in.readLine();
        const QString trimmed = line.trimmed();
        const auto    match   = lrcRe.match(trimmed);
        if (!match.hasMatch())
            continue;

        const int min      = match.captured(1).toInt();
        const int sec      = match.captured(2).toInt();
        const int msRaw    = match.captured(3).toInt();
        const QString content = match.captured(4).trimmed();

        if (content.isEmpty())
            continue;

        // 标准化毫秒数：1 位 = 100ms，2 位 = 10ms，3 位 = 1ms
        double ms = static_cast<double>(msRaw);
        const int msDigits = match.captured(3).length();
        if (msDigits == 1)
            ms *= 100.0;
        else if (msDigits == 2)
            ms *= 10.0;

        // 转换为秒存储
        const double timeSec = min * 60.0 + sec + ms / 1000.0;

        // --- 分类 ---

        // 跳过 LRC 格式标签（s:, tag:），但不跳过 "作词: Mili" 等元数据
        if (content.startsWith(QLatin1String("s:")) ||
            content.startsWith(QLatin1String("tag:")))
            continue;

        // 手动匹配已知的元数据关键词并跳过，避免误杀含冒号的正常歌词
        static const QStringList metadataPrefixes = {
            // 中文元数据标签
            QStringLiteral("作词"),   QStringLiteral("作曲"),
            QStringLiteral("编曲"),   QStringLiteral("制作"),
            QStringLiteral("演唱"),   QStringLiteral("歌手"),
            QStringLiteral("专辑"),   QStringLiteral("歌名"),
            QStringLiteral("曲名"),   QStringLiteral("原唱"),
            QStringLiteral("翻唱"),   QStringLiteral("混音"),
            QStringLiteral("录音"),   QStringLiteral("出品"),
            QStringLiteral("原曲"),   QStringLiteral("填词"),
            QStringLiteral("谱曲"),   QStringLiteral("监制"),
            QStringLiteral("词"),     QStringLiteral("曲"),
            QStringLiteral("唱"),
            // 英文元数据标签
            QStringLiteral("ar"),     QStringLiteral("al"),
            QStringLiteral("ti"),     QStringLiteral("by"),
            QStringLiteral("offset"),
        };
        bool isMetadata = false;
        for (const auto &prefix : metadataPrefixes) {
            if (content.startsWith(prefix, Qt::CaseInsensitive) &&
                content.size() > prefix.size() &&
                content.at(prefix.size()) == QLatin1Char(':')) {
                isMetadata = true;
                break;
            }
        }
        if (isMetadata) {
            continue;
        }

        // --- 歌词行（原文或译文）---

        const auto it = tsToSentenceIdx.constFind(timeSec);
        if (it != tsToSentenceIdx.constEnd()) {
            // 重复时间戳 → 现有句子的译文
            LrcSentence &sentence = result[it.value()];
            if (sentence.trans.isEmpty()) {
                sentence.trans = content;
            }
        } else {
            // 第一次出现 → 原文
            LrcSentence sentence;
            sentence.startTime = timeSec;
            sentence.text      = content;
            tsToSentenceIdx.insert(timeSec, result.size());
            result.append(sentence);
        }
    }

    file.close();
    return result;
}

/**
 * 从解析后的句子列表构建 JSON 数组（时间单位为秒）。
 *
 * 输出格式：
 *   [
 *     {"text": "...", "start": <double>, "end": <double>, "trans": "...", "words": [...]},
 *     ...
 *   ]
 */
QJsonArray toJsonArray(const LrcSentenceList &sentences)
{
    QJsonArray arr;
    const int  count = sentences.size();

    for (int i = 0; i < count; ++i) {
        const LrcSentence &s = sentences[i];

        // 结束时间 = 下一句的开始时间，最后一句则为当前开始时间 + 5 秒
        const double endTime = (i + 1 < count)
                                   ? sentences[i + 1].startTime
                                   : s.startTime + 5.0;

        QJsonObject obj;
        obj[QStringLiteral("text")]  = QJsonValue(s.text);
        obj[QStringLiteral("start")] = QJsonValue(s.startTime);
        obj[QStringLiteral("end")]   = QJsonValue(endTime);

        if (!s.trans.isEmpty()) {
            obj[QStringLiteral("trans")] = QJsonValue(s.trans);
        }

        // LRC 无单词级时间戳，words 字段省略
        arr.append(obj);
    }

    return arr;
}

/**
 * 生成唯一的临时文件路径用于 JSON 输出。
 * 格式：/tmp/playerfollow_<uuid>.json
 */
QString generateTempPath()
{
    QString uuidStr = QUuid::createUuid().toString();
    uuidStr.remove(QLatin1Char('{'));
    uuidStr.remove(QLatin1Char('}'));
    return QStringLiteral("/tmp/playerfollow_")
           + uuidStr
           + QStringLiteral(".json");
}

/**
 * 将 JSON 数组写入文件。
 * 成功返回 true，失败返回 false。
 */
bool writeJsonFile(const QString &path, const QJsonArray &arr)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    QJsonDocument doc(arr);
    const QByteArray bytes = doc.toJson(QJsonDocument::Compact);
    const qint64 written = file.write(bytes);
    file.close();

    return written == bytes.size();
}

} // anonymous namespace

// -------------------------------------------------------------------
// 钩子：YMediaPlayerManager::loadLrcFromFile(const QString& path)
//
// 对于 .lrc 文件：转换为带有配对原文+译文的 JSON，
// 然后让原始函数直接加载该 JSON。
// 对于非 .lrc 文件：直接透传原始函数，不做处理。
// -------------------------------------------------------------------
PEN_HOOK(void, _ZN19YMediaPlayerManager15loadLrcFromFileERK7QString,
         void* _this, const QString* path) {

    // ---------------------------------------------------------------
    // 1. 验证输入 / 非 LRC 文件直接透传
    // ---------------------------------------------------------------
    if (!path || path->isEmpty()) {
        origin(_this, path);
        return;
    }

    const QFileInfo fi(*path);

    // 仅拦截 .lrc 文件；其他所有文件（包括 .json）由原始函数原生处理。
    if (fi.suffix().compare(QLatin1String("lrc"), Qt::CaseInsensitive) != 0) {
        origin(_this, path);
        return;
    }

    // ---------------------------------------------------------------
    // 2. 解析 LRC 文件为配对的句子列表
    // ---------------------------------------------------------------
    const LrcSentenceList sentences = parseLrc(*path);
    if (sentences.isEmpty()) {
        // 解析失败 → 回退到原始行为
        origin(_this, path);
        return;
    }

    // ---------------------------------------------------------------
    // 3. 转换为 JSON 数组
    // ---------------------------------------------------------------
    const QJsonArray jsonArr = toJsonArray(sentences);
    if (jsonArr.isEmpty()) {
        origin(_this, path);
        return;
    }

    // ---------------------------------------------------------------
    // 4. 将 JSON 写入临时文件
    // ---------------------------------------------------------------
    const QString tmpPath = generateTempPath();
    if (!writeJsonFile(tmpPath, jsonArr)) {
        // 写入临时文件失败 → 回退
        origin(_this, path);
        return;
    }

    // ---------------------------------------------------------------
    // 5. 重定向：使用 JSON 文件路径调用原始函数
    // ---------------------------------------------------------------
    origin(_this, &tmpPath);

    // ---------------------------------------------------------------
    // 6. 清理：删除临时 JSON 文件
    // ---------------------------------------------------------------
    QFile::remove(tmpPath);
}