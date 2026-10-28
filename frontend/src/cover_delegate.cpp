#include "cover_delegate.h"
#include "app.h"
#include "game_list.h"
#include <QFile>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>

CoverDelegate::CoverDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void CoverDelegate::setCoversDir(const QString &dir) { m_coversDir = dir; m_cache.clear(); }

QSize CoverDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const {
    return {kCoverW + 2 * kPad, kCoverH + (m_showTitles ? kTextH : 0) + 2 * kPad};
}

QString CoverDelegate::coverKey(const QString &sha1, const QString &title) { return sha1.isEmpty() ? app::safeFileName(title) : sha1; }

QString CoverDelegate::userCoverFile(const QString &dir, const QString &sha1, const QString &title) {
    for (const QString &name : {sha1, app::safeFileName(title)}) {
        if (name.isEmpty()) continue;
        for (const char *ext : {".png", ".jpg"}) {
            const QString file = dir + "/" + name + ext;
            if (QFile::exists(file)) return file;
        }
    }
    return {};
}

// A user-supplied cover wins; then the generated card; else null (placeholder).
QPixmap CoverDelegate::coverFor(const QString &sha1, const QString &title) const {
    const QString key = coverKey(sha1, title);
    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) return *it;
    QPixmap pm;
    const QString user = userCoverFile(m_coversDir, sha1, title);
    if (!user.isEmpty()) pm.load(user);
    if (pm.isNull()) pm.load(m_coversDir + "/generated/" + key + ".png");
    m_cache.insert(key, pm);
    return pm;
}

// Placeholder: a tinted card (hue from the title) with the title's initials, used when no generated card or user cover exists.
void CoverDelegate::paintPlaceholder(QPainter *p, const QRect &r, const QString &title) {
    const int hue = int(qHash(title) % 360u);
    QLinearGradient g(r.topLeft(), r.bottomRight());
    g.setColorAt(0, QColor::fromHsv(hue, 110, 150));
    g.setColorAt(1, QColor::fromHsv((hue + 40) % 360, 140, 70));
    QPainterPath path;
    path.addRoundedRect(r, 8, 8);
    p->fillPath(path, g);
    p->setPen(QPen(QColor(255, 255, 255, 40), 1));
    p->drawPath(path);

    QString initials;
    static const QRegularExpression sep(R"([\s\-:']+)");
    for (const QString &w : title.split(sep, Qt::SkipEmptyParts)) {
        if (w.compare("the", Qt::CaseInsensitive) == 0 || w.compare("of", Qt::CaseInsensitive) == 0) continue;
        initials += w.at(0).toUpper();
        if (initials.size() == 3) break;
    }
    QFont f = p->font();
    f.setPixelSize(initials.size() >= 3 ? 56 : 68);
    f.setBold(true);
    p->setFont(f);
    p->setPen(QColor(255, 255, 255, 230));
    p->drawText(r, Qt::AlignCenter, initials);
}

void CoverDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRect item = option.rect;
    const QRect cover(item.x() + kPad, item.y() + kPad, kCoverW, kCoverH);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hover = option.state & QStyle::State_MouseOver;
    if (selected || hover) {
        QPainterPath bg;
        bg.addRoundedRect(item.adjusted(2, 2, -2, -2), 10, 10);
        QColor c = option.palette.color(QPalette::Highlight);
        c.setAlpha(selected ? 120 : 40);
        p->fillPath(bg, c);
    }
    const QString title = index.data(Qt::DisplayRole).toString();
    const QPixmap pm = coverFor(index.data(GameListModel::Sha1Role).toString(), title);
    if (pm.isNull()) {
        paintPlaceholder(p, cover, title);
    } else {
        const QPixmap scaled = pm.scaled(cover.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QRect dst(cover.center() - QPoint(scaled.width() / 2, scaled.height() / 2), scaled.size());
        QPainterPath clip;
        clip.addRoundedRect(dst, 6, 6);
        p->setClipPath(clip);
        p->drawPixmap(dst, scaled);
        p->setClipping(false);
    }
    if (index.data(GameListModel::DamagedRole).toBool() || !index.data(GameListModel::KnownRole).toBool()) {
        const bool damaged = index.data(GameListModel::DamagedRole).toBool();
        const QRect badge(cover.right() - 26, cover.top() + 6, 20, 20);
        p->setPen(Qt::NoPen);
        p->setBrush(damaged ? QColor(220, 90, 40) : QColor(90, 90, 100));
        p->drawEllipse(badge);
        p->setPen(Qt::white);
        QFont f = p->font(); f.setBold(true); f.setPixelSize(13); p->setFont(f);
        p->drawText(badge, Qt::AlignCenter, damaged ? "!" : "?");
    }
    if (m_showTitles) {                                    // title under the cover, wrapped to two lines, readable size
        const QRect text(item.x() + kPad, cover.bottom() + 6, kCoverW, kTextH);
        QFont f = option.font;
        f.setPointSizeF(f.pointSizeF() * 1.15);
        f.setBold(true);
        p->setFont(f);
        p->setPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        p->setClipRect(text);
        p->drawText(text, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, title);
    }
    p->restore();
}
