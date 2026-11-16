// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#include "wfdserver.h"

#include <QRandomGenerator>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace
{
// CEA modes we offer: bit in wfd_video_formats' CEA bitmap, H.264 level code.
struct CeaMode {
    int width, height, fps, bit, level;
};
constexpr CeaMode kModes[] = {
    {1920, 1080, 60, 8, 0x10}, // level 4.2
    {1920, 1080, 30, 7, 0x08}, // level 4.1
    {1280, 720, 60, 6, 0x02}, // level 3.2
    {1280, 720, 30, 5, 0x01}, // level 3.1
    {640, 480, 60, 0, 0x01}, // mandatory fallback
};

const QString kUri = u"rtsp://localhost/wfd1.0"_s;

QString paramValue(const QString &body, const QString &name)
{
    const QRegularExpression re(u"(?im)^%1:\\s*(.+)$"_s.arg(QRegularExpression::escape(name)));
    const auto m = re.match(body);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}
}

WfdServer::WfdServer(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &WfdServer::onNewConnection);
    connect(&m_keepalive, &QTimer::timeout, this, [this] {
        sendRequest(u"GET_PARAMETER"_s, kUri, u"keepalive"_s, {}, u"Session: %1\r\n"_s.arg(m_session));
    });
}

bool WfdServer::listen(quint16 port, QString *error)
{
    if (m_server.isListening()) {
        return true;
    }
    if (!m_server.listen(QHostAddress::AnyIPv4, port)) {
        *error = u"Cannot listen on TCP port %1: %2"_s.arg(port).arg(m_server.errorString());
        return false;
    }
    Q_EMIT log(u"RTSP: listening on port %1"_s.arg(port));
    return true;
}

void WfdServer::close()
{
    endSession(QString());
    m_server.close();
}

void WfdServer::setWanted(const Wanted &w)
{
    m_wanted = w;
}

bool WfdServer::hasSession() const
{
    return m_socket;
}

void WfdServer::onNewConnection()
{
    while (QTcpSocket *s = m_server.nextPendingConnection()) {
        if (m_socket) {
            Q_EMIT log(u"RTSP: second connection from %1 refused"_s.arg(s->peerAddress().toString()));
            s->abort();
            s->deleteLater();
            continue;
        }
        m_socket = s;
        m_buffer.clear();
        m_pending.clear();
        m_cseq = 0;
        m_tearingDown = false;
        m_session = QString::number(QRandomGenerator::global()->generate(), 16).toUpper();
        connect(s, &QTcpSocket::readyRead, this, &WfdServer::onReadyRead);
        connect(s, &QTcpSocket::disconnected, this, &WfdServer::onDisconnected);
        Q_EMIT log(u"RTSP: TV connected from %1"_s.arg(s->peerAddress().toString()));
        sendRequest(u"OPTIONS"_s, u"*"_s, u"M1"_s, {}, u"Require: org.wfa.wfd1.0\r\n"_s); // M1
    }
}

void WfdServer::onReadyRead()
{
    if (!m_socket) {
        return;
    }
    m_buffer += m_socket->readAll();
    while (true) {
        const int end = m_buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            return;
        }
        const QString head = QString::fromUtf8(m_buffer.left(end));
        static const QRegularExpression lenRe(u"(?im)^content-length:\\s*(\\d+)"_s);
        const auto lm = lenRe.match(head);
        const int len = lm.hasMatch() ? lm.captured(1).toInt() : 0;
        if (m_buffer.size() < end + 4 + len) {
            return;
        }
        Message m;
        const QStringList lines = head.split(u"\r\n"_s);
        m.startLine = lines.value(0);
        for (int i = 1; i < lines.size(); ++i) {
            const int colon = lines[i].indexOf(u':');
            if (colon > 0) {
                m.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
            }
        }
        m.body = QString::fromUtf8(m_buffer.mid(end + 4, len));
        m_buffer.remove(0, end + 4 + len);
        Q_EMIT log(u"RTSP ← %1%2"_s.arg(m.startLine, m.body.isEmpty() ? QString() : u" | "_s + m.body.simplified()));
        handle(m);
        if (!m_socket) {
            return;
        }
    }
}

