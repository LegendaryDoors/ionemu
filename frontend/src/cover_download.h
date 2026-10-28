// Download Covers... (PCSX2-style): URL templates with ${sha1} / ${title} / ${file} placeholders, plus an optional
// index URL (a JSON object mapping SHA-1 -> image URL) so users need not find sources themselves once the project
// hosts one. Images are saved as covers/<sha1>.png; the app itself ships no artwork.
#pragma once
#include "game_list.h"
#include <QDialog>
#include <QList>
#include <QNetworkAccessManager>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QNetworkReply;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

class CoverDownloadDialog : public QDialog {
    Q_OBJECT
public:
    CoverDownloadDialog(QList<GameEntry> games, QString coversDir, QStringList templates, QString indexUrl, QWidget *parent = nullptr);
    QStringList templates() const;
    QString indexUrl() const;

signals:
    void coverReady(const QString &key);   // a cover file was written for this SHA-1 (or title key)

private:
    void start();
    void fetchIndex();
    void nextGame();
    void tryNextUrl();
    void onReply(QNetworkReply *reply);
    void finish(const QString &summary);
    static QString expand(const QString &tmpl, const GameEntry &g);

    QList<GameEntry> m_games;
    QString m_coversDir;
    QNetworkAccessManager m_nam;
    QPlainTextEdit *m_templates;
    QLineEdit *m_index;
    QCheckBox *m_replace;
    QPushButton *m_start, *m_close;
    QLabel *m_status;
    QProgressBar *m_progress;
    QHash<QString, QString> m_indexMap;   // sha1 -> url
    int m_pos = -1, m_done = 0, m_found = 0;
    QStringList m_urls;                   // remaining urls for the current game
    QNetworkReply *m_reply = nullptr;
    bool m_running = false, m_cancel = false;
};
