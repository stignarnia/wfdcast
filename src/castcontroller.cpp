// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#include "castcontroller.h"
#include "settings.h"

#include <KLocalizedString>
#include <QDateTime>
#include <QJsonDocument>

#include <unistd.h>

using namespace Qt::StringLiterals;

#ifndef WFDCAST_HELPER_PATH
#define WFDCAST_HELPER_PATH "/usr/local/lib/wfdcast/wfdcast-helper.py"
#endif

CastController::CastController(Settings *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    connect(&m_portal, &Portal::ready, this, &CastController::onPortalReady);
    connect(&m_portal, &Portal::failed, this, &CastController::fail);
    connect(&m_portal, &Portal::closed, this, [this] {
        if (m_state != u"idle"_s && m_state != u"stopping"_s) {
            appendLog(u"Screen sharing was stopped from the desktop."_s);
            stop();
        }
    });
    connect(&m_server, &WfdServer::play, this, &CastController::onPlay);
    connect(&m_server, &WfdServer::sessionEnded, this, &CastController::onSessionEnded);
    connect(&m_server, &WfdServer::log, this, &CastController::appendLog);
    connect(&m_pipeline, &Pipeline::error, this, &CastController::fail);
    connect(&m_helper, &QProcess::readyReadStandardOutput, this, &CastController::onHelperOutput);
    connect(&m_helper, &QProcess::readyReadStandardError, this, [this] {
        const QStringList lines = QString::fromUtf8(m_helper.readAllStandardError()).split(u'\n', Qt::SkipEmptyParts);
        for (const QString &l : lines) {
            appendLog(u"helper: "_s + l);
        }
    });
    connect(&m_helper, &QProcess::finished, this, &CastController::onHelperFinished);
    connect(m_settings, &QQmlPropertyMap::valueChanged, this, &CastController::onSettingChanged);

    m_tvWait.setSingleShot(true);
    connect(&m_tvWait, &QTimer::timeout, this, [this] {
        if (m_state != u"waiting"_s || m_server.hasSession()) {
            return;
        }
        if (++m_nudges > 3) {
            fail(i18n("The TV joined but never started the stream. Close and reopen Screen mirroring on the TV, then try again."));
            return;
        }
        appendLog(u"The TV has not connected yet; reconnecting the Wi-Fi Direct link to nudge it (%1/3)"_s.arg(m_nudges));
        sendHelper({{u"cmd"_s, u"reassociate"_s}});
        m_tvWait.start(m_settings->getInt(u"tvWaitSeconds"_s) * 1000);
    });
}

CastController::~CastController()
{
    m_server.close();
    m_pipeline.stop();
    if (m_helper.state() != QProcess::NotRunning) {
        sendHelper({{u"cmd"_s, u"quit"_s}});
        m_helper.waitForFinished(8000);
    }
    m_audio.restore();
    m_portal.close();
    if (m_pwFd >= 0) {
        ::close(m_pwFd);
    }
}

void CastController::setState(const QString &s)
{
    if (m_state != s) {
        m_state = s;
        Q_EMIT stateChanged();
    }
}

void CastController::setStatus(const QString &s, bool error)
{
    m_status = s;
    m_error = error;
    appendLog(s);
    Q_EMIT statusChanged();
}

void CastController::appendLog(const QString &line)
{
    m_log << u"%1  %2"_s.arg(QTime::currentTime().toString(u"HH:mm:ss"_s), line);
    while (m_log.size() > 1000) {
        m_log.removeFirst();
    }
    Q_EMIT logChanged();
}

void CastController::clearLog()
{
    m_log.clear();
    Q_EMIT logChanged();
}

void CastController::start()
{
    if (m_state != u"idle"_s) {
        return;
    }
    m_peers.clear();
    Q_EMIT peersChanged();
    m_nudges = 0;
    setState(u"picking"_s);
    setStatus(i18n("Choose what to show on the TV."));
    uint types = 0;
    if (m_settings->getBool(u"pickScreens"_s)) {
        types |= 1;
    }
    if (m_settings->getBool(u"pickWindows"_s)) {
        types |= 2;
    }
    if (m_settings->getBool(u"pickVirtual"_s)) {
        types |= 4;
    }
    m_portal.start(types ? types : 7, m_settings->getString(u"cursorMode"_s) == u"hidden"_s ? 1 : 2);
}

