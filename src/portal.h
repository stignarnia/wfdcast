// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QSize>
#include <QVariantMap>

// xdg-desktop-portal ScreenCast: shows KDE's picker (screen, window or a new
// virtual monitor) and hands back a PipeWire fd + node for the chosen source.
class Portal : public QObject
{
    Q_OBJECT
public:
    explicit Portal(QObject *parent = nullptr);
    ~Portal() override;

    // sourceTypes: bitmask 1 = monitor, 2 = window, 4 = virtual. cursorMode: 1 hidden, 2 embedded.
    void start(uint sourceTypes, uint cursorMode);
    void close();

Q_SIGNALS:
    void ready(int pipewireFd, quint32 node, QSize size);
    void failed(const QString &message);
    void closed();

private Q_SLOTS:
    void onResponse(uint code, const QVariantMap &results);
    void onSessionClosed();

private:
    void request(const QString &method, const QVariantList &args, QVariantMap options);
    void disconnectRequest();

    enum class Step { None, Create, Select, Start };
    Step m_step = Step::None;
    uint m_types = 7;
    uint m_cursor = 2;
    QString m_session;
    QString m_requestPath;
    int m_token = 0;
};
