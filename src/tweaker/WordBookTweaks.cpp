// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/WordBookTweaks.h"

#include "base/YEnum.h"
#include "base/YPointer.h"

#include "common/Event.h"
#include "common/Utils.h"

#include "filemanager/FileManager.h"

#include <QDir>
#include <QFile>
#include <QQmlContext>
#include <QSaveFile>
#include <QTimer>
#include <QVariant>


#define WB_EXPORT_FILE "/userdisk/Favorite/WordBook.json"

bool pickUpQuery = false;

namespace mod {

WordBookTweaks::WordBookTweaks() {

    mCfg = Config::getInstance().read(mClassName);

    mPhraseTab       = mCfg["phrase_tab"];
    mNoCaseSensitive = mCfg["nocase_sensitive"];

    // No Case Sensitive
    connect(&Event::getInstance(), &Event::beforeDatabasePrepareAsyncQuery, [this](QString& query) {
        // for no-case-sensitive;
        if (getNoCaseSensitive()) {
            query.replace("word = (:word)", "word = (:word) COLLATE NOCASE");
        }

        // for tab-type phrase, fix wordCount;
        if (query.startsWith("SELECT count(*) FROM table_wordbook") && getPhraseTab()) {
            auto tabType =
                PEN_CALL(uint32, "_ZNK16YWordBookManager7tabTypeEv", void*)(YPointer<YWordBookManager>::getInstance());
            if (tabType == 2) {
                query += " AND word NOT LIKE '% %'";
            } else if (tabType == 3) {
                query += " AND word LIKE '% %'";
            }
        }
    });

    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("workBookTweaks", this);
    });
}

void WordBookTweaks::doExport() {
#if PL_BUILD_YDP02X
    showToast("正在导出单词本");
    // see xrefs for: YWordbookDB::loadAllWords()
    // line 41~45
    // set queryId.
    *(byte*)(YPointer<YWordBookManager>::getInstance() + 128) = 1;
    *(uint64*)(YPointer<YWordBookManager>::getInstance() + 136) =
        PEN_CALL(uint64, "_ZNK11YWordbookDB12loadAllWordsEN12YEnumWrapper9ItemStateE", void*, uint32)(
            YPointer<YWordbookDB>::getInstance(),
            1
        );
    pickUpQuery = true;
#else
    if (mExporting) return;
    auto  load     = PEN_CALL(uint64, "_ZNK11YWordbookDB12loadAllWordsEN12YEnumWrapper9ItemStateE", void*, uint32);
    auto* database = YPointer<YWordbookDB>::getInstance();
    if (!load || !database) {
        showToast("单词本尚未就绪", "#E9900C");
        return;
    }
    setExporting(true);
    mExportProgress = 0;
    emit exportProgressChanged();
    mPendingExportId     = load(database, 1);
    const auto requestId = mPendingExportId;
    QTimer::singleShot(30000, this, [this, requestId] {
        if (mPendingExportId == requestId && mExporting) {
            mPendingExportId = 0;
            setExporting(false);
            showToast("读取单词本超时", "#E9900C");
        }
    });
#endif
}

bool WordBookTweaks::consumeExportResult(void* result) {
#if PL_BUILD_YDP03X
    // onExecFinished at 0x58f198 reads AsyncQueryResult::id at +0x50.
    // Own the request ID instead of writing the host manager's private flags.
    if (!mExporting || !mPendingExportId || !result) return false;
    const auto id = *reinterpret_cast<const uint64*>(static_cast<const char*>(result) + 0x50);
    if (id != mPendingExportId) return false;
    mPendingExportId = 0;
    if (!PEN_CALL(bool, "_ZNK8Database16AsyncQueryResult7isValidEv", void*)(result)) {
        setExporting(false);
        showToast("读取单词本失败", "#E9900C");
        return true;
    }
    const int count = PEN_CALL(int, "_ZNK8Database16AsyncQueryResult5countEv", void*)(result);
    mExportWords.clear();
    for (int i = 0; i < count; ++i) {
        const auto value =
            PEN_CALL(QVariant, "_ZNK8Database16AsyncQueryResult5valueEiRK7QString", void*, int, const QString&)(
                result,
                i,
                QStringLiteral("word")
            );
        if (!value.toString().isEmpty()) mExportWords.append(value.toString());
    }
    mExportWords.removeDuplicates();
    mExportData = {
        {"version", 100           },
        {"words",   json::array() },
        {"data",    json::object()}
    };
    mExportIndex = 0;
    QTimer::singleShot(0, this, &WordBookTweaks::exportNextWord);
    return true;
#else
    Q_UNUSED(result);
    return false;
#endif
}