void CastController::onPortalReady(int fd, quint32 node, QSize size)
{
    if (m_pwFd >= 0) {
        ::close(m_pwFd);
    }
    m_pwFd = fd;
    m_pwNode = node;
    appendLog(u"Screen source ready: PipeWire node %1, %2×%3"_s.arg(node).arg(size.width()).arg(size.height()));

    QString audioWarning;
    m_audioDevice = m_audio.setup(m_settings->getString(u"audioMode"_s), &audioWarning);
    if (!audioWarning.isEmpty()) {
        appendLog(audioWarning);
    }

    WfdServer::Wanted wanted;
    wanted.sourceWidth = size.width();
    wanted.sourceHeight = size.height();
    wanted.fps = m_settings->getInt(u"fps"_s);
    wanted.keepaliveSeconds = m_settings->getInt(u"keepaliveSeconds"_s);
    wanted.localRtpPort = m_settings->getInt(u"rtpPort"_s);
    m_server.setWanted(wanted);
    QString err;
    if (!m_server.listen(quint16(m_settings->getInt(u"rtspPort"_s)), &err)) {
        fail(err);
        return;
    }
    startHelper();
}

void CastController::startHelper()
{
    setState(u"authenticating"_s);
    setStatus(i18n("Authenticate to set up the Wi-Fi Direct link to the TV."));
    m_helperBuffer.clear();
    const QString helper = qEnvironmentVariableIsSet("WFDCAST_HELPER") ? qEnvironmentVariable("WFDCAST_HELPER")
                                                                        : QStringLiteral(WFDCAST_HELPER_PATH);
    m_helper.start(u"pkexec"_s, {helper});
    if (!m_helper.waitForStarted(5000)) {
        fail(i18n("Could not start pkexec: %1", m_helper.errorString()));
        return;
    }
    QJsonObject start{{u"cmd"_s, u"start"_s}};
    start.insert(u"iface"_s, m_settings->getString(u"iface"_s));
    start.insert(u"name"_s, m_settings->getString(u"deviceName"_s));
    start.insert(u"rtsp_port"_s, m_settings->getInt(u"rtspPort"_s));
    start.insert(u"throughput"_s, m_settings->getInt(u"throughput"_s));
    start.insert(u"find_timeout"_s, m_settings->getInt(u"findTimeout"_s));
    start.insert(u"search_round"_s, m_settings->getInt(u"searchRound"_s));
    start.insert(u"answer_timeout"_s, m_settings->getInt(u"answerTimeout"_s));
    start.insert(u"accept_timeout"_s, m_settings->getInt(u"acceptTimeout"_s));
    start.insert(u"assoc_timeout"_s, m_settings->getInt(u"assocTimeout"_s));
    start.insert(u"dhcp_timeout"_s, m_settings->getInt(u"dhcpTimeout"_s));
    sendHelper(start);
}

void CastController::sendHelper(const QJsonObject &cmd)
{
    if (m_helper.state() == QProcess::Running) {
        m_helper.write(QJsonDocument(cmd).toJson(QJsonDocument::Compact) + '\n');
    }
}

