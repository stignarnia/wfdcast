// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#pragma once

#include "audiorouter.h"
#include "pipeline.h"
#include "portal.h"
#include "wfdserver.h"

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>

class Settings;

// Runs one casting session: KDE picker -> privileged helper joins the TV's
// Wi-Fi Direct group -> the TV connects to our RTSP server -> stream.
class CastController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool error READ error NOTIFY statusChanged)
    Q_PROPERTY(QVariantList peers READ peers NOTIFY peersChanged)
    Q_PROPERTY(QString log READ log NOTIFY logChanged)
    Q_PROPERTY(QString encoderInUse READ encoderInUse NOTIFY stateChanged)
    Q_PROPERTY(QStringList availableEncoders READ availableEncoders CONSTANT)

public:
    explicit CastController(Settings *settings, QObject *parent = nullptr);
    ~CastController() override;

    // idle, picking, authenticating, scanning, choosing, joining, accept, waiting, streaming, stopping
    QString state() const { return m_state; }
    QString status() const { return m_status; }
    bool error() const { return m_error; }
    QVariantList peers() const { return m_peers; }
    QString log() const { return m_log.join(u'\n'); }
    QString encoderInUse() const { return m_pipeline.running() ? m_pipeline.encoderInUse() : QString(); }
    QStringList availableEncoders() const { return Pipeline::availableEncoders(); }

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void choosePeer(const QString &mac, const QString &name);
    Q_INVOKABLE void forgetTv();
    Q_INVOKABLE void clearLog();

Q_SIGNALS:
    void stateChanged();
    void statusChanged();
    void peersChanged();
    void logChanged();

private:
    void setState(const QString &s);
    void setStatus(const QString &s, bool error = false);
    void appendLog(const QString &line);
    void fail(const QString &message);
    void finishStop();

    void onPortalReady(int fd, quint32 node, QSize size, uint sourceType);
    static QString windowAppId(quint32 node); // app id of a shared window's KWin stream
    void startHelper();
    void sendHelper(const QJsonObject &cmd);
    void onHelperOutput();
    void onHelperEvent(const QJsonObject &e);
    void onHelperFinished(int code, QProcess::ExitStatus status);
    void connectTo(const QString &mac);
    void sendConnect(const QString &mac, const QString &psk);
    // The TV's Wi-Fi Direct passphrase lives in KWallet (via QtKeychain), keyed by the TV address.
    void storeSecret(const QString &mac, const QString &psk);
    void deleteSecret(const QString &mac);
    void onPlay(const QString &ip, int port, int w, int h, int fps);
    void onSessionEnded(const QString &reason);
    void onSettingChanged(const QString &key, const QVariant &value);
    void updateCastingStatus();

    Settings *m_settings;
    Portal m_portal;
    WfdServer m_server;
    Pipeline m_pipeline;
    AudioRouter m_audio;
    QProcess m_helper;
    QByteArray m_helperBuffer;
    QTimer m_tvWait;
    int m_nudges = 0;

    QString m_state = QStringLiteral("idle");
    QString m_status;
    bool m_error = false;
    QVariantList m_peers;
    QStringList m_log;

    int m_pwFd = -1;
    quint32 m_pwNode = 0;
    QSize m_sourceSize;
    bool m_windowSource = false;
    int m_castW = 0, m_castH = 0, m_castFps = 0;
    QString m_audioDevice;
    QString m_targetMac;
    QString m_targetName;
};
