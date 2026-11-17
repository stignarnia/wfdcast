// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#include "pipeline.h"
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QDBusInterface>
#include <QDBusConnection>

#include <QRegularExpression>

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

Pipeline::Gpu Pipeline::compositingGpu()
{
    Gpu gpu;
    QDBusInterface kwin(u"org.kde.KWin"_s, u"/GpuSwitch"_s, u"org.kde.KWin.GpuSwitch"_s, QDBusConnection::sessionBus());
    const QString current = kwin.isValid() ? kwin.property("Current").toString() : QString();
    if (!current.isEmpty()) {
        gpu.pci = current;
        gpu.how = u"kwin-gpuswitch"_s;
    } else {
        // KWin composites on the GPU driving the internal panel by default.
        QString first;
        const QDir drm(u"/sys/class/drm"_s);
        for (const QString &name : drm.entryList({u"card*-*"_s}, QDir::Dirs | QDir::System)) {
            QFile enabled(drm.filePath(name + u"/enabled"_s));
            if (!enabled.open(QIODevice::ReadOnly) || enabled.readAll().trimmed() != "enabled") {
                continue;
            }
            const QString pci = QFileInfo(QFileInfo(drm.filePath(name + u"/device/device"_s)).canonicalFilePath()).fileName();
            if (first.isEmpty()) {
                first = pci;
            }
            if (name.contains(u"-eDP-"_s) || name.contains(u"-LVDS-"_s) || name.contains(u"-DSI-"_s)) {
                gpu.pci = pci;
                gpu.how = u"GPU driving the internal panel"_s;
                break;
            }
        }
        if (gpu.pci.isEmpty() && !first.isEmpty()) {
            gpu.pci = first;
            gpu.how = u"GPU driving the first screen"_s;
        }
    }
    if (gpu.pci.isEmpty()) {
        return gpu;
    }
    QFile vendor(u"/sys/bus/pci/devices/%1/vendor"_s.arg(gpu.pci));
    if (vendor.open(QIODevice::ReadOnly)) {
        const QByteArray v = vendor.readAll().trimmed();
        gpu.vendor = v == "0x10de" ? u"nvidia"_s : v == "0x1002" ? u"amd"_s : v == "0x8086" ? u"intel"_s : QString::fromLatin1(v);
    }
    const QStringList nodes = QDir(u"/sys/bus/pci/devices/%1/drm"_s.arg(gpu.pci)).entryList({u"renderD*"_s}, QDir::Dirs);
    gpu.renderNode = nodes.value(0);
    return gpu;
}

QStringList Pipeline::autoOrder(const Gpu &gpu)
{
    const QStringList have = availableEncoders();
    const QStringList preferred = gpu.vendor == u"nvidia"_s ? QStringList{u"nvenc"_s, u"vaapi"_s, u"x264"_s}
                                                            : QStringList{u"vaapi"_s, u"nvenc"_s, u"x264"_s};
    QStringList out;
    for (const QString &e : preferred) {
        if (have.contains(e)) {
            out << e;
        }
    }
    return out;
}

QString Pipeline::vaElement(const QString &suffix) const
{
    // gst-va names elements of non-default devices like "varenderD129h264enc".
    const QString perDevice = u"va%1%2"_s.arg(m_vaNode, suffix);
    if (!m_vaNode.isEmpty() && hasElement(perDevice.toLatin1().constData())) {
        return perDevice;
    }
    return u"va"_s + suffix;
}

bool Pipeline::start(const Config &c, QString *error)
{
    m_config = c;
    QStringList candidates;
    if (c.encoder == u"auto"_s) {
        const Gpu gpu = compositingGpu();
        m_vaNode = gpu.renderNode;
        candidates = autoOrder(gpu);
        Q_EMIT log(u"Automatic encoder: KDE composites on %1 (%2, %3) → trying %4"_s.arg(
            gpu.vendor.isEmpty() ? u"an unknown GPU"_s : gpu.vendor, gpu.pci, gpu.how, candidates.join(u", "_s)));
    } else {
        m_vaNode.clear();
        if (availableEncoders().contains(c.encoder)) {
            candidates << c.encoder;
        }
    }
    if (candidates.isEmpty()) {
        *error = u"The %1 encoder is not available on this system."_s.arg(c.encoder);
        return false;
    }
    while (!candidates.isEmpty()) {
        const QString enc = candidates.takeFirst();
        if (startOnce(c, enc, error)) {
            m_fallbacks = candidates;
            return true;
        }
        if (!candidates.isEmpty()) {
            Q_EMIT log(u"%1 encoder failed to start (%2); trying %3"_s.arg(enc, *error, candidates.first()));
        }
    }
    return false;
}

