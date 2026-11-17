// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#include "portal.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>

#include <unistd.h>

using namespace Qt::StringLiterals;

namespace
{
const QString kService = u"org.freedesktop.portal.Desktop"_s;
const QString kPath = u"/org/freedesktop/portal/desktop"_s;
const QString kScreenCast = u"org.freedesktop.portal.ScreenCast"_s;
const QString kRequest = u"org.freedesktop.portal.Request"_s;
const QString kSession = u"org.freedesktop.portal.Session"_s;

QString senderToken()
{
    QString s = QDBusConnection::sessionBus().baseService();
    s.remove(0, 1);
    s.replace(u'.', u'_');
    return s;
}
}

Portal::Portal(QObject *parent)
    : QObject(parent)
{
}

Portal::~Portal()
{
    close();
}

void Portal::start(uint sourceTypes, uint cursorMode)
{
    close();
    m_types = sourceTypes ? sourceTypes : 7;
    m_cursor = cursorMode;
    m_step = Step::Create;
    request(u"CreateSession"_s, {}, {{u"session_handle_token"_s, u"wfdcast_session%1"_s.arg(++m_token)}});
}

void Portal::request(const QString &method, const QVariantList &args, QVariantMap options)
{
    disconnectRequest();
    const QString token = u"wfdcast%1"_s.arg(++m_token);
    options.insert(u"handle_token"_s, token);
    m_requestPath = u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(senderToken(), token);
    QDBusConnection::sessionBus().connect(kService, m_requestPath, kRequest, u"Response"_s, this,
                                          SLOT(onResponse(uint, QVariantMap)));

    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kScreenCast, method);
    QVariantList all = args;
    all << options;
    msg.setArguments(all);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, method](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError()) {
            m_step = Step::None;
            Q_EMIT failed(u"Screen sharing portal (%1): %2"_s.arg(method, w->error().message()));
        }
    });
}

void Portal::disconnectRequest()
{
    if (!m_requestPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(kService, m_requestPath, kRequest, u"Response"_s, this,
                                                 SLOT(onResponse(uint, QVariantMap)));
        m_requestPath.clear();
    }
}

void Portal::onResponse(uint code, const QVariantMap &results)
{
    if (code != 0) {
        m_step = Step::None;
        Q_EMIT failed(code == 1 ? u"Screen sharing was cancelled."_s : u"Screen sharing was refused."_s);
        return;
    }
    switch (m_step) {
    case Step::Create: {
        m_session = results.value(u"session_handle"_s).toString();
        QDBusConnection::sessionBus().connect(kService, m_session, kSession, u"Closed"_s, this, SLOT(onSessionClosed()));
        m_step = Step::Select;
        request(u"SelectSources"_s, {QVariant::fromValue(QDBusObjectPath(m_session))},
                {{u"types"_s, m_types}, {u"multiple"_s, false}, {u"cursor_mode"_s, m_cursor}});
        break;
    }
    case Step::Select:
        m_step = Step::Start;
        request(u"Start"_s, {QVariant::fromValue(QDBusObjectPath(m_session)), QString()}, {});
        break;
    case Step::Start: {
        m_step = Step::None;
        disconnectRequest();
        quint32 node = 0;
        QSize size;
        uint sourceType = 0;
        const QDBusArgument streams = results.value(u"streams"_s).value<QDBusArgument>();
        streams.beginArray();
        if (!streams.atEnd()) {
            QVariantMap props;
            streams.beginStructure();
            streams >> node >> props;
            streams.endStructure();
            sourceType = props.value(u"source_type"_s).toUInt();
            const QDBusArgument sz = props.value(u"size"_s).value<QDBusArgument>();
            if (sz.currentType() == QDBusArgument::StructureType) {
                int w = 0, h = 0;
                sz.beginStructure();
                sz >> w >> h;
                sz.endStructure();
                size = QSize(w, h);
            }
        }
        streams.endArray();
        if (!node) {
            Q_EMIT failed(u"The screen sharing portal returned no stream."_s);
            return;
        }
        QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kScreenCast, u"OpenPipeWireRemote"_s);
        msg.setArguments({QVariant::fromValue(QDBusObjectPath(m_session)), QVariantMap()});
        const QDBusReply<QDBusUnixFileDescriptor> reply = QDBusConnection::sessionBus().call(msg);
        if (!reply.isValid()) {
            Q_EMIT failed(u"Could not open the PipeWire stream: %1"_s.arg(reply.error().message()));
            return;
        }
        Q_EMIT ready(::dup(reply.value().fileDescriptor()), node, size, sourceType);
        break;
    }
    case Step::None:
        break;
    }
}

void Portal::onSessionClosed()
{
    m_session.clear();
    Q_EMIT closed();
}

void Portal::close()
{
    disconnectRequest();
    m_step = Step::None;
    if (!m_session.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(kService, m_session, kSession, u"Closed"_s, this, SLOT(onSessionClosed()));
        QDBusMessage msg = QDBusMessage::createMethodCall(kService, m_session, kSession, u"Close"_s);
        QDBusConnection::sessionBus().asyncCall(msg);
        m_session.clear();
    }
}
