#include "cover_generator.h"
#include "cover_delegate.h"
#include "ioncore/ioncore.h"
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <vector>

namespace covers {

Shot shotFor(const QString &sha1, int entries) {
    // Title cards found on the discs' own intros (directory entry 2) in the first seconds; Best of Nickelodeon's menu
    // holds the camera picture, so it is rendered without the stand-in player.
    static const struct { const char *sha1; Shot shot; } table[] = {
        {"58b566e71a801063bc0a81bdea7b086f65fdfa58", {2, 150, true}},    // Blue's Room
        {"ef9eb885cee363c97dbc9d6efd67166a10336fb6", {2, 400, true}},    // Dora
        {"4402f14adf50596f6bdefc36316e15e91f50c012", {2, 240, true}},    // Batman
        {"8b3e0260583738b7caa02936d44ff73c13ad3256", {2, 480, false}},   // Best of Nickelodeon
        {"18373e026cabdcb2f1c2f5100cfc6a1ab1f58641", {2, 240, true}},    // SpongeBob (no title card in its intro; Bikini Bottom)
        {"9e3e7a773608492356dbd8cce6fb8ac8246b8176", {2, 120, true}},    // Danny Phantom (repaired)
        {"61fe7ea24c0efffe953406044101775ffc1cabf6", {2, 120, true}},    // Danny Phantom (damaged dump)
    };
    for (const auto &t : table) if (sha1 == t.sha1) return t.shot;
    return entries >= 2 ? Shot{2, 240, true} : Shot{1, 600, true};
}

// Same composite stand-in as the game view.
static void composite(const uint32_t *src, uint32_t *dst) {
    for (int y = 0; y < ion::SCREEN_H; y++) {
        const uint32_t *s = src + y * ion::SCREEN_W;
        uint32_t *d = dst + y * ion::SCREEN_W;
        for (int x = 0; x < ion::SCREEN_W; x++) {
            const uint32_t a = s[x > 0 ? x - 1 : 0], b = s[x], c = s[x < ion::SCREEN_W - 1 ? x + 1 : x];
            uint32_t out = 0;
            for (int sh = 0; sh <= 16; sh += 8)
                out |= ((((a >> sh) & 0xff) + 2 * ((b >> sh) & 0xff) + ((c >> sh) & 0xff) + 2) >> 2) << sh;
            d[x] = out;
        }
    }
}

QImage renderTitleFrame(const QString &path, const Shot &shot) {
    ion::Options opt;
    opt.camera_mode = shot.camera ? 1 : 0;
    ion::Console con(opt);
    if (!con.open(path.toStdString())) return {};
    const int entry = shot.entry <= con.entry_count() ? shot.entry : 1;
    if (!con.power_on(entry)) return {};
    con.input().hand_active = false;
    for (int i = 0; i < shot.frame; i++) { con.step_frame(); con.audio().clear(); con.events().clear(); }
    std::vector<uint32_t> px(size_t(ion::SCREEN_W) * ion::SCREEN_H), shown(px.size());
    con.render(px.data());
    composite(px.data(), shown.data());
    for (uint32_t &v : shown) v |= 0xff000000u;   // Format_RGB32 requires 0xff in the top byte; the core writes 0x00RRGGBB
    return QImage(reinterpret_cast<const uchar *>(shown.data()), ion::SCREEN_W, ion::SCREEN_H, ion::SCREEN_W * 4, QImage::Format_RGB32).copy();
}

QImage composeCard(const QImage &frame, const QString &title, bool titleInside) {
    const int W = CoverDelegate::kCoverW * 2, H = CoverDelegate::kCoverH * 2;   // 360 x 504
    QImage card(W, H, QImage::Format_ARGB32_Premultiplied);
    card.fill(Qt::transparent);
    // background tinted by the frame's average colour
    qint64 r = 0, g = 0, b = 0;
    const QImage small = frame.scaled(32, 24, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < small.height(); y++)
        for (int x = 0; x < small.width(); x++) { const QRgb p = small.pixel(x, y); r += qRed(p); g += qGreen(p); b += qBlue(p); }
    const int n = small.width() * small.height();
    QColor avg(int(r / n), int(g / n), int(b / n));
    int h, s, v;
    avg.getHsv(&h, &s, &v);
    if (h < 0) h = 220;
    const QColor top = QColor::fromHsv(h, qMin(255, s / 2 + 60), 110), bottom = QColor::fromHsv(h, qMin(255, s / 2 + 90), 45);
    QPainter p(&card);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QLinearGradient grad(0, 0, 0, H);
    grad.setColorAt(0, top);
    grad.setColorAt(1, bottom);
    QPainterPath rounded;
    rounded.addRoundedRect(QRectF(0, 0, W, H), 16, 16);
    p.fillPath(rounded, grad);
    // the frame, 4:3, with rounded corners and a thin light edge (centred vertically when there is no title inside)
    const int margin = 14, fw = W - 2 * margin, fh = fw * 3 / 4;
    const QRectF fr(margin, titleInside ? margin : (H - fh) / 2, fw, fh);
    QPainterPath clip;
    clip.addRoundedRect(fr, 10, 10);
    p.save();
    p.setClipPath(clip);
    p.drawImage(fr, frame);
    p.restore();
    p.setPen(QPen(QColor(255, 255, 255, 70), 2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(clip);
    if (!titleInside) { p.end(); return card; }
    // the title below, wrapped, shrinking until it fits
    const QRectF tr(margin, fr.bottom() + 16, fw, H - fr.bottom() - 30);
    QFont f = QFontDatabase::systemFont(QFontDatabase::GeneralFont);   // the platform's UI font ("Sans Serif" would map to
    f.setBold(true);                                                      // the bitmap MS Sans Serif on Windows)
    for (int px = 34; px >= 18; px -= 2) {
        f.setPixelSize(px);
        p.setFont(f);
        const QRectF need = p.boundingRect(tr, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, title);
        if (need.height() <= tr.height()) break;
    }
    p.setPen(QColor(0, 0, 0, 120));
    p.drawText(tr.translated(0, 2), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, title);
    p.setPen(QColor(255, 255, 255, 235));
    p.drawText(tr, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, title);
    p.end();
    return card;
}

bool generate(const QString &path, const QString &sha1, int entries, const QString &title, const QString &file, bool titleInside) {
    const QImage frame = renderTitleFrame(path, shotFor(sha1, entries));
    if (frame.isNull()) return false;
    QDir().mkpath(QFileInfo(file).path());
    return composeCard(frame, title, titleInside).save(file, "PNG");
}

}  // namespace covers