bool Pipeline::startOnce(const Config &c, const QString &encoder, QString *error)
{
    stop();
    m_encoderName = encoder;
    const int q = qBound(1, c.quality, 7);
    const int gop = qMax(1, c.keyframeSeconds * c.fps);
    const QString size = u"width=%1,height=%2"_s.arg(c.width).arg(c.height);
    const QString h264 = u"video/x-h264,profile=constrained-baseline,stream-format=byte-stream"_s;

    // KDE only sends a frame when the screen changes. Repeat the last frame every
    // frame interval so the TV gets a steady stream; with gaps its player stalls
    // and catches up instead of running at a constant small delay.
    const int frameMs = qMax(1, 1000 / qMax(1, c.fps));
    // Letterbox in the picture (the TV stretches every mode to its panel), on the
    // encoder's own device: the scaler fits the source's shape (fitcaps), a
    // compositor places it on a black frame of the TV mode. The compositor also
    // produces exactly <fps> frames per second of real time, reusing the last
    // picture when KDE sends none (it only sends on screen changes) and dropping
    // extras, so the stream can never fall behind real time.
    // When the delivered shape changes (resized window) onScalerEvent() re-fits live.
    m_outW = c.width;
    m_outH = c.height;
    m_fit = fitInside(c.sourceWidth > 0 ? c.sourceWidth : c.width, c.sourceHeight > 0 ? c.sourceHeight : c.height, c.width, c.height);
    // video/x-raw = system memory: KWin then hands over plain (memfd) buffers. Left
    // open, it offers its tiled DMA-BUFs (e.g. AMD modifier 0x0200000010401b04),
    // which vapostproc accepted in negotiation and then failed on
    // ("Internal data stream error"). Every working run used system memory.
    // No leaky queue here: with a PipeWire source, leaky queue + compositor dies with
    // "Internal data stream error" (reproduced with pipewiresink -> pipewiresrc). The
    // compositor already holds real time, and PipeWire skips frames when we're busy.
    QString video = u"pipewiresrc name=src fd=%1 path=%2 do-timestamp=true keepalive-time=%3 ! video/x-raw ! "_s.arg(::dup(c.pipewireFd))
                        .arg(c.node)
                        .arg(frameMs);
    QString scale, place, mem;
    const QString format = m_encoderName == u"x264"_s ? u"I420"_s : u"NV12"_s;
    if (m_encoderName == u"vaapi"_s) {
        // AMD: vacompositor fails in the driver with PipeWire input ("vaEndPicture:
        // operation failed"), so a second vapostproc adds the borders around the
        // already correctly shaped picture and videorate holds the frame rate.
        // Borders are added in BGRx: vapostproc fills them with zeros, which is black
        // in RGB but green in NV12. A last vapostproc converts to NV12 for the encoder.
        mem = u"(memory:VAMemory)"_s;
        const QString pp = vaElement(u"postproc"_s);
        scale = pp + u" name=fitscale"_s;
        place = u"%4 add-borders=true ! video/x-raw%1,format=BGRx,%2,pixel-aspect-ratio=1/1 ! "
                "%4 ! video/x-raw%1,format=NV12 ! videorate ! video/x-raw%1,framerate=%3/1 ! "_s.arg(mem, size)
                    .arg(c.fps)
                    .arg(pp);
    } else {
        // NVIDIA / software: the compositor places the picture on a black frame and
        // emits exactly <fps> frames per second of real time.
        QString comp;
        if (m_encoderName == u"nvenc"_s) {
            mem = u"(memory:CUDAMemory)"_s;
            scale = u"cudaupload ! cudaconvertscale name=fitscale"_s;
            comp = u"cudacompositor"_s;
        } else {
            scale = u"videoconvert n-threads=0 ! videoscale name=fitscale n-threads=0"_s;
            comp = u"compositor background=black"_s;
        }
        place = u"%1 name=comp sink_0::xpos=%2 sink_0::ypos=%3 ! video/x-raw%4,format=%5,%6,framerate=%7/1,pixel-aspect-ratio=1/1 ! "_s
                    .arg(comp)
                    .arg(m_fit.left)
                    .arg(m_fit.top)
                    .arg(mem, format, size)
                    .arg(c.fps);
    }
    m_fitCapsTemplate = u"video/x-raw%1,format=%2,width=%3,height=%4,pixel-aspect-ratio=1/1"_s.arg(
        mem, m_encoderName == u"vaapi"_s ? u"BGRx"_s : format);
    video += u"%1 ! capsfilter name=fitcaps caps=\"%2\" ! %3"_s.arg(scale, m_fitCapsTemplate.arg(m_fit.w).arg(m_fit.h), place);
    if (m_encoderName == u"vaapi"_s) {
        video += u"%5 name=enc bitrate=%1 rate-control=cbr key-int-max=%2 b-frames=0 target-usage=%3 ! %4 ! "_s
                     .arg(c.videoKbps)
                     .arg(gop)
                     .arg(8 - q)
                     .arg(h264)
                     .arg(vaElement(u"h264enc"_s));
    } else if (m_encoderName == u"nvenc"_s) {
        video += u"nvh264enc name=enc bitrate=%1 rc-mode=cbr gop-size=%2 bframes=0 zerolatency=true preset=p%3 "
                 "tune=ultra-low-latency ! %4 ! "_s
                     .arg(c.videoKbps)
                     .arg(gop)
                     .arg(q)
                     .arg(h264);
    } else {
        video += u"x264enc name=enc tune=zerolatency speed-preset=%1 bitrate=%2 key-int-max=%3 ! %4 ! "_s
                     .arg(QString::fromLatin1(kX264Presets[q - 1]))
                     .arg(c.videoKbps)
                     .arg(gop)
                     .arg(h264);
    }
    Q_EMIT log(u"Picture: %1×%2 inside %3×%4 (bars %5/%6 px left/right, %7/%8 px top/bottom)"_s.arg(m_fit.w)
                   .arg(m_fit.h)
                   .arg(m_outW)
                   .arg(m_outH)
                   .arg(m_fit.left)
                   .arg(m_fit.right)
                   .arg(m_fit.top)
                   .arg(m_fit.bottom));
    video += u"h264parse config-interval=-1 ! queue ! mux. "_s;

    const QString aac = hasElement("fdkaacenc") ? u"fdkaacenc bitrate=%1"_s.arg(c.audioKbps * 1000)
                                                : u"avenc_aac bitrate=%1"_s.arg(c.audioKbps * 1000);
    const QString audioSrc = c.audioDevice.isEmpty()
        ? u"audiotestsrc is-live=true wave=silence"_s
        // No do-timestamp: audio sources stamp buffers from the sample count, which
        // is smooth; arrival-time stamps jitter and the TV plays audio in chunks.
        : u"pulsesrc device=\"%1\" provide-clock=false"_s.arg(c.audioDevice);
    const QString audio = audioSrc + u" ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! "_s + aac
        + u" ! aacparse ! queue ! mux. "_s;

    const QString out = u"mpegtsmux name=mux alignment=7 ! rtpmp2tpay ! "
                        "udpsink host=%1 port=%2 bind-port=%3 sync=false async=false"_s.arg(c.sinkIp)
                            .arg(c.sinkPort)
                            .arg(c.localPort);

    const QByteArray desc = (video + audio + out).toUtf8();
    Q_EMIT log(u"Pipeline: %1"_s.arg(QString::fromUtf8(desc).replace(QRegularExpression(u"fd=\\d+"_s), u"fd=…"_s)));
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
    // Steady master clock. Otherwise the screen source's clock is picked, which
    // ticks only when KDE delivers frames, and audio gets resynced to it in jerks.
    GstClock *clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(m_pipeline), clock);
    gst_object_unref(clock);
    m_encoder = gst_bin_get_by_name(GST_BIN(m_pipeline), "enc");
    m_fitCaps = gst_bin_get_by_name(GST_BIN(m_pipeline), "fitcaps");
    m_box = gst_bin_get_by_name(GST_BIN(m_pipeline), "comp");
    if (GstElement *scaler = gst_bin_get_by_name(GST_BIN(m_pipeline), "fitscale")) {
        GstPad *sink = gst_element_get_static_pad(scaler, "sink");
        gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, GstPadProbeCallback(&Pipeline::onScalerEvent), this, nullptr);
        gst_object_unref(sink);
        gst_object_unref(scaler);
    }
    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        QString why;
        GstBus *bus = gst_element_get_bus(m_pipeline);
        if (GstMessage *msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
            GError *err = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            why = u": %1 (%2)"_s.arg(QString::fromUtf8(err ? err->message : ""), QString::fromUtf8(dbg ? dbg : ""));
            g_clear_error(&err);
            g_free(dbg);
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
        *error = u"The stream did not start (encoder %1)%2"_s.arg(m_encoderName, why);
        stop();
        return false;
    }
    m_sourceCapsLogged = false;
    m_startedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_busTimer.start();
    return true;
}

