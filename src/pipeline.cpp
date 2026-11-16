// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#include "pipeline.h"

#include <gst/gst.h>

#include <unistd.h>

using namespace Qt::StringLiterals;

namespace
{
bool hasElement(const char *name)
{
    GstElementFactory *f = gst_element_factory_find(name);
    if (!f) {
        return false;
    }
    gst_object_unref(f);
    return true;
}

const char *kX264Presets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow"};
}

Pipeline::Pipeline(QObject *parent)
    : QObject(parent)
{
    m_busTimer.setInterval(100);
    connect(&m_busTimer, &QTimer::timeout, this, &Pipeline::pollBus);
}

Pipeline::~Pipeline()
{
    stop();
}

QStringList Pipeline::availableEncoders()
{
    QStringList out;
    if (hasElement("x264enc")) {
        out << u"x264"_s;
    }
    if (hasElement("vah264enc") && hasElement("vapostproc")) {
        out << u"vaapi"_s;
    }
    if (hasElement("nvh264enc") && hasElement("cudaupload") && hasElement("cudaconvertscale")) {
        out << u"nvenc"_s;
    }
    return out;
}

QString Pipeline::resolveEncoder(const QString &wanted)
{
    const QStringList have = availableEncoders();
    if (wanted != u"auto"_s) {
        return have.contains(wanted) ? wanted : QString();
    }
    // Auto: the GPU that usually drives the screen first, then the discrete one, then software.
    for (const QString &e : {u"vaapi"_s, u"nvenc"_s, u"x264"_s}) {
        if (have.contains(e)) {
            return e;
        }
    }
    return {};
}

bool Pipeline::start(const Config &c, QString *error)
{
    stop();
    m_encoderName = resolveEncoder(c.encoder);
    if (m_encoderName.isEmpty()) {
        *error = u"The %1 encoder is not available on this system."_s.arg(c.encoder);
        return false;
    }
    const int q = qBound(1, c.quality, 7);
    const int gop = qMax(1, c.keyframeSeconds * c.fps);
    const QString size = u"width=%1,height=%2"_s.arg(c.width).arg(c.height);
    const QString h264 = u"video/x-h264,profile=constrained-baseline,stream-format=byte-stream"_s;

    QString video = u"pipewiresrc name=src fd=%1 path=%2 do-timestamp=true keepalive-time=1000 ! "
                    "queue max-size-buffers=2 leaky=downstream ! videorate ! video/x-raw,framerate=%3/1 ! "_s
                        .arg(::dup(c.pipewireFd))
                        .arg(c.node)
                        .arg(c.fps);
    if (m_encoderName == u"vaapi"_s) {
        video += u"vapostproc add-borders=true ! video/x-raw(memory:VAMemory),format=NV12,%1,pixel-aspect-ratio=1/1 ! "
                 "vah264enc name=enc bitrate=%2 rate-control=cbr key-int-max=%3 b-frames=0 target-usage=%4 ! %5 ! "_s
                     .arg(size)
                     .arg(c.videoKbps)
                     .arg(gop)
                     .arg(8 - q)
                     .arg(h264);
    } else if (m_encoderName == u"nvenc"_s) {
        video += u"cudaupload ! cudaconvertscale add-borders=true ! "
                 "video/x-raw(memory:CUDAMemory),format=NV12,%1,pixel-aspect-ratio=1/1 ! "
                 "nvh264enc name=enc bitrate=%2 rc-mode=cbr gop-size=%3 bframes=0 zerolatency=true preset=p%4 "
                 "tune=ultra-low-latency ! %5 ! "_s
                     .arg(size)
                     .arg(c.videoKbps)
                     .arg(gop)
                     .arg(q)
                     .arg(h264);
    } else {
        video += u"videoconvert ! videoscale add-borders=true ! video/x-raw,format=I420,%1,pixel-aspect-ratio=1/1 ! "
                 "x264enc name=enc tune=zerolatency speed-preset=%2 bitrate=%3 key-int-max=%4 ! %5 ! "_s
                     .arg(size)
                     .arg(QString::fromLatin1(kX264Presets[q - 1]))
                     .arg(c.videoKbps)
                     .arg(gop)
                     .arg(h264);
    }
    video += u"h264parse config-interval=-1 ! queue ! mux. "_s;

    const QString aac = hasElement("fdkaacenc") ? u"fdkaacenc bitrate=%1"_s.arg(c.audioKbps * 1000)
                                                : u"avenc_aac bitrate=%1"_s.arg(c.audioKbps * 1000);
    const QString audioSrc = c.audioDevice.isEmpty()
        ? u"audiotestsrc is-live=true wave=silence"_s
        : u"pulsesrc device=\"%1\" do-timestamp=true provide-clock=false"_s.arg(c.audioDevice);
    const QString audio = audioSrc + u" ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! "_s + aac
        + u" ! aacparse ! queue ! mux. "_s;

    const QString out = u"mpegtsmux name=mux alignment=7 ! rtpmp2tpay ! "
                        "udpsink host=%1 port=%2 bind-port=%3 sync=false async=false"_s.arg(c.sinkIp)
                            .arg(c.sinkPort)
                            .arg(c.localPort);

    const QByteArray desc = (video + audio + out).toUtf8();
    GError *err = nullptr;
    m_pipeline = gst_parse_launch(desc.constData(), &err);
    if (!m_pipeline || err) {
        *error = u"Could not build the stream: %1"_s.arg(QString::fromUtf8(err ? err->message : "unknown error"));
        if (err) {
            g_error_free(err);
        }
        stop();
        return false;
    }
    m_encoder = gst_bin_get_by_name(GST_BIN(m_pipeline), "enc");
    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *error = u"The stream did not start (encoder %1)."_s.arg(m_encoderName);
        stop();
        return false;
    }
    m_busTimer.start();
    return true;
}

void Pipeline::stop()
{
    m_busTimer.stop();
    if (m_encoder) {
        gst_object_unref(m_encoder);
        m_encoder = nullptr;
    }
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
}

bool Pipeline::running() const
{
    return m_pipeline;
}

QString Pipeline::encoderInUse() const
{
    return m_encoderName;
}

void Pipeline::setVideoBitrate(int kbps)
{
    if (m_encoder) {
        g_object_set(m_encoder, "bitrate", guint(kbps), nullptr);
    }
}

void Pipeline::pollBus()
{
    if (!m_pipeline) {
        return;
    }
    GstBus *bus = gst_element_get_bus(m_pipeline);
    while (GstMessage *msg = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
        QString text = u"The stream ended."_s;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            text = u"Stream error (%1): %2"_s.arg(QString::fromUtf8(GST_OBJECT_NAME(msg->src)),
                                                 QString::fromUtf8(err ? err->message : ""));
            g_clear_error(&err);
            g_free(dbg);
        }
        gst_message_unref(msg);
        gst_object_unref(bus);
        Q_EMIT error(text);
        return;
    }
    gst_object_unref(bus);
}
