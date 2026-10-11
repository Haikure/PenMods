// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "Updater.h"
#include "mod/Config.h"

#include "base/YPointer.h"

#include "common/Downloader.h"
#include "common/Event.h"
#include "common/Utils.h"
#include "common/util/System.h"

#include <QCryptographicHash>
#include <QDir>
#include <QProcess>
#include <QQmlContext>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QUrl>

#define UH_TEMP_PATH    "/tmp/modupdate/"
#define UH_TEMP_DIRNAME "modupdate"

#if PL_BUILD_YDP02X
#define UH_MAIN_URL "https://cdn.jsdelivr.net/gh/lyrecoul/penmods-ota@main/updates.json"
#endif

namespace mod {

namespace {
[[maybe_unused]] bool isThirdGenerationLibrary(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray header = file.read(20);
    if (header.size() != 20
        || header.left(4)
               != QByteArray(
                   "\x7f"
                   "ELF",
                   4
               )
        || header[4] != 2 || header[5] != 1 || header[16] != 3 || header[17] != 0
        || static_cast<unsigned char>(header[18]) != 183 || header[19] != 0)
        return false;
    const QByteArray marker("PenMods.Platform=YDP03X");
    QByteArray       tail;
    while (!file.atEnd()) {
        tail += file.read(65536);
        if (tail.contains(marker)) return true;
        tail = tail.right(marker.size() - 1);
    }
    return false;
}
} // namespace

Updater::Updater() : Logger("Updater") {
    mSource = QString::fromStdString(Config::getInstance().read("updater").value("ydp03x_source", std::string()));
    connect(&Event::getInstance(), &Event::uiCompleted, this, &Updater::onUiCompleted);
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("updater", this);
    });
}

QString Version::toString() const {
    return QString("%1.%2.%3").arg(QString::number(mMajor), QString::number(mMinor), QString::number(mRevision));
}

uint32 Version::toNumber() const { return mMajor * 100 + mMinor * 10 + mRevision; }

bool Version::operator==(const Version b) const {
    return mMajor == b.mMajor && mMinor == b.mMinor && mRevision == b.mRevision;
}

bool Version::operator>(const Version b) const {
    return std::tie(mMajor, mMinor, mRevision) > std::tie(b.mMajor, b.mMinor, b.mRevision);
}

bool Version::operator<(const Version b) const { return b > *this; }

QString Updater::source() const {
#if PL_BUILD_YDP02X
    return QStringLiteral(UH_MAIN_URL);
#else
    return mSource;
#endif
}