void WordBookTweaks::exportNextWord() {
#if PL_BUILD_YDP03X
    if (!mExporting) return;
    if (mExportIndex < mExportWords.size()) {
        const QString word = mExportWords.at(mExportIndex++);
        mExportData["words"].push_back(word.toStdString());
        const bool option                                        = false;
        using Lookup                                             = QString (*)(void*, const QString&, const bool&);
        const std::pair<const char*, const char*> dictionaries[] = {
            {"Senior",                "_ZN16YDictQueryEngine11querySeniorERK7QStringRKb"               },
            {"PureEnglishAndExample", "_ZN16YDictQueryEngine26queryPureEnglishAndExampleERK7QStringRKb"}
        };
        auto* engine = YPointer<YDictQueryEngine>::getInstance();
        for (const auto& [name, symbol] : dictionaries) {
            auto lookup = reinterpret_cast<Lookup>(PEN_SYM(symbol));
            if (!engine || !lookup) continue;
            const QString raw    = lookup(engine, word, option);
            auto          parsed = json::parse(raw.toStdString(), nullptr, false);
            if (!parsed.is_discarded()) mExportData["data"][name][word.toStdString()] = std::move(parsed);
        }
        mExportProgress = mExportIndex * 100 / mExportWords.size();
        emit exportProgressChanged();
        // Yield between words; native dictionary readers stay on the UI thread.
        QTimer::singleShot(0, this, &WordBookTweaks::exportNextWord);
        return;
    }
    QDir().mkpath(QStringLiteral("/userdisk/Favorite"));
    QSaveFile  file(QStringLiteral(WB_EXPORT_FILE));
    const auto payload = mExportData.dump();
    const bool saved =
        file.open(QIODevice::WriteOnly)
        && file.write(payload.data(), static_cast<qint64>(payload.size())) == static_cast<qint64>(payload.size())
        && file.commit();
    mExportWords.clear();
    mExportData     = json();
    mExportProgress = saved ? 100 : 0;
    emit exportProgressChanged();
    setExporting(false);
    showToast(saved ? "单词本已导出到 Favorite/WordBook.json" : "保存单词本失败", saved ? "#64D99A" : "#E9900C");
#endif
}

#if PL_BUILD_YDP02X
bool WordBookTweaks::exportFromQueryResult(uint64 queryResult) {
    QFile file(WB_EXPORT_FILE);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        showToast("单词本导出失败，文件打开失败", "#E9900C");
        return false;
    }
    json j;
    j["version"]    = 100;
    int index       = -1;
    mExportProgress = 0;
    auto count      = PEN_CALL(int, "_ZNK8Database16AsyncQueryResult5countEv", uint64)(queryResult);
    while (count > index) {
        index++;
        mExportProgress += (index / (double)count) * 100.0;
        emit exportProgressChanged();
        auto qv = PEN_CALL(QVariant, "_ZNK8Database16AsyncQueryResult5valueEiRK7QString", uint64, int, QString)(
            queryResult,
            index,
            "word"
        );
        if (!qv.canConvert<QString>()) {
            continue;
        }
        auto word       = qv.toString();
        auto HasFeature = PEN_CALL(bool, "_ZN2FTL10HasFeatureE14DictPenFeature", DictPenFeature);
        if (HasFeature(DICT_SENIOR)) {
            QString querySenior =
                PEN_CALL(QString, "_ZN16YDictQueryEngine11querySeniorERK7QString", void*, QString, QString)(
                    YPointer<YDictQueryEngine>::getInstance(),
                    word,
                    QString()
                ); // 高中生词典
            try {
                if (!querySenior.isEmpty()) {
                    j["data"]["Senior"][word.toStdString()] = json::parse(querySenior.toStdString());
                }
            } catch (...) {}
        }
        if (HasFeature(DICT_SIMPLE)) {
            QString queryPureEnglishAndExample =
                PEN_CALL(QString, "_ZN16YDictQueryEngine26queryPureEnglishAndExampleERK7QString", void*, QString, QString)(
                    YPointer<YDictQueryEngine>::getInstance(),
                    word,
                    QString()
                ); // 有道简明释义
            try {
                if (!queryPureEnglishAndExample.isEmpty()) {
                    j["data"]["PureEnglishAndExample"][word.toStdString()] =
                        json::parse(queryPureEnglishAndExample.toStdString());
                }
            } catch (...) {}
        }
        // if (HasFeature(OXFORD)) {
        //     QString queryOxford = PEN_CALL(QString,"_ZN16YDictQueryEngine11queryOxfordERK7QString",
        //                     uint64,QString,QString)(engine,word,QString()); // 牛津词典
        //     try {
        //         if (!queryOxford.isEmpty()) {
        //             j["data"]["Oxford"][word.toStdString()] = json::parse(queryOxford.toStdString());
        //         }
        //     } catch (...) {}
        // }
    }
    file.write(QString::fromStdString(j.dump()).toUtf8());
    file.close();
    showToast("单词本导出成功");
    filemanager::FileManager::getInstance().setMtpOnoff(true);
    return true;
}
#endif

