// Generated covers: the core boots the disc's own intro and renders its title card, which is composed into a portrait
// card with the title underneath. Rendered locally from the user's disc at scan time, cached per SHA-1, never shipped.
#pragma once
#include <QImage>
#include <QString>

namespace covers {

struct Shot { int entry; int frame; bool camera; };            // where a disc shows its title card
Shot shotFor(const QString &sha1, int entries);                // per-disc table, else a default

// Boots `path` and renders the title frame (320x240, composite filter). Null on failure.
QImage renderTitleFrame(const QString &path, const Shot &shot);
// The portrait cover card (2x the grid size for crispness): the frame on a tinted background, the title below.
QImage composeCard(const QImage &frame, const QString &title, bool titleInside = true);
// Convenience: render + compose + save to `file`; false if the disc could not be rendered.
bool generate(const QString &path, const QString &sha1, int entries, const QString &title, const QString &file, bool titleInside = true);

}  // namespace covers