void Pipeline::stop()
{
    m_busTimer.stop();
    if (m_fitCaps) {
        gst_object_unref(m_fitCaps);
        m_fitCaps = nullptr;
    }
    if (m_box) {
        gst_object_unref(m_box);
        m_box = nullptr;
    }
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
    if (!m_sourceCapsLogged) {
        if (GstElement *src = gst_bin_get_by_name(GST_BIN(m_pipeline), "src")) {
            GstPad *pad = gst_element_get_static_pad(src, "src");
            if (GstCaps *caps = gst_pad_get_current_caps(pad)) {
                gchar *s = gst_caps_to_string(caps);
                Q_EMIT log(u"Screen capture delivers: %1"_s.arg(QString::fromUtf8(s)));
                g_free(s);
                gst_caps_unref(caps);
                m_sourceCapsLogged = true;
            }
            gst_object_unref(pad);
            gst_object_unref(src);
        }
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
        if (!m_fallbacks.isEmpty() && QDateTime::currentMSecsSinceEpoch() - m_startedAtMs < 3000) {
            const QString failed = m_encoderName;
            QStringList rest = m_fallbacks;
            QString err;
            while (!rest.isEmpty()) {
                const QString next = rest.takeFirst();
                Q_EMIT log(u"%1 encoder failed right after starting (%2); trying %3"_s.arg(failed, text, next));
                if (startOnce(m_config, next, &err)) {
                    m_fallbacks = rest;
                    Q_EMIT encoderChanged();
                    return;
                }
            }
            text += u" (no other encoder worked: "_s + err + u")"_s;
        }
        Q_EMIT error(text);
        return;
    }
    gst_object_unref(bus);
}

