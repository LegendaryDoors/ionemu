#include "camera_source.h"
#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>
#include <cmath>

CameraSource::CameraSource(QObject *parent) : QObject(parent) {}
CameraSource::~CameraSource() {
    m_destroying = true;
    stop();
}

QList<CameraSource::DeviceInfo> CameraSource::devices() {
    QList<DeviceInfo> out;
    for (const QCameraDevice &d : QMediaDevices::videoInputs()) out.push_back({QString::fromUtf8(d.id()), d.description(), d.isDefault()});
    return out;
}

// Prefer the mode nearest 640x480 at >= 25 fps, uncompressed before MJPEG (the
// console samples 30 camera frames/s; a dim room makes many webcams drop to 15 fps).
static QCameraFormat chooseFormat(const QCameraDevice &dev) {
    QCameraFormat best;
    double bestCost = 1e18;
    for (const QCameraFormat &f : dev.videoFormats()) {
        const QSize r = f.resolution();
        const double fps = f.maxFrameRate();
        const bool mjpeg = f.pixelFormat() == QVideoFrameFormat::Format_Jpeg;
        const double cost = std::abs(r.width() * r.height() - 640 * 480) / 1000.0 + (fps < 25 ? 1e6 : 0) + (60 - std::min(fps, 60.0)) * 10 + (mjpeg ? 500 : 0);
        if (cost < bestCost) { bestCost = cost; best = f; }
    }
    return best;
}

bool CameraSource::start(const QString &deviceId) {
    stop();
    QCameraDevice dev;
    for (const QCameraDevice &d : QMediaDevices::videoInputs())
        if (deviceId.isEmpty() ? d.isDefault() : QString::fromUtf8(d.id()) == deviceId) { dev = d; break; }
    if (dev.isNull() && deviceId.isEmpty() && !QMediaDevices::videoInputs().isEmpty()) dev = QMediaDevices::videoInputs().first();
    if (dev.isNull()) { m_error = deviceId.isEmpty() ? "No camera found" : "Camera not found: " + deviceId; emit errorOccurred(m_error); return false; }
    m_camera = std::make_unique<QCamera>(dev);
    const QCameraFormat fmt = chooseFormat(dev);
    if (!fmt.isNull()) m_camera->setCameraFormat(fmt);
    m_sink = std::make_unique<QVideoSink>();
    m_session = std::make_unique<QMediaCaptureSession>();
    m_session->setCamera(m_camera.get());
    m_session->setVideoSink(m_sink.get());
    connect(m_sink.get(), &QVideoSink::videoFrameChanged, this, &CameraSource::onVideoFrame);
    connect(m_camera.get(), &QCamera::errorOccurred, this, [this](QCamera::Error err, const QString &msg) {
        m_error = msg;
        emit errorOccurred(msg);
        if (err != QCamera::NoError && !m_stopping) stop();          // the device failed or went away: report it as off
    }, Qt::QueuedConnection);
    connect(m_camera.get(), &QCamera::activeChanged, this, [this](bool active) {
        if (!active && !m_stopping && m_camera) { m_error = "camera stopped (unplugged?)"; stop(); }
    }, Qt::QueuedConnection);
    m_deviceId = QString::fromUtf8(dev.id());
    m_format.clear();
    m_fpsCount = 0; m_fps = 0;
    m_fpsClock.start();
    m_camera->start();
    emit stateChanged(true);
    return true;
}

void CameraSource::stop() {
    if (!m_camera) return;
    m_stopping = true;
    m_camera->stop();
    m_session.reset();
    m_sink.reset();
    m_camera.reset();
    m_deviceId.clear();
    m_stopping = false;
    {
        QMutexLocker lock(&m_mutex);
        m_frame = QImage();
    }
    if (!m_destroying) emit stateChanged(false);
}

QString CameraSource::formatDescription() const { return m_format; }
double CameraSource::fps() const { return m_fps; }

bool CameraSource::latestFrame(QImage &out, quint64 &serial) const {
    QMutexLocker lock(&m_mutex);
    if (m_frame.isNull() || m_serial == serial) return false;
    out = m_frame;
    serial = m_serial;
    return true;
}

void CameraSource::onVideoFrame(const QVideoFrame &frame) {
    if (!frame.isValid()) return;
    QImage img = frame.toImage();
    if (img.isNull()) return;
    if (img.format() != QImage::Format_RGB32) img = img.convertToFormat(QImage::Format_RGB32);
    if (m_format.isEmpty()) {
        const QVideoFrameFormat f = frame.surfaceFormat();
        m_format = QString("%1x%2 %3").arg(f.frameWidth()).arg(f.frameHeight()).arg(QVideoFrameFormat::pixelFormatToString(f.pixelFormat()));
    }
    m_fpsCount++;
    if (m_fpsClock.elapsed() >= 1000) {
        m_fps = m_fpsCount * 1000.0 / m_fpsClock.elapsed();
        m_fpsCount = 0;
        m_fpsClock.restart();
    }
    {
        QMutexLocker lock(&m_mutex);
        m_frame = img;
        m_serial++;
    }
    emit frameArrived();
}
