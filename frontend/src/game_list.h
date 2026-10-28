// The disc list shown in the grid: entries found by the scanner, cached in a JSON file keyed by path + size + mtime.
#pragma once
#include <QAbstractTableModel>
#include <QHash>
#include <QMetaType>
#include <QString>
#include <QVector>

struct GameEntry {
    QString path;        // .cue / .bin / .mdx image, or a directory of extracted programs
    QString title;       // known title from the SHA-1 table, or a name derived from the file name
    QString sha1;        // logical-image SHA-1 ("" for an extracted directory without logical.bin)
    int entries = 0;     // loadable directory entries
    qint64 size = 0, mtime = 0;   // of `path`, to reuse cached results
    bool known = false;  // identified by SHA-1
    bool damaged = false;// bad checksums or missing sectors were reported

    QString saveName() const;   // file-system-safe base name for this disc's save files
};
Q_DECLARE_METATYPE(GameEntry)

class GameListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Roles { PathRole = Qt::UserRole + 1, Sha1Role, KnownRole, EntriesRole, DamagedRole };
    enum Columns { ColTitle = 0, ColFile, ColEntries, ColStatus, ColCount };
    explicit GameListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : ColCount; }
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    const GameEntry *entryAt(int row) const;
    int rowOf(const QString &path) const;
    void addOrReplace(const GameEntry &e);          // keeps the list sorted by title
    void retainOnly(const QSet<QString> &paths);    // drop entries no longer on disk
    void clear();
    QHash<QString, GameEntry> byPath() const;

    bool loadCache(const QString &file);
    bool saveCache(const QString &file) const;

private:
    QVector<GameEntry> m_games;
};
