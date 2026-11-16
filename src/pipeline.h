// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QSize>
#include <QStringList>
#include <QTimer>

typedef struct _GstElement GstElement;
typedef struct _GstPad GstPad;
typedef struct _GstPadProbeInfo GstPadProbeInfo;

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
    QSize pictureSize() const { return {m_fit.w, m_fit.h}; } // the source's part of the TV mode

    // Encoders this machine can use, for the UI: {"x264", "vaapi", "nvenc"} subset.
    static QStringList availableEncoders();

Q_SIGNALS:
    void error(const QString &message);
    void log(const QString &line);

private:
    void pollBus();

    // Where the source sits inside the TV mode: its shape kept, black bars around.
    struct Fit {
        int w = 0, h = 0, left = 0, right = 0, top = 0, bottom = 0;
        bool operator==(const Fit &) const = default;
    };
    static Fit fitInside(int srcW, int srcH, int outW, int outH);
    void applyFit(const Fit &fit); // live: scaler caps + borders
    static int onScalerEvent(GstPad *pad, GstPadProbeInfo *info, void *self);
    static QString resolveEncoder(const QString &wanted);

    GstElement *m_pipeline = nullptr;
    GstElement *m_encoder = nullptr;
    QString m_encoderName;
    QTimer m_busTimer;
    bool m_sourceCapsLogged = false;
    GstElement *m_fitCaps = nullptr;
    GstElement *m_box = nullptr; // the compositor placing the picture on the black frame
    QString m_fitCapsTemplate; // %1 width, %2 height
    Fit m_fit;
    int m_outW = 0, m_outH = 0;
};
