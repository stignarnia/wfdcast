// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#include "audiorouter.h"

#include <QProcess>

using namespace Qt::StringLiterals;

namespace
{
const QString kSinkName = u"wfdcast"_s;
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
        if (line.contains(u"module-null-sink"_s) && line.contains(u"sink_name="_s + kSinkName)) {
            pactl({u"unload-module"_s, line.section(u'\t', 0, 0)});
        }
    }
    if (pactl({u"get-default-sink"_s}) == kSinkName) { // still pointing at a removed output
        const QStringList sinks = pactl({u"list"_s, u"short"_s, u"sinks"_s}).split(u'\n', Qt::SkipEmptyParts);
        for (const QString &line : sinks) {
            const QString name = line.section(u'\t', 1, 1);
            if (!name.isEmpty() && name != kSinkName) {
                pactl({u"set-default-sink"_s, name});
                break;
            }
        }
    }
}

QString AudioRouter::setup(const QString &mode, QString *error)
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
    if (mode == u"mirror"_s) {
        return m_previousSink + u".monitor"_s;
    }
    // mode "tv": a virtual output that only the TV hears
    m_moduleId = pactl({u"load-module"_s, u"module-null-sink"_s, u"sink_name="_s + kSinkName,
                        u"sink_properties=device.description=WFD-Cast-TV"_s},
                       &ok);
    if (!ok) {
        m_moduleId.clear();
        *error = u"Could not create the TV audio output. Mirroring the laptop output instead."_s;
        return m_previousSink + u".monitor"_s;
    }
    pactl({u"set-default-sink"_s, kSinkName});
    return kSinkName + u".monitor"_s;
}

void AudioRouter::restore()
{
    if (!m_moduleId.isEmpty()) {
        if (!m_previousSink.isEmpty()) {
            pactl({u"set-default-sink"_s, m_previousSink});
        }
        pactl({u"unload-module"_s, m_moduleId});
        m_moduleId.clear();
    }
    m_previousSink.clear();
}
