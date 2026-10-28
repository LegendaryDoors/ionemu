// Background disc scan: walks the configured folders, reuses cached results for unchanged files and identifies the
// rest with ion::identify_disc (SHA-1 of the logical image, ~0.3 s per disc).
#pragma once
#include "game_list.h"
#include <QHash>
#include <QStringList>
#include <QThread>
#include <atomic>

class Scanner : public QThread {
    Q_OBJECT
public:
    Scanner(QStringList folders, QHash<QString, GameEntry> cache, QString coversDir, QObject *parent = nullptr);
    void cancel() { m_cancel = true; }

    // Files the scanner considers disc images; .bin files referenced by a .cue in the same folder are skipped.
    static QStringList candidates(const QStringList &folders, int *archivesSkipped = nullptr);
    static bool identify(const QString &path, GameEntry &out);   // fills title/sha1/entries/known/damaged

signals:
    void found(const GameEntry &entry);
    void coverReady(const QString &key);   // a generated cover was written for this disc (SHA-1, or the save name)
    void progress(int done, int total, const QString &path);
    void done(int found, int skipped, int archives, const QStringList &seen);   // seen = every candidate path

protected:
    void run() override;

private:
    void ensureCover(const GameEntry &g);
    QStringList m_folders;
    QHash<QString, GameEntry> m_cache;
    QString m_coversDir;
    std::atomic<bool> m_cancel{false};
};