void WfdServer::handle(const Message &m)
{
    if (m.isResponse()) {
        handleResponse(m);
    } else {
        handleRequest(m);
    }
}

void WfdServer::handleResponse(const Message &m)
{
    const QString purpose = m_pending.take(m.header(u"cseq"_s).toInt());
    if (!m.startLine.contains(u" 200 "_s)) {
        endSession(u"The TV refused %1: %2"_s.arg(purpose, m.startLine));
        return;
    }
    if (purpose == u"M1"_s) { // M3: ask what the TV supports
        sendRequest(u"GET_PARAMETER"_s, kUri, u"M3"_s, u"wfd_video_formats\r\nwfd_audio_codecs\r\nwfd_client_rtp_ports\r\n"_s);
    } else if (purpose == u"M3"_s) {
        const QString ports = paramValue(m.body, u"wfd_client_rtp_ports"_s);
        static const QRegularExpression portRe(u"RTP/AVP/UDP;unicast\\s+(\\d+)"_s);
        const auto pm = portRe.match(ports);
        m_sinkRtpPort = pm.hasMatch() ? pm.captured(1).toInt() : 1028;
        if (!paramValue(m.body, u"wfd_audio_codecs"_s).contains(u"AAC"_s)) {
            endSession(u"The TV does not accept AAC audio."_s);
            return;
        }
        if (!chooseFormat(paramValue(m.body, u"wfd_video_formats"_s))) {
            return;
        }
        const QString local = m_socket->localAddress().toString();
        const QString params = u"wfd_video_formats: %1\r\n"
                               "wfd_audio_codecs: AAC 00000001 00\r\n"
                               "wfd_presentation_URL: rtsp://%2/wfd1.0/streamid=0 none\r\n"
                               "wfd_client_rtp_ports: RTP/AVP/UDP;unicast %3 0 mode=play\r\n"_s.arg(m_videoFormats, local)
                                   .arg(m_sinkRtpPort);
        sendRequest(u"SET_PARAMETER"_s, kUri, u"M4"_s, params); // M4
    } else if (purpose == u"M4"_s) {
        sendRequest(u"SET_PARAMETER"_s, kUri, u"M5"_s, u"wfd_trigger_method: SETUP\r\n"_s); // M5
    } else if (purpose == u"teardown"_s) {
        QTimer::singleShot(1500, this, [this] {
            endSession(u"Stopped."_s);
        });
    }
}

bool WfdServer::chooseFormat(const QString &sinkFormats)
{
    // "native preferred profile level CEA VESA HH latency min-slice slice-enc frame-skip max-h max-v"
    const QStringList f = sinkFormats.split(u' ', Qt::SkipEmptyParts);
    bool okCea = false, okLevel = false;
    const quint32 cea = f.value(4).toUInt(&okCea, 16);
    const int sinkLevel = f.value(3).toInt(&okLevel, 16);
    const CeaMode *chosen = nullptr;
    for (const CeaMode &mode : kModes) {
        if (mode.width == m_wanted.width && mode.height == m_wanted.height && mode.fps == m_wanted.fps
            && (!okCea || (cea & (1u << mode.bit)))) {
            chosen = &mode;
        }
    }
    if (!chosen) { // wanted mode not supported: best supported one
        for (const CeaMode &mode : kModes) {
            if (!okCea || (cea & (1u << mode.bit))) {
                chosen = &mode;
                break;
            }
        }
        if (chosen) {
            Q_EMIT log(u"RTSP: the TV does not support %1×%2 at %3 fps, using %4×%5 at %6 fps"_s.arg(m_wanted.width)
                           .arg(m_wanted.height)
                           .arg(m_wanted.fps)
                           .arg(chosen->width)
                           .arg(chosen->height)
                           .arg(chosen->fps));
        }
    }
    if (!chosen) {
        endSession(u"The TV advertised no video mode we can send."_s);
        return false;
    }
    const int level = okLevel && sinkLevel > 0 ? qMin(chosen->level, sinkLevel) : chosen->level;
    m_width = chosen->width;
    m_height = chosen->height;
    m_fps = chosen->fps;
    m_videoFormats = u"00 00 01 %1 %2 00000000 00000000 00 0000 0000 00 none none"_s
                         .arg(level, 2, 16, u'0')
                         .arg(1u << chosen->bit, 8, 16, u'0');
    return true;
}