void Updater::setSource(const QString& value) {
    if (busy()) return;
    const auto address = value.trimmed();
    const QUrl url(address);
    if (!address.isEmpty()
        && (!url.isValid() || url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http"))) {
        showToast("请输入有效的更新清单网址", "#E9900C");
        return;
    }
    mSource                 = address;
    auto config             = Config::getInstance().read("updater");
    config["ydp03x_source"] = address.toStdString();
    Config::getInstance().write("updater", std::move(config));
    mLatestObject    = json();
    mInfo.mOtaStatus = LATEST_VERSION;
    mInfo.mLatestStr.clear();
    mInfo.mUpdateNote.clear();
    emit stateChanged();
}

bool Updater::busy() const {
    return mInfo.mOtaStatus == CHECKING || mInfo.mOtaStatus == DOWNLOADING || mInfo.mOtaStatus == MD5_CHECKING
        || mInfo.mOtaStatus == INSTALLING;
}

QString Updater::statusText() const {
    if (source().isEmpty()) return QStringLiteral("未设置三代更新源");
    switch (mInfo.mOtaStatus) {
    case CHECKING:
        return QStringLiteral("正在检查更新");
    case HAS_NEW_VERSION:
        return QStringLiteral("有可用更新");
    case DOWNLOADING:
        return QStringLiteral("正在下载");
    case DOWNLOAD_FINISHED:
        return QStringLiteral("下载完成，可以安装");
    case MD5_CHECKING:
        return QStringLiteral("正在校验更新包");
    case INSTALLING:
        return QStringLiteral("正在安装");
    case INSTALL_SUCCESSFULLY:
        return QStringLiteral("安装完成，请重新启动应用");
    case LATEST_VERSION:
        return mInfo.mLatestStr.isEmpty() ? QStringLiteral("尚未检查") : QStringLiteral("已是最新版本");
    case ERROR_LOW_BATTERY:
        return QStringLiteral("电量不足");
    case ERROR_NO_ENOUGH_MEMORY:
        return QStringLiteral("可用空间不足");
    case ERROR_MD5_CHECK:
        return QStringLiteral("更新包校验失败");
    case DOWNLOAD_FAILED:
        return QStringLiteral("下载失败");
    case INSTALL_FAILED:
        return QStringLiteral("安装失败");
    default:
        return QStringLiteral("更新检查失败，请检查网络或更新清单");
    }
}

void Updater::check() {
    if (busy()) return;
    if (source().isEmpty()) {
        showToast("请先设置三代独立更新清单地址", "#E9900C");
        return;
    }
    info("Starting to check update...");
    QFile     battery("/sys/class/power_supply/battery/capacity");
    bool      knownPower = false;
    const int power =
        battery.open(QIODevice::ReadOnly) ? QString::fromUtf8(battery.readAll()).trimmed().toInt(&knownPower) : 0;
    if (knownPower && power < 10) {
        error("Checking for updates stalled: Low battery.");
        _setOtaStatus(ERROR_LOW_BATTERY);
        return;
    }
    _setOtaStatus(CHECKING);
    _cleanupTemp();
    Downloader::getInstance().createTask(
        source(),
        UH_TEMP_PATH "version_list.temp",
        nullptr,
        [&](Downloader::TaskId, const QString& savedPath, Downloader::ResultStatus stat) {
            switch (stat) {
            case Downloader::ResultStatus::ERROR_FAIL_TO_START:
                error("Checking for updates stalled: Fail to start.");
            case Downloader::ResultStatus::ERROR_STOPPED:
                error("Checking for updates stalled: Stopped by user.");
            case Downloader::ResultStatus::ERROR_UNKNOWN:
                error("Checking for updates stalled: Unknwon error.");
                _setOtaStatus(ERROR_NO_CONNECTION);
                return;
            case Downloader::ResultStatus::ERROR_FAIL_OPEN_FILE:
                error("Checking for updates stalled: Fail to open file.");
            case Downloader::ResultStatus::ERROR_FAIL_STORAGE_INVALID:
                error("Checking for updates stalled: No enough space(1).");
                _setOtaStatus(ERROR_NO_ENOUGH_MEMORY);
                return;
            case Downloader::ResultStatus::SUCCEED:
                break;
            }
            QFile file(savedPath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                error("Checking for updates stalled: No network connection");
                _setOtaStatus(ERROR_NO_CONNECTION);
                return;
            }
            if (file.size() > 1024 * 1024) {
                _setOtaStatus(ERROR_NO_CONNECTION);
                return;
            }
            auto result = file.readAll().toStdString();
            try {
                auto j = json::parse(result);
                if (j["status"] != 200) {
                    throw std::runtime_error("status != 200");
                }
                if (!j["versions"].is_array()) throw std::runtime_error("versions must be an array");
                json versions = json::array();
                for (const auto& entry : j["versions"]) {
#if PL_BUILD_YDP03X
                    if (entry.value("platform", j.value("platform", std::string())) != "YDP03X") continue;
#endif
                    if (!entry.contains("version") || !entry["version"].is_array() || entry["version"].size() != 3)
                        throw std::runtime_error("invalid version");
                    for (const auto& number : entry["version"])
                        if (!number.is_number_unsigned()) throw std::runtime_error("invalid version number");
                    versions.push_back(entry);
                }
                if (versions.empty()) throw std::runtime_error("no matching platform releases");
                // matchSelfVersion -> needGoNextVersion? -> latestDirectly / nextVersion
                bool selfVersionDetected = false;
                auto topVer              = mSelfVersion;
                for (auto i : versions) {
                    auto that = Version{i["version"][0], i["version"][1], i["version"][2]};
                    if (that > topVer) {
                        topVer = that;
                    } else if (that == mSelfVersion) {
                        selfVersionDetected = true;
                        if (i.find("next") != i.end()) {
                            topVer = Version{i["next"][0], i["next"][1], i["next"][2]};
                            break;
                        }
                    }
                }
                if (!selfVersionDetected) {
                    throw std::runtime_error("Cannot get self version from mod_versions.");
                }
                for (auto i : versions) {
                    auto that = Version{i["version"][0], i["version"][1], i["version"][2]};
                    if (that == topVer) {
                        mLatestObject = i;
                        break;
                    }
                }
                _setLatestVersion(topVer.toString());
                if (topVer > mSelfVersion) {
                    const QString md5 = QString::fromStdString(mLatestObject.at("md5").get<std::string>());
                    if (!QRegularExpression(QStringLiteral("^[0-9a-fA-F]{32}$")).match(md5).hasMatch())
                        throw std::runtime_error("invalid package checksum");
                    const QUrl download(QString::fromStdString(mLatestObject.at("download").get<std::string>()));
                    if (!download.isValid() || download.host().isEmpty()
                        || (download.scheme() != "https" && download.scheme() != "http"))
                        throw std::runtime_error("invalid download URL");
                    auto   avail = QStorageInfo(UH_TEMP_PATH).bytesAvailable();
                    qint64 bytes = mLatestObject["size"];
                    if (bytes <= 0) throw std::runtime_error("invalid package size");
                    if (avail == -1 || avail < bytes) {
                        error("Checking for updates stalled: No enough space(2).");
                        _setOtaStatus(ERROR_NO_ENOUGH_MEMORY);
                        return;
                    }
                    _setUpdateNote(QString::fromStdString(mLatestObject["note"]));
                    _setUpdatePackSize((double)bytes / 1024.0 / 1024.0);
                    info("New version({}) founded!", topVer.toString().toStdString());
                    _setOtaStatus(HAS_NEW_VERSION);
                } else {
                    info("Already the latest version.");
                    _setOtaStatus(LATEST_VERSION);
                }
            } catch (const std::exception& e) {
                error("An error occurred while parsing the version information.");
                error(e.what());
                _setOtaStatus(ERROR_NO_CONNECTION);
            }
        }
    );
}

Updater::UpdateInfo& Updater::getInfo() { return mInfo; }

void Updater::download() {
    if (!canDownload()) return;
    _setOtaStatus(DOWNLOADING);
    Downloader::getInstance().createTask(
        QString::fromStdString(mLatestObject["download"]),
        UH_TEMP_PATH "download.temp",
        [&](Downloader::TaskId, int prog) { _setDownloadProgress(prog); },
        [&](Downloader::TaskId, const QString& savedPath, Downloader::ResultStatus stat) {
            if (stat == Downloader::ResultStatus::SUCCEED) {
                _setOtaStatus(DOWNLOAD_FINISHED);
            } else {
                _setOtaStatus(DOWNLOAD_FAILED);
            }
        }
    );
}

void Updater::install() {
    if (!canInstall()) return;
    _setOtaStatus(MD5_CHECKING);
    if (_calcFileMd5(UH_TEMP_PATH "download.temp")
        != QString::fromStdString(mLatestObject["md5"]).toLower().toStdString()) {
        _setOtaStatus(ERROR_MD5_CHECK);
    } else {
        bool valid = _unzip(UH_TEMP_PATH "download.temp", UH_TEMP_PATH) && QFile(UH_TEMP_PATH "_do_update.sh").exists();
#if PL_BUILD_YDP03X
        valid = valid && isThirdGenerationLibrary(UH_TEMP_PATH "libPenMods.so")
             && isThirdGenerationLibrary(UH_TEMP_PATH "libPenModsResources.so");
#endif
        if (!valid) {
            _setOtaStatus(ERROR_MD5_CHECK);
        } else {
            _setOtaStatus(MD5_CHECK_SUCCESSFULLY);
            _setOtaStatus(INSTALLING);
            exec(QString("chmod +x \"%1\"").arg(UH_TEMP_PATH "_do_update.sh"));
            exec(QString("cd \"%1\" && bash _do_update.sh").arg(UH_TEMP_PATH));
            _setInstallProgress(100);
            if (QFile(UH_TEMP_PATH "INSTALL_SUCCESSFULLY").exists()) {
                _setOtaStatus(INSTALL_SUCCESSFULLY);
            } else {
                _setOtaStatus(INSTALL_FAILED);
            }
        }
    }
}

void Updater::onUiCompleted() {
    _cleanupTemp();

    _setCurrentVersion(mSelfVersion.toString());
}

std::string Updater::_calcFileMd5(const QString& path) {
    QFile              file(path);
    QCryptographicHash hash(QCryptographicHash::Md5);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return hash.result().toHex().toStdString();
}

bool Updater::_unzip(QString zipPath, QString toWhere) {
    QFile::remove(toWhere + "_do_update.sh");
    QFile::remove(toWhere + "INSTALL_SUCCESSFULLY");
    QStringList arguments{"-q", "-o", zipPath};
#if PL_BUILD_YDP03X
    arguments << "_do_update.sh" << "libPenMods.so" << "libPenModsResources.so";
#endif
    arguments << "-d" << toWhere;
    return QProcess::execute("unzip", arguments) == 0;
}

void Updater::_cleanupTemp() {
    QDir(UH_TEMP_PATH).removeRecursively();
    QDir(UH_TEMP_PATH).mkpath(".");
}

void Updater::_setCurrentVersion(QString verstr) {
    mInfo.mCurrentStr = std::move(verstr);
    emit stateChanged();
    PEN_CALL(void*, "_ZN14YUpdateManager23onCurrentVersionChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setLatestVersion(QString latest) {
    mInfo.mLatestStr = std::move(latest);
    emit stateChanged();
    PEN_CALL(void*, "_ZN14YUpdateManager22onTargetVersionChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setUpdateNote(QString note) {
    mInfo.mUpdateNote = std::move(note);
    emit stateChanged();
    PEN_CALL(void*, "_ZN14YUpdateManager19onUpdateNoteChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setUpdatePackSize(double size) {
    mInfo.mPackageSize = size;
#if PL_BUILD_YDP02X
    *(double*)(*((uint64*)YPointer<YUpdateManager>::getInstance() + 2) + 64LL) = dec(size, 1);
#endif
    emit stateChanged();
    PEN_CALL(void*, "_ZN14YUpdateManager20updateImgSizeChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setOtaStatus(UpdateStatus stat) {
    mInfo.mOtaStatus = stat;
    emit stateChanged();
    PEN_CALL(void*, "_ZN14YUpdateManager18onOtaStatusChangedEv", void*)(YPointer<YUpdateManager>::getInstance());
}

void Updater::_setDownloadProgress(int prog) {
    mInfo.mDownloadProgress = prog;
    emit stateChanged();
}

void Updater::_setInstallProgress(int prog) {
    mInfo.mInstallProgress = prog;
    emit stateChanged();
}

} // namespace mod

PEN_HOOK(QString, _ZN14YUpdateManager14currentVersionEv, uint64 self, void* a2) {
    return mod::Updater::getInstance().getInfo().mCurrentStr;
}

PEN_HOOK(QString, _ZN14YUpdateManager13targetVersionEv, uint64 self, void* a2) {
    return mod::Updater::getInstance().getInfo().mLatestStr;
}

PEN_HOOK(UpdateStatus, _ZN14YUpdateManager9otaStatusEv, uint64) {
    return mod::Updater::getInstance().getInfo().mOtaStatus;
}

PEN_HOOK(QString, _ZN14YUpdateManager10updateNoteEv, uint64 self, void* a2, void* a3) {
    return mod::Updater::getInstance().getInfo().mUpdateNote;
}

#if PL_BUILD_YDP03X
// YDP03X-only getters (the setting page reads these to show download/install state).
PEN_HOOK(double, _ZN14YUpdateManager13updateImgSizeEv, void* self) {
    return mod::Updater::getInstance().getInfo().mPackageSize;
}

PEN_HOOK(uint32, _ZN14YUpdateManager16downloadProgressEv, void* self) {
    return mod::Updater::getInstance().getInfo().mDownloadProgress;
}

PEN_HOOK(uint32, _ZN14YUpdateManager15installProgressEv, void* self) {
    return mod::Updater::getInstance().getInfo().mInstallProgress;
}
#endif

// Disabled Functions;

PEN_HOOK(void, _ZN14YUpdateManager7installEv, uint64 self) {}

PEN_HOOK(void, _ZN14YUpdateManager13downloadStartEv, uint64 self) {}

PEN_HOOK(void, _ZN14YUpdateManager13downloadPauseEv, uint64 self) {}

PEN_HOOK(void, _ZN14YUpdateManager14downloadResumeEv, uint64 self) {}

PEN_HOOK(bool, _ZN21YUpdateManagerPrivate25checkOtaOperateThreadFuncEv, uint64 self) { return false; }

PEN_HOOK(bool, _ZN21YUpdateManagerPrivate32checkInstallOtaOperateThreadFuncEv, uint64 self) { return false; }

PEN_HOOK(uint64, _ZN21YUpdateManagerPrivate15checkOtaInstallEv, uint64 self) { return 0; }

PEN_HOOK(void, _ZN21YUpdateManagerPrivate14checkOtaUpdateEv, uint64 self) {}

PEN_HOOK(void, _ZN21YUpdateManagerPrivate19onPowerStatusChangeEiRK7QString, void* self, void* a2, void* a3) {}
