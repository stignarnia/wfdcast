// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#include "audiorouter.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace
{
const QString kSinkName = u"wfdcast"_s;
const QString kBothName = u"wfdcast_both"_s;

QString normalized(QString s)
{
    s = s.toLower().trimmed();
    for (const QString &suffix : {u"-bin"_s, u".bin"_s, u"-wrapped"_s}) {
        if (s.endsWith(suffix)) {
            s.chop(suffix.size());
        }
    }
    return s;
}
}

AudioRouter::AudioRouter(QObject *parent)
    : QObject(parent)
{
    m_followTimer.setInterval(1500);
    connect(&m_followTimer, &QTimer::timeout, this, &AudioRouter::moveAppStreams);
}

AudioRouter::~AudioRouter()
{
    restore();
}

QString AudioRouter::pactl(const QStringList &args, bool *ok)
{
    QProcess p;
    p.start(u"pactl"_s, args);
    const bool done = p.waitForFinished(5000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    if (ok) {
        *ok = done;
    }
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

void AudioRouter::cleanupStale()
{
    const QStringList modules = pactl({u"list"_s, u"short"_s, u"modules"_s}).split(u'\n', Qt::SkipEmptyParts);
    for (const QString &line : modules) {
        if (line.contains(u"sink_name="_s + kSinkName + u' ') || line.contains(u"sink_name="_s + kSinkName + u'\t')
            || line.endsWith(u"sink_name="_s + kSinkName) || line.contains(u"sink_name="_s + kBothName)) {
            pactl({u"unload-module"_s, line.section(u'\t', 0, 0)});
        }
    }
    const QString def = pactl({u"get-default-sink"_s});
    if (def == kSinkName || def == kBothName) { // still pointing at a removed output
        const QStringList sinks = pactl({u"list"_s, u"short"_s, u"sinks"_s}).split(u'\n', Qt::SkipEmptyParts);
        for (const QString &line : sinks) {
            const QString name = line.section(u'\t', 1, 1);
            if (!name.isEmpty() && name != kSinkName && name != kBothName) {
                pactl({u"set-default-sink"_s, name});
                break;
            }
        }
    }
}

QString AudioRouter::loadModule(const QStringList &args, QString *error)
{
    bool ok = false;
    const QString id = pactl(QStringList{u"load-module"_s} + args, &ok);
    if (!ok || id.isEmpty()) {
        *error = u"Could not create an audio output (%1)."_s.arg(args.join(u' '));
        return {};
    }
    m_moduleIds << id;
    return id;
}

QString AudioRouter::setup(const QString &mode, const QString &appId, QString *error)
{
    restore();
    cleanupStale();
    if (mode == u"none"_s) {
        return {};
    }
    bool ok = false;
    m_previousSink = pactl({u"get-default-sink"_s}, &ok);
    if (!ok || m_previousSink.isEmpty()) {
        *error = u"Could not read the default audio output (is pactl installed?). Sending silence."_s;
        return {};
    }

    if (!appId.isEmpty()) {
        // Names that identify the shared window's application in its audio streams:
        // the app id's last part, and the program and name from its .desktop file.
        m_appNames = {normalized(appId.section(u'.', -1))};
        const QString desktop = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, appId + u".desktop"_s);
        QFile f(desktop);
        if (f.open(QIODevice::ReadOnly)) {
            for (const QByteArray &raw : f.readAll().split('\n')) {
                const QString line = QString::fromUtf8(raw);
                if (line.startsWith(u"Exec="_s)) {
                    m_appNames << normalized(line.mid(5).section(u' ', 0, 0).section(u'/', -1));
                } else if (line.startsWith(u"Name="_s)) {
                    m_appNames << normalized(line.mid(5));
                }
                if (line.startsWith(u'[') && line != u"[Desktop Entry]"_s) {
                    break;
                }
            }
        }
        m_appNames.removeDuplicates();
        m_appNames.removeAll(QString());
        if (loadModule({u"module-null-sink"_s, u"sink_name="_s + kSinkName, u"sink_properties=device.description=WFD-Cast-TV"_s}, error)
                .isEmpty()) {
            return {};
        }
        m_appTargetSink = kSinkName;
        if (mode == u"mirror"_s) {
            if (loadModule({u"module-combine-sink"_s, u"sink_name="_s + kBothName, u"slaves="_s + kSinkName + u',' + m_previousSink,
                            u"sink_properties=device.description=WFD-Cast-Laptop-and-TV"_s},
                           error)
                    .isEmpty()) {
                return {};
            }
            m_appTargetSink = kBothName;
        }
        Q_EMIT log(u"Audio: only the shared window's app (%1) goes to the TV%2"_s.arg(
            m_appNames.join(u", "_s), mode == u"mirror"_s ? u", and still plays on the laptop"_s : QString()));
        moveAppStreams();
        m_followTimer.start();
        return kSinkName + u".monitor"_s;
    }

    if (mode == u"mirror"_s) {
        return m_previousSink + u".monitor"_s;
    }
    // mode "tv", whole screen: a virtual output that only the TV hears, made the default
    if (loadModule({u"module-null-sink"_s, u"sink_name="_s + kSinkName, u"sink_properties=device.description=WFD-Cast-TV"_s}, error)
            .isEmpty()) {
        return m_previousSink + u".monitor"_s;
    }
    pactl({u"set-default-sink"_s, kSinkName});
    return kSinkName + u".monitor"_s;
}

bool AudioRouter::matchesApp(const QVariantMap &p) const
{
    const QStringList fields = {normalized(p.value(u"application.process.binary"_s).toString()),
                                normalized(p.value(u"application.name"_s).toString()),
                                normalized(p.value(u"application.id"_s).toString().section(u'.', -1)),
                                normalized(p.value(u"pipewire.access.portal.app_id"_s).toString().section(u'.', -1))};
    for (const QString &f : fields) {
        if (!f.isEmpty() && m_appNames.contains(f)) {
            return true;
        }
    }
    return false;
}

void AudioRouter::moveAppStreams()
{
    const QJsonArray inputs = QJsonDocument::fromJson(pactl({u"-f"_s, u"json"_s, u"list"_s, u"sink-inputs"_s}).toUtf8()).array();
    QHash<QString, QString> sinkNames; // index -> name, to compare with our target
    for (const QString &line : pactl({u"list"_s, u"short"_s, u"sinks"_s}).split(u'\n', Qt::SkipEmptyParts)) {
        sinkNames.insert(line.section(u'\t', 0, 0), line.section(u'\t', 1, 1));
    }
    QStringList seen;
    bool anyMatch = false;
    for (const QJsonValue &v : inputs) {
        const QJsonObject in = v.toObject();
        const QVariantMap seenProps = in.value(u"properties"_s).toObject().toVariantMap();
        seen << u"%1/%2"_s.arg(seenProps.value(u"application.name"_s).toString(), seenProps.value(u"application.process.binary"_s).toString());
        anyMatch = anyMatch || matchesApp(seenProps);
        const int index = in.value(u"index"_s).toInt();
        const QVariantMap props = in.value(u"properties"_s).toObject().toVariantMap();
        if (!matchesApp(props)) {
            continue;
        }
        if (sinkNames.value(QString::number(in.value(u"sink"_s).toInt())) != m_appTargetSink) {
            pactl({u"move-sink-input"_s, QString::number(index), m_appTargetSink});
            if (!m_movedStreams.contains(index)) {
                Q_EMIT log(u"Audio: sending %1 (stream %2) to the TV"_s.arg(props.value(u"application.name"_s).toString()).arg(index));
            }
            m_movedStreams.insert(index);
        }
    }
    if (!anyMatch && !m_loggedNoMatch) {
        m_loggedNoMatch = true;
        Q_EMIT log(u"Audio: no stream of %1 playing yet (streams now: %2)"_s.arg(m_appNames.join(u", "_s), seen.join(u", "_s)));
    }
}

void AudioRouter::restore()
{
    m_followTimer.stop();
    if (!m_previousSink.isEmpty()) {
        for (int index : std::as_const(m_movedStreams)) {
            pactl({u"move-sink-input"_s, QString::number(index), m_previousSink}); // gone streams just fail
        }
        if (pactl({u"get-default-sink"_s}) == kSinkName) {
            pactl({u"set-default-sink"_s, m_previousSink});
        }
    }
    m_movedStreams.clear();
    m_loggedNoMatch = false;
    for (auto it = m_moduleIds.crbegin(); it != m_moduleIds.crend(); ++it) { // combine sink before its slave
        pactl({u"unload-module"_s, *it});
    }
    m_moduleIds.clear();
    m_appTargetSink.clear();
    m_appNames.clear();
    m_previousSink.clear();
}
