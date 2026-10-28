// Toolbar/menu icons: Lucide SVGs (ISC) bundled as resources, recoloured for the current palette at load time
// (the SVGs use stroke="currentColor", which Qt's SVG renderer does not resolve).
#pragma once
#include <QIcon>
#include <QColor>
#include <QPixmap>
#include <QString>

namespace icons {
QIcon get(const QString &name);   // e.g. "play"; a null icon if the name is unknown
QPixmap coloured(const QString &name, int size, const QColor &colour);   // one icon in one colour ("app" = the orb logo, colour ignored)
}