void WfdServer::handleRequest(const Message &m)
{
    const QString method = m.startLine.section(u' ', 0, 0);
    const QString cseq = m.header(u"cseq"_s);
    if (method == u"OPTIONS"_s) { // M2
        sendReply(cseq, u"Public: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER, SET_PARAMETER\r\n"_s);
    } else if (method == u"SETUP"_s) { // M6
        static const QRegularExpression cpRe(u"client_port=(\\d+)"_s);
        const auto cm = cpRe.match(m.header(u"transport"_s));
        if (cm.hasMatch()) {
            m_sinkRtpPort = cm.captured(1).toInt();
        }
        sendReply(cseq, u"Session: %1;timeout=30\r\nTransport: RTP/AVP/UDP;unicast;client_port=%2;server_port=%3\r\n"_s.arg(m_session).arg(m_sinkRtpPort).arg(m_wanted.localRtpPort));
    } else if (method == u"PLAY"_s) { // M7
        sendReply(cseq, u"Session: %1\r\nRange: npt=now-\r\n"_s.arg(m_session));
        m_keepalive.start(qMax(5, m_wanted.keepaliveSeconds) * 1000);
        Q_EMIT play(m_socket->peerAddress().toString(), m_sinkRtpPort, m_width, m_height, m_fps);
    } else if (method == u"TEARDOWN"_s) {
        sendReply(cseq, u"Session: %1\r\n"_s.arg(m_session));
        endSession(m_tearingDown ? u"Stopped."_s : u"The TV ended the session."_s);
    } else { // GET_PARAMETER keepalive, SET_PARAMETER, PAUSE...
        sendReply(cseq, u"Session: %1\r\n"_s.arg(m_session));
    }
}

void WfdServer::sendRequest(const QString &method, const QString &uri, const QString &purpose,
                            const QString &body, const QString &extraHeaders)
{
    ++m_cseq;
    m_pending.insert(m_cseq, purpose);
    QString text = u"%1 %2 RTSP/1.0\r\nCSeq: %3\r\n"_s.arg(method, uri).arg(m_cseq) + extraHeaders;
    if (!body.isEmpty()) {
        text += u"Content-Type: text/parameters\r\nContent-Length: %1\r\n"_s.arg(body.toUtf8().size());
    }
    send(text + u"\r\n"_s + body);
}

void WfdServer::sendReply(const QString &cseq, const QString &extraHeaders)
{
    send(u"RTSP/1.0 200 OK\r\nCSeq: %1\r\n"_s.arg(cseq) + extraHeaders + u"\r\n"_s);
}

void WfdServer::send(const QString &text)
{
    if (!m_socket) {
        return;
    }
    Q_EMIT log(u"RTSP → %1"_s.arg(text.section(u"\r\n"_s, 0, 0)));
    m_socket->write(text.toUtf8());
}

void WfdServer::teardown()
{
    if (!m_socket) {
        return;
    }
    m_tearingDown = true;
    sendRequest(u"SET_PARAMETER"_s, kUri, u"teardown"_s, u"wfd_trigger_method: TEARDOWN\r\n"_s);
    QTimer::singleShot(3000, this, [this] { // the TV may never answer
        endSession(u"Stopped."_s);
    });
}

void WfdServer::onDisconnected()
{
    endSession(m_tearingDown ? u"Stopped."_s : u"The TV closed the connection."_s);
}

void WfdServer::endSession(const QString &reason)
{
    m_keepalive.stop();
    if (!m_socket) {
        return;
    }
    QTcpSocket *s = m_socket;
    m_socket = nullptr;
    s->disconnect(this);
    s->abort();
    s->deleteLater();
    m_pending.clear();
    if (!reason.isNull()) {
        Q_EMIT log(u"RTSP: session ended: %1"_s.arg(reason));
        Q_EMIT sessionEnded(reason);
    }
}
