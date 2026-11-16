// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#include "settings.h"

#include <KSharedConfig>
#include <QFile>
#include <QHostInfo>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

Settings::Settings(QObject *parent)
    : QQmlPropertyMap(this, parent)
{
    m_defaults = {
        // Picture
        {u"fps"_s, 60}, // 60: audio is released with each video frame, smaller clumps for the TV
        {u"videoBitrate"_s, 8000}, // kbit/s, changes live
        {u"keyframeSeconds"_s, 1},
        {u"encoder"_s, u"auto"_s}, // auto | x264 | vaapi | nvenc
        {u"encoderQuality"_s, 2}, // 1 fastest .. 7 best quality
        // Sound
        {u"audioMode"_s, u"tv"_s}, // tv | mirror | none
        {u"audioBitrate"_s, 128}, // kbit/s
        // KDE picker
        {u"pickScreens"_s, true},
        {u"pickWindows"_s, true},
        {u"pickVirtual"_s, true},
        {u"cursorMode"_s, u"embedded"_s}, // embedded | hidden
        // Connection (advanced)
        {u"deviceName"_s, QHostInfo::localHostName()},
        {u"iface"_s, u"wfd0"_s},
        {u"rtspPort"_s, 7236},
        {u"rtpPort"_s, 5000},
        {u"throughput"_s, 200}, // Mbit/s announced to the TV
        {u"findTimeout"_s, 60},
        {u"searchRound"_s, 10}, // s per Wi-Fi Direct search round
        {u"answerTimeout"_s, 8}, // s after the join request before pairing starts anyway
        {u"acceptTimeout"_s, 120},
        {u"assocTimeout"_s, 20},
        {u"dhcpTimeout"_s, 15},
        {u"tvWaitSeconds"_s, 12}, // wait for the TV's RTSP connection before nudging it
        {u"keepaliveSeconds"_s, 20},
        // Remembered TV (written by the app)
        {u"tvMac"_s, QString()},
        {u"tvName"_s, QString()},
        {u"tvSsid"_s, QString()},
        {u"tvPskHex"_s, false},
        {u"tvBssid"_s, QString()},
    };
    m_group = KSharedConfig::openConfig(u"wfdcastrc"_s)->group(u"Settings"_s);
    for (auto it = m_defaults.cbegin(); it != m_defaults.cend(); ++it) {
        insert(it.key(), m_group.readEntry(it.key(), it.value()));
    }
}

QVariant Settings::get(const QString &key) const
{
    return value(key);
}

int Settings::getInt(const QString &key) const
{
    return value(key).toInt();
}

QString Settings::getString(const QString &key) const
{
    return value(key).toString();
}

bool Settings::getBool(const QString &key) const
{
    return value(key).toBool();
}

void Settings::put(const QString &key, const QVariant &v)
{
    insert(key, v);
    save(key, v);
    Q_EMIT valueChanged(key, v);
}

QVariant Settings::defaultValue(const QString &key) const
{
    return m_defaults.value(key);
}

void Settings::resetToDefaults()
{
    for (auto it = m_defaults.cbegin(); it != m_defaults.cend(); ++it) {
        if (!it.key().startsWith(u"tv"_s)) {
            put(it.key(), it.value());
        }
    }
}

QVariant Settings::updateValue(const QString &key, const QVariant &input)
{
    // Keep the stored type (QML hands us doubles for ints, etc.)
    QVariant v = input;
    const QVariant def = m_defaults.value(key);
    if (def.isValid() && v.canConvert(def.metaType())) {
        v.convert(def.metaType());
    }
    save(key, v);
    return v;
}

void Settings::save(const QString &key, const QVariant &v)
{
    m_group.writeEntry(key, v);
    m_group.sync();
    // Owner-only: it names the TV and its Wi-Fi Direct network (the passphrase is in KWallet).
    const QString path = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, u"wfdcastrc"_s);
    if (!path.isEmpty()) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
}
