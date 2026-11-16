// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#pragma once

#include <QString>

// Picks the audio the TV gets, through PipeWire's PulseAudio interface (pactl):
//  "tv"     a virtual output "WFD Cast (TV)" becomes the default output while casting,
//           so sound plays only on the TV;
//  "mirror" the current output's monitor: sound plays on the laptop and the TV;
//  "none"   silence.
class AudioRouter
{
public:
    ~AudioRouter();
    // Returns the PulseAudio source to record ("" = silence). Fills *error on problems.
    QString setup(const QString &mode, QString *error);
    void restore();

private:
    static QString pactl(const QStringList &args, bool *ok = nullptr);
    QString m_previousSink;
    QString m_moduleId;
};