Pipeline::Fit Pipeline::fitInside(int srcW, int srcH, int outW, int outH)
{
    Fit f;
    srcW = qMax(1, srcW);
    srcH = qMax(1, srcH);
    f.w = outW;
    f.h = int(qRound(double(outW) * srcH / srcW));
    if (f.h > outH) {
        f.h = outH;
        f.w = int(qRound(double(outH) * srcW / srcH));
    }
    f.w = qMax(2, f.w & ~1);
    f.h = qMax(2, f.h & ~1);
    f.left = ((outW - f.w) / 2) & ~1;
    f.top = ((outH - f.h) / 2) & ~1;
    f.right = outW - f.w - f.left;
    f.bottom = outH - f.h - f.top;
    return f;
}

void Pipeline::applyFit(const Fit &f)
{
    if (!m_fitCaps) {
        return;
    }
    GstCaps *caps = gst_caps_from_string(m_fitCapsTemplate.arg(f.w).arg(f.h).toUtf8().constData());
    if (GstPad *pad = m_box ? gst_element_get_static_pad(m_box, "sink_0") : nullptr) {
        g_object_set(pad, "xpos", f.left, "ypos", f.top, nullptr);
        gst_object_unref(pad);
    }
    g_object_set(m_fitCaps, "caps", caps, nullptr);
    gst_caps_unref(caps);
    m_fit = f;
}

// Streaming thread: the scaler's input caps changed (first caps, or KWin resized
// the stream, e.g. a shared window was resized). Keep the delivered shape.
int Pipeline::onScalerEvent(GstPad *, GstPadProbeInfo *info, void *self)
{
    auto *p = static_cast<Pipeline *>(self);
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS) {
        return GST_PAD_PROBE_OK;
    }
    GstCaps *caps = nullptr;
    gst_event_parse_caps(event, &caps);
    const GstStructure *st = gst_caps_get_structure(caps, 0);
    int w = 0, h = 0, parN = 1, parD = 1;
    if (!gst_structure_get_int(st, "width", &w) || !gst_structure_get_int(st, "height", &h) || w <= 0 || h <= 0) {
        return GST_PAD_PROBE_OK;
    }
    // KWin leaves the pixel aspect unset and pipewiresrc then reports a
    // placeholder (1/2147483647). Only trust plausible values; else square pixels.
    if (!gst_structure_get_fraction(st, "pixel-aspect-ratio", &parN, &parD) || parN <= 0 || parD <= 0
        || double(parN) / parD < 0.25 || double(parN) / parD > 4.0) {
        parN = parD = 1;
    }
    const Fit fit = fitInside(int(qint64(w) * parN / qMax(1, parD)), h, p->m_outW, p->m_outH);
    if (fit == p->m_fit) {
        return GST_PAD_PROBE_OK;
    }
    p->applyFit(fit);
    QMetaObject::invokeMethod(
        p,
        [p, w, h, fit] {
            Q_EMIT p->log(u"Source is now %1×%2: picture %3×%4 inside %5×%6 (bars %7/%8 px left/right, %9/%10 px top/bottom)"_s.arg(w)
                              .arg(h)
                              .arg(fit.w)
                              .arg(fit.h)
                              .arg(p->m_outW)
                              .arg(p->m_outH)
                              .arg(fit.left)
                              .arg(fit.right)
                              .arg(fit.top)
                              .arg(fit.bottom));
        },
        Qt::QueuedConnection);
    return GST_PAD_PROBE_OK;
}
