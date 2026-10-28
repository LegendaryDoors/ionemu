// The webcam through Qt Multimedia: QCamera -> QVideoSink -> RGB32 QImage. Frames are converted on the GUI thread and
// kept as "the newest frame" with a serial number, so any number of consumers (the emulation thread, the setup page
// preview) can each fetch a frame once.
#pragma once
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <memory>

class QCamera;
class QCameraDevice;
class QMediaCaptureSession;
class QVideoSink;

class CameraSource : public QObject {
    Q_OBJECT
public:
    explicit CameraSource(QObject *parent = nullptr);
    ~CameraSource() override;

    struct DeviceInfo { QString id, description; bool isDefault; };
    static QList<DeviceInfo> devices();

    bool start(const QString &deviceId);   // empty id = the default camera; false + lastError() on failure
    void stop();
    bool active() const { return m_camera != nullptr; }
    QString deviceId() const { return m_deviceId; }
    QString lastError() const { return m_error; }
    QString formatDescription() const;     // "640x480 YUYV" once frames arrive
    double fps() const;                    // measured over the last second

    // The newest frame if its serial differs from `serial` (updated). Format_RGB32, camera orientation (not mirrored).
    bool latestFrame(QImage &out, quint64 &serial) const;

signals:
    void frameArrived();
    void stateChanged(bool active);
    void errorOccurred(const QString &message);

private:
    void onVideoFrame(const class QVideoFrame &frame);

    std::unique_ptr<QCamera> m_camera;
    std::unique_ptr<QMediaCaptureSession> m_session;
    std::unique_ptr<QVideoSink> m_sink;
    QString m_deviceId, m_error, m_format;
    mutable QMutex m_mutex;
    QImage m_frame;
    quint64 m_serial = 0;
    QElapsedTimer m_fpsClock;
    int m_fpsCount = 0;
    double m_fps = 0;
    bool m_destroying = false;   // no signals from the destructor: receivers may already be gone
    bool m_stopping = false;     // our own stop() must not be mistaken for the device going away
};