void CastController::onHelperOutput()
{
    m_helperBuffer += m_helper.readAllStandardOutput();
    int nl;
    while ((nl = m_helperBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_helperBuffer.left(nl);
        m_helperBuffer.remove(0, nl + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) {
            onHelperEvent(doc.object());
        } else if (!line.trimmed().isEmpty()) {
            appendLog(u"helper: "_s + QString::fromUtf8(line));
        }
    }
}

void CastController::onHelperEvent(const QJsonObject &e)
{
    const QString ev = e.value(u"event"_s).toString();
    if (ev == u"log"_s) {
        appendLog(u"helper: "_s + e.value(u"msg"_s).toString());
    } else if (ev == u"ready"_s) {
        appendLog(u"Wi-Fi Direct link ready on %1 (main Wi-Fi %2 untouched)"_s.arg(e.value(u"iface"_s).toString(),
                                                                              e.value(u"main"_s).toString()));
        const QString mac = m_settings->getString(u"tvMac"_s);
        if (!mac.isEmpty()) {
            m_targetName = m_settings->getString(u"tvName"_s);
            connectTo(mac);
        } else {
            setState(u"choosing"_s);
            setStatus(i18n("Looking for TVs… Open Screen mirroring on the TV, then choose it below."));
            sendHelper({{u"cmd"_s, u"scan"_s}});
        }
    } else if (ev == u"peer"_s) {
        if (!e.value(u"sink"_s).toBool()) {
            return;
        }
        const QString mac = e.value(u"mac"_s).toString();
        for (const QVariant &p : std::as_const(m_peers)) {
            if (p.toMap().value(u"mac"_s) == mac) {
                return;
            }
        }
        m_peers << QVariantMap{{u"mac"_s, mac}, {u"name"_s, e.value(u"name"_s).toString()}};
        Q_EMIT peersChanged();
    } else if (ev == u"status"_s) {
        setStatus(e.value(u"msg"_s).toString());
    } else if (ev == u"accept"_s) {
        setState(u"accept"_s);
        setStatus(i18n("Press Accept on the TV."));
    } else if (ev == u"credentials"_s) {
        m_settings->put(u"tvSsid"_s, e.value(u"ssid"_s).toString());
        m_settings->put(u"tvPsk"_s, e.value(u"psk"_s).toString());
        m_settings->put(u"tvPskHex"_s, e.value(u"psk_hex"_s).toBool());
        m_settings->put(u"tvBssid"_s, e.value(u"bssid"_s).toString());
        appendLog(u"Pairing saved: next time no Accept on the TV is needed."_s);
    } else if (ev == u"forget"_s) {
        m_settings->put(u"tvSsid"_s, QString());
        m_settings->put(u"tvPsk"_s, QString());
        m_settings->put(u"tvBssid"_s, QString());
    } else if (ev == u"connected"_s) {
        setState(u"waiting"_s);
        setStatus(i18n("Connected to the TV's network (%1). Waiting for the TV to start the stream…",
                       e.value(u"ip"_s).toString()));
        m_tvWait.start(m_settings->getInt(u"tvWaitSeconds"_s) * 1000);
    } else if (ev == u"disconnected"_s) {
        if (m_state != u"stopping"_s) {
            setStatus(i18n("Lost the link to the TV, reconnecting…"));
        }
    } else if (ev == u"error"_s) {
        fail(e.value(u"msg"_s).toString());
    }
}

void CastController::connectTo(const QString &mac)
{
    m_targetMac = mac;
    setState(u"joining"_s);
    setStatus(i18n("Connecting to %1…", m_targetName.isEmpty() ? mac : m_targetName));
    QJsonObject cmd{{u"cmd"_s, u"connect"_s}, {u"mac"_s, mac}};
    if (!m_settings->getString(u"tvPsk"_s).isEmpty() && m_settings->getString(u"tvMac"_s) == mac) {
        cmd.insert(u"creds"_s,
                   QJsonObject{{u"ssid"_s, m_settings->getString(u"tvSsid"_s)},
                               {u"psk"_s, m_settings->getString(u"tvPsk"_s)},
                               {u"psk_hex"_s, m_settings->getBool(u"tvPskHex"_s)},
                               {u"bssid"_s, m_settings->getString(u"tvBssid"_s)}});
    }
    sendHelper(cmd);
}

void CastController::choosePeer(const QString &mac, const QString &name)
{
    if (m_state != u"choosing"_s) {
        return;
    }
    if (m_settings->getString(u"tvMac"_s) != mac) {
        forgetTv();
    }
    m_settings->put(u"tvMac"_s, mac);
    m_settings->put(u"tvName"_s, name);
    m_targetName = name;
    connectTo(mac);
}

void CastController::forgetTv()
{
    for (const QString &k : {u"tvMac"_s, u"tvName"_s, u"tvSsid"_s, u"tvPsk"_s, u"tvBssid"_s}) {
        m_settings->put(k, QString());
    }
    m_settings->put(u"tvPskHex"_s, false);
}

void CastController::onPlay(const QString &ip, int port, int w, int h, int fps)
{
    m_tvWait.stop();
    Pipeline::Config c;
    c.pipewireFd = m_pwFd;
    c.node = m_pwNode;
    c.audioDevice = m_audioDevice;
    c.encoder = m_settings->getString(u"encoder"_s);
    c.width = w;
    c.height = h;
    c.fps = fps;
    c.videoKbps = m_settings->getInt(u"videoBitrate"_s);
    c.audioKbps = m_settings->getInt(u"audioBitrate"_s);
    c.keyframeSeconds = m_settings->getInt(u"keyframeSeconds"_s);
    c.quality = m_settings->getInt(u"encoderQuality"_s);
    c.sinkIp = ip;
    c.sinkPort = port;
    c.localPort = m_settings->getInt(u"rtpPort"_s);
    QString err;
    if (!m_pipeline.start(c, &err)) {
        fail(err);
        return;
    }
    setState(u"streaming"_s);
    setStatus(i18n("Casting to %1 at %2×%3, %4 fps (%5 encoder).",
                   m_targetName.isEmpty() ? m_targetMac : m_targetName, w, h, fps, m_pipeline.encoderInUse()));
}

void CastController::onSessionEnded(const QString &reason)
{
    m_pipeline.stop();
    Q_EMIT stateChanged();
    if (m_state == u"stopping"_s) {
        finishStop();
    } else if (m_state == u"streaming"_s || m_state == u"waiting"_s) {
        // The TV usually reconnects on its own after closing a session.
        setState(u"waiting"_s);
        setStatus(i18n("%1 Waiting for the TV to reconnect…", reason));
        m_tvWait.start(m_settings->getInt(u"tvWaitSeconds"_s) * 1000);
    }
}

void CastController::onSettingChanged(const QString &key, const QVariant &value)
{
    if (key == u"videoBitrate"_s && m_pipeline.running()) {
        m_pipeline.setVideoBitrate(value.toInt());
        appendLog(u"Video bitrate set to %1 kbit/s"_s.arg(value.toInt()));
    }
}

void CastController::fail(const QString &message)
{
    if (m_state == u"idle"_s) {
        setStatus(message, true);
        return;
    }
    const QString msg = message;
    stop();
    setStatus(msg, true);
}

void CastController::stop()
{
    if (m_state == u"idle"_s || m_state == u"stopping"_s) {
        return;
    }
    m_tvWait.stop();
    setState(u"stopping"_s);
    setStatus(i18n("Stopping…"));
    if (m_server.hasSession()) {
        m_server.teardown(); // finishStop() follows from sessionEnded
        return;
    }
    finishStop();
}

void CastController::finishStop()
{
    m_pipeline.stop();
    m_server.close();
    if (m_helper.state() != QProcess::NotRunning) {
        sendHelper({{u"cmd"_s, u"quit"_s}});
        m_helper.closeWriteChannel();
        if (!m_helper.waitForFinished(10000)) {
            m_helper.kill();
        }
    }
    m_audio.restore();
    m_portal.close();
    if (m_pwFd >= 0) {
        ::close(m_pwFd);
        m_pwFd = -1;
    }
    m_peers.clear();
    Q_EMIT peersChanged();
    setState(u"idle"_s);
    setStatus(i18n("Not casting."));
}

void CastController::onHelperFinished(int code, QProcess::ExitStatus status)
{
    if (m_state == u"idle"_s || m_state == u"stopping"_s) {
        return;
    }
    if (status == QProcess::NormalExit && (code == 126 || code == 127)) {
        fail(i18n("Authentication was cancelled or refused."));
    } else {
        fail(i18n("The Wi-Fi Direct helper stopped unexpectedly (exit code %1). See the log.", code));
    }
}
