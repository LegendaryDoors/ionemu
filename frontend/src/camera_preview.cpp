#include "camera_preview.h"
#include "camera_source.h"
#include "emu_thread.h"
#include <QPainter>
#include <QPainterPath>

CameraPreview::CameraPreview(CameraSource *camera, EmuThread *emu, QWidget *parent) : QWidget(parent), m_camera(camera), m_emu(emu) {
    setMinimumSize(320, 240);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    if (m_emu) m_emu->addOverlayConsumer(+1);          // blue squares need the game's polling trace while we exist
    connect(m_camera, &CameraSource::frameArrived, this, &CameraPreview::onFrame);
    connect(m_camera, &CameraSource::stateChanged, this, [this](bool active) {
        if (!active) { m_picture = QImage(); m_flagged = 0; m_detector.reset(); emit motionChanged(0); }
        update();
    });
}

CameraPreview::~CameraPreview() {
    if (m_emu) m_emu->addOverlayConsumer(-1);
}

void CameraPreview::onFrame() {
    QImage frame;
    if (!m_camera->latestFrame(frame, m_serial)) return;
    // The games' threshold is 12 (Blue's/Dora) or from a per-profile table; 12 is the setup-page reference.
    m_flagged = m_detector.feed(reinterpret_cast<const uint32_t *>(frame.constBits()), frame.width(), frame.height(), m_mirror,
                                float(m_zoom), float(m_sensitivity), 12, m_cells, m_cols, m_rows);
    m_picture = QImage(reinterpret_cast<const uchar *>(m_detector.picture()), ion::SCREEN_W, ion::SCREEN_H, ion::SCREEN_W * 4, QImage::Format_RGB32).copy();
    for (int y = 0; y < m_picture.height(); y++) {   // the core writes 0x00RRGGBB; Format_RGB32 wants 0xff on top
        QRgb *row = reinterpret_cast<QRgb *>(m_picture.scanLine(y));
        for (int x = 0; x < m_picture.width(); x++) row[x] |= 0xff000000u;
    }
    emit motionChanged(m_flagged);
    update();
}

// Where the games expect the player: head in the upper middle (Blue's Room polls x 96-224, y 56-104 for the head),
// shoulders around y = 130, body filling the lower half.
void CameraPreview::paintOutline(QPainter &p, const QRectF &t, double alpha) {
    const double sx = t.width() / ion::SCREEN_W, sy = t.height() / ion::SCREEN_H;
    auto P = [&](double x, double y) { return QPointF(t.x() + x * sx, t.y() + y * sy); };
    QPainterPath path;
    path.addEllipse(P(160, 78), 23 * sx, 26 * sy);
    QPainterPath body;
    body.moveTo(P(96, 240));
    body.lineTo(P(98, 150));
    body.cubicTo(P(100, 128), P(125, 118), P(145, 116));
    body.lineTo(P(150, 108));
    body.moveTo(P(170, 108));
    body.lineTo(P(175, 116));
    body.cubicTo(P(195, 118), P(220, 128), P(222, 150));
    body.lineTo(P(224, 240));
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor c(255, 255, 255, int(alpha * 255));
    p.setPen(QPen(c, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.drawPath(body);
}

void CameraPreview::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 20, 22));
    int tw = width(), th = width() * 3 / 4;
    if (th > height()) { th = height(); tw = height() * 4 / 3; }
    const QRect t((width() - tw) / 2, (height() - th) / 2, tw, th);
    if (!m_picture.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(t, m_picture);
    } else {
        p.fillRect(t, QColor(40, 40, 44));
    }
    const double cw = t.width() / double(m_cols), ch = t.height() / double(m_rows);
    // blue: where a running game is polling (from the emulation thread), decaying like ionview's overlay
    int gcols = m_cols, grows = m_rows;
    if (m_emu && m_emu->isRunning() && m_emu->overlayData(m_heat, m_gameCells, gcols, grows)) {
        const double hw = t.width() / 20.0, hh = t.height() / 30.0;   // the map is always 600 words: 20 x 30
        for (int i = 0; i < 600 && i < int(m_heat.size()); i++)
            if (m_heat[i] > 0.1f) p.fillRect(QRectF(t.x() + (i % 20) * hw, t.y() + (i / 20) * hh, hw, hh), QColor(40, 120, 255, int(90 * m_heat[i])));
    }
    // red: movement the console would flag
    if (!m_picture.isNull())
        for (int r = 0; r < m_rows; r++)
            for (int c = 0; c < m_cols; c++)
                if (m_cells[size_t(r) * m_cols + c]) p.fillRect(QRectF(t.x() + c * cw, t.y() + r * ch, cw, ch), QColor(255, 40, 40, 120));
    if (m_outline) paintOutline(p, t, m_picture.isNull() ? 0.5 : 0.85);
}
