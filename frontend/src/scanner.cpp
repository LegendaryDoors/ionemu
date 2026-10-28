#include "scanner.h"
#include "app.h"
#include "cover_delegate.h"
#include "cover_generator.h"
#include "ioncore/ioncore.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

Scanner::Scanner(QStringList folders, QHash<QString, GameEntry> cache, QString coversDir, QObject *parent)
    : QThread(parent), m_folders(std::move(folders)), m_cache(std::move(cache)), m_coversDir(std::move(coversDir)) {}

// Generated cover (covers/generated/<key>.png) unless the user supplied one or it exists already. About a second per
// disc the first time; rendered from the user's own disc, never shipped.
void Scanner::ensureCover(const GameEntry &g) {
    if (m_coversDir.isEmpty()) return;
    const QString key = CoverDelegate::coverKey(g.sha1, g.title);
    if (CoverDelegate::userCoverFile(m_coversDir, g.sha1, g.title).isEmpty() == false) return;
    const QString file = m_coversDir + "/generated/" + key + ".png";
    if (QFile::exists(file)) return;
    if (covers::generate(g.path, g.sha1, g.entries, g.title, file)) emit coverReady(key);
}

// FILE "name" BINARY lines of a cue sheet, resolved against the sheet's folder.
static QSet<QString> cueReferences(const QString &cuePath) {
    QSet<QString> refs;
    QFile f(cuePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return refs;
    static const QRegularExpression re(R"re(^\s*FILE\s+"?([^"]+?)"?\s+\w+\s*$)re", QRegularExpression::CaseInsensitiveOption);
    const QDir dir = QFileInfo(cuePath).dir();
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        const auto m = re.match(line);
        if (m.hasMatch()) refs.insert(QFileInfo(dir, m.captured(1)).absoluteFilePath());
    }
    return refs;
}

QStringList Scanner::candidates(const QStringList &folders, int *archivesSkipped) {
    static const QStringList imageExt = {"cue", "mdx", "bin", "img", "iso"};
    static const QStringList archiveExt = {"zip", "7z", "rar"};
    QStringList files;
    QSet<QString> claimed;   // .bin files owned by a .cue
    int archives = 0;
    for (const QString &folder : folders) {
        QDirIterator it(folder, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            const QFileInfo fi = it.fileInfo();
            if (fi.isDir()) {                                   // a directory of extracted programs counts as a disc
                if (!QDir(p).entryList({"*.mem.bin"}, QDir::Files).isEmpty()) files << fi.absoluteFilePath();
                continue;
            }
            const QString ext = fi.suffix().toLower();
            if (archiveExt.contains(ext)) { archives++; continue; }
            if (!imageExt.contains(ext)) continue;
            if (fi.dir().dirName().isEmpty()) continue;
            if (ext == "cue") claimed.unite(cueReferences(fi.absoluteFilePath()));
            files << fi.absoluteFilePath();
        }
    }
    QStringList out;
    const QSet<QString> all(files.begin(), files.end());
    for (const QString &f : files) {
        const QFileInfo fi(f);
        const QString ext = fi.suffix().toLower();
        if (ext != "cue" && claimed.contains(f)) continue;
        // a damaged dump that has been repaired (Repair Damaged Disc...) is superseded by the copy next to it
        const QString repaired = fi.dir().filePath(fi.completeBaseName() + " (repaired)." + fi.suffix());
        if (!fi.isDir() && !fi.completeBaseName().endsWith(" (repaired)") && all.contains(repaired)) continue;
        if (!out.contains(f)) out << f;
    }
    if (archivesSkipped) *archivesSkipped = archives;
    return out;
}

bool Scanner::identify(const QString &path, GameEntry &out) {
    const ion::DiscInfo info = ion::identify_disc(path.toStdString());
    if (!info.ok || info.entries <= 0) return false;
    out.path = path;
    out.sha1 = QString::fromStdString(info.sha1);
    out.known = !info.title.empty();
    out.title = out.known ? QString::fromStdString(info.title) : QFileInfo(path).completeBaseName();
    out.entries = info.entries;
    out.damaged = info.bad_checksums > 0 || info.missing_sectors > 0;
    return true;
}

void Scanner::run() {
    int archives = 0;
    const QStringList files = candidates(m_folders, &archives);
    int nFound = 0, skipped = 0, nDone = 0;
    for (const QString &path : files) {
        if (m_cancel) break;
        emit progress(nDone++, files.size(), path);
        const QFileInfo fi(path);
        const qint64 size = fi.isDir() ? 0 : fi.size(), mtime = fi.lastModified().toSecsSinceEpoch();
        const auto cached = m_cache.constFind(path);
        // Cached results are reused for unchanged files, except unidentified ones: a newer identification table may
        // know them now, and re-checking costs ~0.3 s each.
        if (cached != m_cache.constEnd() && cached->size == size && cached->mtime == mtime && cached->known) {
            emit found(*cached);
            nFound++;
            if (!m_cancel) ensureCover(*cached);
            continue;
        }
        GameEntry g;
        if (!identify(path, g)) { skipped++; continue; }   // not an iON data track (e.g. a loose audio-track .bin)
        g.size = size; g.mtime = mtime;
        emit found(g);
        nFound++;
        if (!m_cancel) ensureCover(g);
    }
    emit done(nFound, skipped, archives, files);
}
