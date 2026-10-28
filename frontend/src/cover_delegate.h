// Grid item: a cover (a user-supplied PNG from the covers folder, else a generated placeholder) with the title below.
#pragma once
#include <QHash>
#include <QPixmap>
#include <QStyledItemDelegate>

class CoverDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    static constexpr int kCoverW = 180, kCoverH = 252, kTextH = 64, kPad = 10;   // three lines of the larger label

    explicit CoverDelegate(QObject *parent = nullptr);
    void setCoversDir(const QString &dir);
    void setShowTitles(bool on) { m_showTitles = on; }
    bool showTitles() const { return m_showTitles; }
    void clearCache() { m_cache.clear(); }
    void invalidate(const QString &key) { m_cache.remove(key); }
    static QString coverKey(const QString &sha1, const QString &title);                      // cache/file key
    static QString userCoverFile(const QString &dir, const QString &sha1, const QString &title);   // "" if none

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    QPixmap coverFor(const QString &sha1, const QString &title) const;   // user PNG or null
    static void paintPlaceholder(QPainter *p, const QRect &r, const QString &title);
    QString m_coversDir;
    bool m_showTitles = false;
    mutable QHash<QString, QPixmap> m_cache;
};
