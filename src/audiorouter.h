// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

// Picks the audio the TV gets, through PipeWire's PulseAudio interface (pactl).
// Whole screen / virtual screen:
//  "tv"     a virtual output "WFD Cast (TV)" becomes the default output while casting,
//           so all sound plays only on the TV;
//  "mirror" the current output's monitor: sound plays on the laptop and the TV;
//  "none"   silence.
// A shared window (appId set): only that application's audio streams are moved,
//  "tv"     to the TV output (other apps keep playing on the laptop),
//  "mirror" to an output that plays on both the laptop and the TV.
// New streams of that app are picked up while casting; all are moved back afterwards.
class AudioRouter : public QObject
{
    Q_OBJECT
public:
    explicit AudioRouter(QObject *parent = nullptr);
    ~AudioRouter() override;
    // Returns the PulseAudio source to record ("" = silence). Fills *error on problems.
    // windowPids: processes owning the shared window (from KWin); their audio streams and
    // those of their child processes go to the TV. Empty: match by application name.
    QString setup(const QString &mode, const QString &appId, const QList<qint64> &windowPids, QString *error);
    void restore();
    // Remove "wfdcast" outputs left by a run that was killed, and leave a real default output.
    static void cleanupStale();

Q_SIGNALS:
    void log(const QString &line);

private:
    static QString pactl(const QStringList &args, bool *ok = nullptr);
    QString loadModule(const QStringList &args, QString *error);
    void moveAppStreams();
    bool matchesApp(const QVariantMap &props) const;

    QString m_previousSink;
    QStringList m_moduleIds;
    QString m_appTargetSink; // where the shared window's app streams go
    QStringList m_appNames;   // normalized names identifying the shared app (fallback)
    QList<qint64> m_windowPids;
    QSet<int> m_movedStreams;
    bool m_loggedNoMatch = false;
    QTimer m_followTimer;
};