#if PL_BUILD_YDP03X
PEN_HOOK(void, _ZN16YWordBookManager14onExecFinishedERKN8Database16AsyncQueryResultE, void* self, void* result) {
    if (!mod::WordBookTweaks::getInstance().consumeExportResult(result)) origin(self, result);
}

PEN_HOOK(
    QString,
    _ZNK18YWordbookDBPrivate15getLoadItemsCmdExN12YEnumWrapper13WordGroupTypeEiiNS0_12LanguageTypeENS0_9ItemStateENS0_9SyncStateE,
    void*   self,
    int64_t lastId,
    int     group,
    int     page,
    int     limit,
    int     language,
    int     itemState,
    int     syncState
) {
    const bool split = mod::WordBookTweaks::getInstance().getPhraseTab() && (group == 2 || group == 3);
    auto       query = origin(self, lastId, split ? 1 : group, page, limit, language, itemState, syncState);
    if (split)
        query.replace("ORDER BY", group == 2 ? "AND word NOT LIKE '% %' ORDER BY" : "AND word LIKE '% %' ORDER BY");
    return query;
}
#endif

bool WordBookTweaks::getExporting() const { return mExporting; }

bool WordBookTweaks::getPhraseTab() const { return mPhraseTab; }

bool WordBookTweaks::getNoCaseSensitive() const { return mNoCaseSensitive; }

void WordBookTweaks::setExporting(bool val) {
    if (mExporting != val) {
        mExporting = val;
        emit exportingChanged();
    }
}

void WordBookTweaks::setPhraseTab(bool val) {
    if (mPhraseTab != val) {
        mPhraseTab         = val;
        mCfg["phrase_tab"] = val;
        WRITE_CFG;
        emit phraseTabChanged();
    }
}

void WordBookTweaks::setNoCaseSensitive(bool val) {
    if (mNoCaseSensitive != val) {
        mNoCaseSensitive         = val;
        mCfg["nocase_sensitive"] = val;
        WRITE_CFG;
        emit noCaseSensitiveChanged();
    }
}

} // namespace mod

// Bugfix: Missing "wbLanguageFitler" cfg item.
PEN_HOOK(uint64, _ZN16YWordBookManager8wipeDataEb, uint64 self, bool a2) {
    a2 = false;
    return origin(self, a2);
}

// Phrase Tab

// YDP02X: getLoadItemsCmd does not exist on YDP03X and the doExport hook writes
// YDP02X offsets; both are guarded.
#if PL_BUILD_YDP02X
PEN_HOOK(
    QString,
    _ZNK18YWordbookDBPrivate15getLoadItemsCmdExN12YEnumWrapper13WordGroupTypeEiNS0_12LanguageTypeENS0_9ItemStateENS0_9SyncStateE,
    uint64 a1,
    uint64 a2,
    int    tabType,
    uint32 limit,
    uint32 a5,
    uint32 a6,
    uint32 a7,
    uint64 rtn
) {
    auto cmd = origin(a1, a2, tabType, limit, a5, a6, a7, rtn);
    // tabType
    // 0 = (def) sentence
    // 1 = (def) word
    // 2 = (mod) word
    // 3 = (mod) phrase
    if (mod::WordBookTweaks::getInstance().getPhraseTab()) {
        if (tabType == 2) {
            cmd.replace("ORDER BY", "AND word NOT LIKE '% %' ORDER BY");
        } else if (tabType == 3) {
            cmd.replace("ORDER BY", "AND word LIKE '% %' ORDER BY");
        }
    }
    return cmd;
}

// Mod Exporter

PEN_HOOK(void, _ZN16YWordBookManager8doExportERKN8Database16AsyncQueryResultE, uint64 self, uint64 result) {
    mod::WordBookTweaks::getInstance().setExporting(true);
    if (!pickUpQuery) {
        QFile(WB_EXPORT_FILE).remove();
        origin(self, result);
        mod::WordBookTweaks::getInstance().setExporting(false);
    } else {
        pickUpQuery  = false;
        auto isValid = PEN_CALL(bool, "_ZNK8Database16AsyncQueryResult7isValidEv", uint64)(result);
        if (isValid) {
            mod::WordBookTweaks::getInstance().exportFromQueryResult(result);
        } else {
            mod::showToast("导出失败，无效的查询", "#E9900C");
        }
        *(byte*)(*((uint64*)self + 4) + 128) = 0;
        *(byte*)(*((uint64*)self + 4) + 136) = 0;
        mod::WordBookTweaks::getInstance().setExporting(false);
    }
}
#endif
