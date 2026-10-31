// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "common/service/Logger.h"
#include "common/service/Singleton.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <memory>

class QNetworkReply;
class QProcess;
class QTemporaryDir;
class QTimer;

namespace mod {

// Downloads are independent of activation: only the external launcher switches generations.
class UpdateCenter : public QObject, public Singleton<UpdateCenter>, private Logger {
    Q_OBJECT
    Q_PROPERTY(QVariantList components READ components NOTIFY componentsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool pendingRestart READ pendingRestart NOTIFY stateChanged)
    Q_PROPERTY(bool launcherReady READ launcherReady NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool autoCheck READ autoCheck WRITE setAutoCheck NOTIFY settingsChanged)
    Q_PROPERTY(bool autoDownload READ autoDownload WRITE setAutoDownload NOTIFY settingsChanged)

public:
    QVariantList components() const;
    bool         busy() const { return !mActiveId.isEmpty() || !mQueue.isEmpty(); }
    bool         pendingRestart() const { return mPendingRestart; }
    bool         launcherReady() const { return mLauncherReady; }
    QString      statusText() const { return mStatusText; }
    bool         autoCheck() const;
    bool         autoDownload() const;
    void         setAutoCheck(bool enabled);
    void         setAutoDownload(bool enabled);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void downloadComponent(QString id);
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void cancelPending();
    Q_INVOKABLE void restartNow();
    Q_INVOKABLE void setComponentAutoUpdate(QString id, bool enabled);

signals:
    void componentsChanged();
    void stateChanged();
    void settingsChanged();

private:
    friend Singleton<UpdateCenter>;
    UpdateCenter();
    struct Component {
        QString     id, name, installedVersion, pendingVersion, state, error;
        QJsonObject descriptor;
        int         progress  = 0;
        bool        installed = false;
    };
    struct Download {
        QString     id;
        QJsonObject descriptor;
    };
    QList<Component>               mComponents;
    QList<Download>                mQueue;
    QString                        mActiveId, mStatusText;
    QJsonObject                    mActiveDescriptor;
    QStringList                    mSources;
    QByteArray                     mCatalog;
    QSet<QString>                  mAutomaticAttempts;
    QDateTime                      mNextAutoCheck;
    QString                        mManifestSource;
    QPointer<QTimer>               mHeartbeatTimer;
    int                            mAutoCheckFailures = 0;
    bool                           mObservedCheck     = false;
    bool                           mTrialActive       = false;
    QPointer<QNetworkReply>        mReply;
    QPointer<QProcess>             mStageProcess, mStatusProcess;
    std::shared_ptr<QTemporaryDir> mDownloadDir;
    bool                           mLauncherReady = false, mPendingRestart = false, mCancellingPending = false;
    bool                           mAutoScheduled = false, mUiReady = false;
    quint64                        mRequestSequence = 0, mHealthSequence = 0;

    Component* component(const QString& id);
    QString    rootPath() const;
    void       ingestManifest(const QByteArray& manifest);
    void       updateCatalog();
    void       scheduleAutomaticDownloads();
    void       runAutomaticCheck();
    void       observeManifestCheck();
    void       startNextDownload();
    void       requestNextSource();
    void       stageDownloadedArchive();
    void       finishDownload(const QString& error = {});
    void       notify();
    void       writeHeartbeat();
    bool       componentAutoUpdate(const QString& id) const;
    QString    dependencyProblem(const Component& candidate) const;
};

} // namespace mod
