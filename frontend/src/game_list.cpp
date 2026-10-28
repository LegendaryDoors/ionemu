#include "game_list.h"
#include "app.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>

QString GameEntry::saveName() const {
    QString base = title.isEmpty() ? QFileInfo(path).completeBaseName() : title;
    if (!sha1.isEmpty()) base += " [" + sha1.left(8) + "]";
    return app::safeFileName(base);
}

GameListModel::GameListModel(QObject *parent) : QAbstractTableModel(parent) { qRegisterMetaType<GameEntry>(); }

QVariant GameListModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char *names[ColCount] = {"Title", "File", "Entries", "Status"};
    return section >= 0 && section < ColCount ? QString(names[section]) : QString();
}

int GameListModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_games.size(); }

QVariant GameListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_games.size()) return {};
    const GameEntry &g = m_games[index.row()];
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case ColFile: return QFileInfo(g.path).fileName();
        case ColEntries: return g.entries;
        case ColStatus: return g.damaged ? QStringLiteral("Damaged (repair offered on start)") : !g.known ? QStringLiteral("Unknown disc") : QStringLiteral("OK");
        default: return g.title;
        }
    case Qt::TextAlignmentRole: return index.column() == ColEntries ? QVariant(int(Qt::AlignRight | Qt::AlignVCenter)) : QVariant();
    case Qt::ToolTipRole: {
        QString tip = g.title + "\n" + g.path;
        if (!g.sha1.isEmpty()) tip += "\nSHA-1 " + g.sha1;
        if (!g.known) tip += "\nUnknown disc (not in the identification table)";
        if (g.damaged) tip += "\nDamaged dump: some sectors were read with errors. Starting it offers a repair.";
        return tip;
    }
    case PathRole: return g.path;
    case Sha1Role: return g.sha1;
    case KnownRole: return g.known;
    case EntriesRole: return g.entries;
    case DamagedRole: return g.damaged;
    default: return {};
    }
}

const GameEntry *GameListModel::entryAt(int row) const { return row >= 0 && row < m_games.size() ? &m_games[row] : nullptr; }

int GameListModel::rowOf(const QString &path) const {
    for (int i = 0; i < m_games.size(); i++) if (m_games[i].path == path) return i;
    return -1;
}

static bool titleLess(const GameEntry &a, const GameEntry &b) {
    const int c = QString::compare(a.title, b.title, Qt::CaseInsensitive);
    return c != 0 ? c < 0 : a.path < b.path;
}

void GameListModel::addOrReplace(const GameEntry &e) {
    const int old = rowOf(e.path);
    if (old >= 0) {
        beginRemoveRows({}, old, old);
        m_games.remove(old);
        endRemoveRows();
    }
    const auto it = std::lower_bound(m_games.begin(), m_games.end(), e, titleLess);
    const int row = int(it - m_games.begin());
    beginInsertRows({}, row, row);
    m_games.insert(row, e);
    endInsertRows();
}

void GameListModel::retainOnly(const QSet<QString> &paths) {
    for (int i = m_games.size() - 1; i >= 0; i--)
        if (!paths.contains(m_games[i].path)) {
            beginRemoveRows({}, i, i);
            m_games.remove(i);
            endRemoveRows();
        }
}

void GameListModel::clear() {
    if (m_games.isEmpty()) return;
    beginResetModel();
    m_games.clear();
    endResetModel();
}

QHash<QString, GameEntry> GameListModel::byPath() const {
    QHash<QString, GameEntry> h;
    for (const GameEntry &g : m_games) h.insert(g.path, g);
    return h;
}

bool GameListModel::loadCache(const QString &file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return false;
    beginResetModel();
    m_games.clear();
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        GameEntry g;
        g.path = o["path"].toString();
        g.title = o["title"].toString();
        g.sha1 = o["sha1"].toString();
        g.entries = o["entries"].toInt();
        g.size = qint64(o["size"].toDouble());
        g.mtime = qint64(o["mtime"].toDouble());
        g.known = o["known"].toBool();
        g.damaged = o["damaged"].toBool();
        if (!g.path.isEmpty()) m_games.push_back(g);
    }
    std::sort(m_games.begin(), m_games.end(), titleLess);
    endResetModel();
    return true;
}

bool GameListModel::saveCache(const QString &file) const {
    QJsonArray arr;
    for (const GameEntry &g : m_games) {
        QJsonObject o;
        o["path"] = g.path; o["title"] = g.title; o["sha1"] = g.sha1; o["entries"] = g.entries;
        o["size"] = double(g.size); o["mtime"] = double(g.mtime); o["known"] = g.known; o["damaged"] = g.damaged;
        arr.push_back(o);
    }
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return true;
}
