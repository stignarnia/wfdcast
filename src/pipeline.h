// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>

typedef struct _GstElement GstElement;

// The stream: PipeWire screen + PulseAudio sound -> H.264 (Constrained Baseline)
// + AAC -> MPEG-TS -> RTP/UDP to the TV, the format Wi-Fi Display sinks expect.
class Pipeline : public QObject
{
    Q_OBJECT
public:
    struct Config {
        int pipewireFd = -1;
        quint32 node = 0;
        int sourceWidth = 0, sourceHeight = 0; // size KDE reported for the shared source
        QString audioDevice; // empty = silence
        QString encoder; // auto | x264 | vaapi | nvenc
        int width = 1920, height = 1080, fps = 30;
        int videoKbps = 8000, audioKbps = 128;
        int keyframeSeconds = 1;
        int quality = 2; // 1 fastest .. 7 best
        QString sinkIp;
        int sinkPort = 0, localPort = 5000;
    };

    explicit Pipeline(QObject *parent = nullptr);
    ~Pipeline() override;

    bool start(const Config &config, QString *error);
    void stop();
    bool running() const;
    void setVideoBitrate(int kbps);
    QString encoderInUse() const;

    // Encoders this machine can use, for the UI: {"x264", "vaapi", "nvenc"} subset.
    static QStringList availableEncoders();

Q_SIGNALS:
    void error(const QString &message);
    void log(const QString &line);

private:
    void pollBus();
    static QString resolveEncoder(const QString &wanted);

    GstElement *m_pipeline = nullptr;
    GstElement *m_encoder = nullptr;
    QString m_encoderName;
    QTimer m_busTimer;
    bool m_sourceCapsLogged = false;
};
