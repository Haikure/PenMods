// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "common/Utils.h"

#include "base/YPointer.h"

#include <QUuid>

namespace mod {

std::string exec(const QString& cmd) { return exec(cmd.toUtf8().constData()); }

std::string exec(const char* cmd) {
    char        buffer[128];
    std::string result;
    FILE*       pipe = popen(cmd, "r");
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    try {
        while (fgets(buffer, sizeof buffer, pipe) != nullptr) {
            result += buffer;
        }
    } catch (...) {
        pclose(pipe);
        throw;
    }
    pclose(pipe);
    return result.substr(0, result.length() - 1);
}

// 12.333 -> 12.3, if n=1
double dec(double d, uint16 n) { return round(d * pow(10, n)) / pow(10, n); }

std::string readFileNoLast(const char* path) {
    auto str = readFile(path);
    return str.substr(0, str.length() - 1);
}

std::string readFile(const char* path) {
    std::ifstream ifile;
    ifile.open(path, std::ios::in);
    if (!ifile.good()) {
        return "";
    }
    std::string str((std::istreambuf_iterator<char>(ifile)), std::istreambuf_iterator<char>());
    return str;
}

void showToast(const std::string& content, const QColor& theme) {
    PEN_CALL(uint64, "_ZN7YGlobal9showToastERK7QStringRK6QColor", YGlobal*, const QString&, const QColor&)
    (YPointer<YGlobal>::getInstance(), QString::fromStdString(content), theme);
}

bool judgeIsLegalFileName(const QString& filename) {
    return !(
        filename.contains(QStringLiteral("/")) || filename.contains(QStringLiteral("\\"))
        || filename.contains(QStringLiteral("\"")) || filename.contains(QStringLiteral("\n"))
        || filename.contains(QStringLiteral(":")) || filename.contains(QStringLiteral("?"))
        || filename.contains(QStringLiteral("*")) || filename.contains(QStringLiteral("|"))
        || filename.contains(QStringLiteral("<")) || filename.contains(QStringLiteral(">"))
        || filename.contains(QStringLiteral("$"))
    );
}

QString generateUUID() { return QUuid::createUuid().toString().remove("{").remove("}"); }

double fuzzyLrcMatch(const QString& songName, const QString& lrcName) {
    if (songName.isEmpty() || lrcName.isEmpty())
        return 0.0;
    if (songName == lrcName)
        return 1.0;
    // 一方包含另一方
    if (songName.contains(lrcName) || lrcName.contains(songName)) {
        double ratio = static_cast<double>(qMin(songName.length(), lrcName.length()))
                       / qMax(songName.length(), lrcName.length());
        return 0.85 * ratio;
    }
    // 去掉常见的修饰后缀后再比较
    auto stripSuffix = [](QString s) -> QString {
        static const QStringList patterns = {"-", "—", "–", "(", "（", "[", "【"};
        for (const auto& p : patterns) {
            int idx = s.indexOf(p);
            if (idx > 0)
                s = s.left(idx).trimmed();
        }
        return s;
    };
    QString a = stripSuffix(songName);
    QString b = stripSuffix(lrcName);
    if (a == b)
        return 0.9;
    if (a.contains(b) || b.contains(a))
        return 0.8;
    // 计算公共前缀长度
    int commonPrefix = 0;
    int minLen       = qMin(a.length(), b.length());
    while (commonPrefix < minLen && a[commonPrefix] == b[commonPrefix])
        ++commonPrefix;
    if (commonPrefix > 0)
        return 0.5 * static_cast<double>(commonPrefix) / qMax(a.length(), b.length());
    return 0.0;
}

} // namespace mod
