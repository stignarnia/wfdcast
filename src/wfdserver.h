// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

// Wi-Fi Display source side of RTSP. The TV (sink) connects to us; we send
// M1 OPTIONS, M3 GET_PARAMETER, M4 SET_PARAMETER (chosen format), M5 trigger
// SETUP, answer its OPTIONS/SETUP/PLAY, then keep the session alive.
// We never ask for wfd_content_protection, so no HDCP is attempted.
class WfdServer : public QObject
{
    Q_OBJECT
public:
    struct Wanted {
        int width = 1920, height = 1080, fps = 30;
        int keepaliveSeconds = 20;
        int localRtpPort = 5000;
    };

    explicit WfdServer(QObject *parent = nullptr);
    bool listen(quint16 port, QString *error);
    void close();
    void setWanted(const Wanted &w);
    void teardown(); // ask the TV to end the session
    bool hasSession() const;

Q_SIGNALS:
    // The TV said PLAY: stream to sinkIp:sinkPort with the negotiated mode.
    void play(const QString &sinkIp, int sinkPort, int width, int height, int fps);
    void sessionEnded(const QString &reason);
    void log(const QString &line);

private:
    struct Message {
        QString startLine;
        QHash<QString, QString> headers; // lower-case names
        QString body;
        bool isResponse() const { return startLine.startsWith(QLatin1String("RTSP/1.0")); }
        QString header(const QString &name) const { return headers.value(name.toLower()); }
    };

    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void handle(const Message &m);
    void handleRequest(const Message &m);
    void handleResponse(const Message &m);
    void sendRequest(const QString &method, const QString &uri, const QString &purpose,
                     const QString &body = {}, const QString &extraHeaders = {});
    void sendReply(const QString &cseq, const QString &extraHeaders = {});
    void send(const QString &text);
    void endSession(const QString &reason);
    bool chooseFormat(const QString &sinkParams);

    QTcpServer m_server;
    QPointer<QTcpSocket> m_socket;
    QByteArray m_buffer;
    int m_cseq = 0;
    QHash<int, QString> m_pending; // our CSeq -> purpose
    Wanted m_wanted;
    QString m_session;
    int m_sinkRtpPort = 0;
    int m_width = 0, m_height = 0, m_fps = 0;
    QString m_videoFormats;
    QTimer m_keepalive;
    bool m_tearingDown = false;
};
