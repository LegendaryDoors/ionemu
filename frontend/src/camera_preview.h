// Live camera preview for the Camera settings page: the picture as the game sees it (cropped, mirrored, zoomed),
// red cells where the console's detector sees movement (ion::MotionPreview), blue cells where a running game is
// polling, and a white player outline showing where the games expect the head and shoulders. No text over the
// picture: the explanations live in the page's labels.
#pragma once
#include "ioncore/ioncore.h"
#include <QImage>
#include <QWidget>
#include <vector>

class CameraSource;
class EmuThread;

class CameraPreview : public QWidget {
    Q_OBJECT
public:
    CameraPreview(CameraSource *camera, EmuThread *emu, QWidget *parent = nullptr);
    ~CameraPreview() override;
    void setMirror(bool mirror) { m_mirror = mirror; }
    void setZoom(double zoom) { m_zoom = zoom; }
    void setSensitivity(double s) { m_sensitivity = s; }
    void setOutlineVisible(bool on) { m_outline = on; update(); }
    int flaggedCells() const { return m_flagged; }
    bool hasPicture() const { return !m_picture.isNull(); }
    static void paintOutline(QPainter &p, const QRectF &target, double alpha = 0.9);   // shared with the game view
    QSize sizeHint() const override { return {480, 360}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w * 3 / 4; }

signals:
    void motionChanged(int flaggedCells);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void onFrame();
    CameraSource *m_camera;
    EmuThread *m_emu;
    ion::MotionPreview m_detector;
    QImage m_picture;
    std::vector<uint8_t> m_cells;
    int m_cols = 40, m_rows = 30, m_flagged = 0;
    quint64 m_serial = 0;
    bool m_mirror = true, m_outline = true;
    double m_zoom = 1.0, m_sensitivity = 1.0;
    std::vector<float> m_heat;
    std::vector<uint8_t> m_gameCells;
};
