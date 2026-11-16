// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#include "wfdserver.h"

#include <QRandomGenerator>
#include <QRegularExpression>

#include <tuple>

using namespace Qt::StringLiterals;

namespace
{
// Wi-Fi Display video modes (WFD spec tables): which bitmap, which bit.
// Interlaced CEA modes are left out.
struct Mode {
    int width, height, fps;
    bool vesa; // false = CEA bitmap, true = VESA bitmap
    int bit;
};
constexpr Mode kModes[] = {
    // CEA
    {640, 480, 60, false, 0},    {720, 480, 60, false, 1},    {720, 576, 50, false, 3},
    {1280, 720, 30, false, 5},   {1280, 720, 60, false, 6},   {1920, 1080, 30, false, 7},
    {1920, 1080, 60, false, 8},  {1280, 720, 25, false, 10},  {1280, 720, 50, false, 11},
    {1920, 1080, 25, false, 12}, {1920, 1080, 50, false, 13}, {1280, 720, 24, false, 15},
    {1920, 1080, 24, false, 16},
    // VESA
    {800, 600, 30, true, 0},     {800, 600, 60, true, 1},     {1024, 768, 30, true, 2},
    {1024, 768, 60, true, 3},    {1152, 864, 30, true, 4},    {1152, 864, 60, true, 5},
    {1280, 768, 30, true, 6},    {1280, 768, 60, true, 7},    {1280, 800, 30, true, 8},
    {1280, 800, 60, true, 9},    {1360, 768, 30, true, 10},   {1360, 768, 60, true, 11},
    {1366, 768, 30, true, 12},   {1366, 768, 60, true, 13},   {1280, 1024, 30, true, 14},
    {1280, 1024, 60, true, 15},  {1400, 1050, 30, true, 16},  {1400, 1050, 60, true, 17},
    {1440, 900, 30, true, 18},   {1440, 900, 60, true, 19},   {1600, 900, 30, true, 20},
    {1600, 900, 60, true, 21},   {1600, 1200, 30, true, 22},  {1600, 1200, 60, true, 23},
    {1680, 1024, 30, true, 24},  {1680, 1024, 60, true, 25},  {1680, 1050, 30, true, 26},
    {1680, 1050, 60, true, 27},  {1920, 1200, 30, true, 28},  {1920, 1200, 60, true, 29},
};

// Lowest H.264 level (WFD level code) that fits the mode; 4.0 is promoted to
// 4.1 for its higher bitrate limit.
int levelFor(const Mode &m)
{
    const long mbs = long((m.width + 15) / 16) * ((m.height + 15) / 16);
    const long mbps = mbs * m.fps;
    if (mbs <= 3600 && mbps <= 108000) {
        return 0x01; // 3.1
    }
    if (mbs <= 5120 && mbps <= 216000) {
        return 0x02; // 3.2
    }
    if (mbs <= 8192 && mbps <= 245760) {
        return 0x08; // 4.1
    }
    return 0x10; // 4.2
}

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
    bool okCea = false, okVesa = false, okLevel = false;
    const quint32 cea = f.value(4).toUInt(&okCea, 16);
    const quint32 vesa = f.value(5).toUInt(&okVesa, 16);
    const int sinkLevel = f.value(3).toInt(&okLevel, 16);
    auto advertised = [&](const Mode &m) {
        if (m.vesa) {
            return okVesa && (vesa & (1u << m.bit));
        }
        return okCea ? bool(cea & (1u << m.bit)) : m.bit == 0; // 640x480 is mandatory
    };

    // TV players draw any mode over the whole panel without keeping its shape,
    // so a 16:10 picture gets stretched. The panel shape is taken from the
    // biggest HD mode the TV advertises (CEA modes are its native TV formats);
    // only modes of that shape are sent and the picture is letterboxed here.
    double panelAspect = 0;
    long biggest = 0;
    for (const Mode &m : kModes) {
        if (!m.vesa && m.height >= 720 && advertised(m) && long(m.width) * m.height > biggest) {
            biggest = long(m.width) * m.height;
            panelAspect = double(m.width) / m.height;
        }
    }
    auto supported = [&](const Mode &m) {
        return advertised(m) && (panelAspect <= 0 || qAbs(double(m.width) / m.height - panelAspect) < 0.02);
    };

    // Send the biggest mode of the TV's shape it accepts: the reported source
    // size is logical (a 2x screen has twice the pixels), the TV scales to its
    // panel anyway, and the picture gets letterboxed inside it. Wanted frame
    // rate first, then the closest.
    const int sw = m_wanted.sourceWidth > 0 ? m_wanted.sourceWidth : 1920;
    const int sh = m_wanted.sourceHeight > 0 ? m_wanted.sourceHeight : 1080;
    const Mode *best = nullptr;
    auto rank = [&](const Mode &m) {
        return std::make_tuple(qAbs(m.fps - m_wanted.fps), -long(m.width) * m.height, -m.fps);
    };
    for (const Mode &m : kModes) {
        if (supported(m) && (!best || rank(m) < rank(*best))) {
            best = &m;
        }
    }
    if (!best) {
        endSession(u"The TV advertised no video mode we can send."_s);
        return false;
    }
    const int level = okLevel && sinkLevel > 0 ? qMin(levelFor(*best), sinkLevel) : levelFor(*best);
    m_width = best->width;
    m_height = best->height;
    m_fps = best->fps;
    m_videoFormats = u"00 00 01 %1 %2 %3 00000000 00 0000 0000 00 none none"_s
                         .arg(level, 2, 16, u'0')
                         .arg(best->vesa ? 0u : (1u << best->bit), 8, 16, u'0')
                         .arg(best->vesa ? (1u << best->bit) : 0u, 8, 16, u'0');
    Q_EMIT log(u"RTSP: source %1×%2 → TV mode %3×%4 at %5 fps (%6)"_s.arg(sw)
                   .arg(sh)
                   .arg(m_width)
                   .arg(m_height)
                   .arg(m_fps)
                   .arg(best->width == sw && best->height == sh ? u"exact"_s : u"letterboxed to the TV's shape"_s));
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
