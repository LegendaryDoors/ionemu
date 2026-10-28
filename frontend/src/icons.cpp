#include "icons.h"
#include <QApplication>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

namespace icons {

static QPixmap render(const QByteArray &svg, int size, const QColor &colour) {
    QByteArray data = svg;
    data.replace("currentColor", colour.name(QColor::HexRgb).toUtf8());
    QSvgRenderer r(data);
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    r.render(&p);
    return pm;
}

QIcon get(const QString &name) {
    static QHash<QString, QIcon> cache;
    const auto it = cache.constFind(name);
    if (it != cache.constEnd()) return *it;
    QFile f(":/icons/" + name + ".svg");
    QIcon icon;
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray svg = f.readAll();
        const QPalette pal = QApplication::palette();
        const QColor normal = pal.color(QPalette::Active, QPalette::ButtonText), disabled = pal.color(QPalette::Disabled, QPalette::ButtonText);
        for (int size : {16, 24, 32, 48}) {
            icon.addPixmap(render(svg, size, normal), QIcon::Normal);
            icon.addPixmap(render(svg, size, disabled), QIcon::Disabled);
        }
    }
    cache.insert(name, icon);
    return icon;
}

QPixmap coloured(const QString &name, int size, const QColor &colour) {
    QFile f(name == "app" ? QString(":/branding/icon.svg") : ":/icons/" + name + ".svg");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return render(f.readAll(), size, colour.isValid() ? colour : QApplication::palette().color(QPalette::ButtonText));
}

}  // namespace icons
