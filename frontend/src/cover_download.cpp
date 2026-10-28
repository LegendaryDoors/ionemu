#include "cover_download.h"
#include "app.h"
#include "cover_delegate.h"
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

CoverDownloadDialog::CoverDownloadDialog(QList<GameEntry> games, QString coversDir, QStringList templates, QString indexUrl, QWidget *parent)
    : QDialog(parent), m_games(std::move(games)), m_coversDir(std::move(coversDir)) {
    setWindowTitle("Download Covers");
    resize(640, 480);
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel("Fetches one cover per disc from the sources below, replacing the generated cards. iONemu ships no artwork, "
                             "so the sources are yours to choose.", this);
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto *idx = new QGroupBox("Cover index (optional)", this);
    auto *iv = new QVBoxLayout(idx);
    m_index = new QLineEdit(indexUrl, idx);
    m_index->setPlaceholderText("https://.../covers/index.json");
    iv->addWidget(m_index);
    auto *inote = new QLabel("A JSON file mapping each disc's SHA-1 to an image URL.", idx);
    inote->setWordWrap(true);
    inote->setStyleSheet("color: palette(placeholder-text);");
    iv->addWidget(inote);
    v->addWidget(idx);
    auto *tpl = new QGroupBox("URL templates (one per line, tried in order)", this);
    auto *tv = new QVBoxLayout(tpl);
    m_templates = new QPlainTextEdit(tpl);
    m_templates->setPlainText(templates.join("\n"));
    m_templates->setPlaceholderText("https://example.org/ion-covers/${sha1}.png\nhttps://example.org/ion-covers/${title}.jpg");
    tv->addWidget(m_templates);
    auto *tnote = new QLabel("Placeholders: ${sha1} (the disc's SHA-1), ${title} (the title, URL-encoded), ${file} (the image file's base name).", tpl);
    tnote->setWordWrap(true);
    tnote->setStyleSheet("color: palette(placeholder-text);");
    tv->addWidget(tnote);
    v->addWidget(tpl, 1);
    m_replace = new QCheckBox("Replace covers that were downloaded before", this);
    v->addWidget(m_replace);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, qMax(1, m_games.size()));
    m_progress->setValue(0);
    v->addWidget(m_progress);
    m_status = new QLabel(QString("%1 disc%2 in the library.").arg(m_games.size()).arg(m_games.size() == 1 ? "" : "s"), this);
    v->addWidget(m_status);
    auto *h = new QHBoxLayout;
    h->addStretch(1);
    m_start = new QPushButton("Start", this);
    m_close = new QPushButton("Close", this);
    h->addWidget(m_start);
    h->addWidget(m_close);
    v->addLayout(h);
    connect(m_start, &QPushButton::clicked, this, &CoverDownloadDialog::start);
    connect(m_close, &QPushButton::clicked, this, [this] { if (m_running) { m_cancel = true; if (m_reply) m_reply->abort(); } else reject(); });
    connect(&m_nam, &QNetworkAccessManager::finished, this, &CoverDownloadDialog::onReply);
}

QStringList CoverDownloadDialog::templates() const {
    QStringList out;
    for (const QString &l : m_templates->toPlainText().split('\n')) if (!l.trimmed().isEmpty()) out << l.trimmed();
    return out;
}

QString CoverDownloadDialog::indexUrl() const { return m_index->text().trimmed(); }

QString CoverDownloadDialog::expand(const QString &tmpl, const GameEntry &g) {
    QString s = tmpl;
    s.replace("${sha1}", g.sha1);
    s.replace("${title}", QString::fromUtf8(QUrl::toPercentEncoding(g.title)));
    s.replace("${file}", QString::fromUtf8(QUrl::toPercentEncoding(QFileInfo(g.path).completeBaseName())));
    return s;
}

void CoverDownloadDialog::start() {
    if (m_running) return;
    if (templates().isEmpty() && indexUrl().isEmpty()) { m_status->setText("Add an index URL or at least one template first."); return; }
    m_running = true; m_cancel = false; m_done = 0; m_found = 0; m_pos = -1;
    m_indexMap.clear();
    m_start->setEnabled(false);
    m_close->setText("Cancel");
    m_progress->setValue(0);
    if (!indexUrl().isEmpty()) fetchIndex(); else nextGame();
}

void CoverDownloadDialog::fetchIndex() {
    m_status->setText("Fetching the cover index...");
    QNetworkRequest req{QUrl(indexUrl())};
    req.setTransferTimeout(15000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam.get(req);
    m_reply->setProperty("index", true);
}

void CoverDownloadDialog::nextGame() {
    if (m_cancel) { finish("Cancelled."); return; }
    while (++m_pos < m_games.size()) {
        const GameEntry &g = m_games[m_pos];
        const QString key = CoverDelegate::coverKey(g.sha1, g.title);
        const QString file = m_coversDir + "/" + key + ".png";
        if (QFile::exists(file) && !m_replace->isChecked()) { m_progress->setValue(m_pos + 1); continue; }
        m_urls.clear();
        if (!g.sha1.isEmpty() && m_indexMap.contains(g.sha1)) m_urls << m_indexMap.value(g.sha1);
        for (const QString &t : templates()) m_urls << expand(t, g);
        if (m_urls.isEmpty()) { m_progress->setValue(m_pos + 1); continue; }
        m_status->setText("Looking for a cover: " + g.title);
        tryNextUrl();
        return;
    }
    finish(QString("Done: %1 cover%2 downloaded, %3 disc%4 without one.").arg(m_found).arg(m_found == 1 ? "" : "s").arg(m_done - m_found).arg(m_done - m_found == 1 ? "" : "s"));
}

void CoverDownloadDialog::tryNextUrl() {
    if (m_cancel) { finish("Cancelled."); return; }
    if (m_urls.isEmpty()) { m_done++; m_progress->setValue(m_pos + 1); nextGame(); return; }
    QNetworkRequest req{QUrl(m_urls.takeFirst())};
    req.setTransferTimeout(20000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam.get(req);
}

void CoverDownloadDialog::onReply(QNetworkReply *reply) {
    reply->deleteLater();
    if (reply != m_reply) return;
    m_reply = nullptr;
    if (reply->property("index").toBool()) {
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonObject map = o.contains("covers") ? o["covers"].toObject() : o;
            for (auto it = map.begin(); it != map.end(); ++it) if (it.value().isString()) m_indexMap.insert(it.key().toLower(), it.value().toString());
            m_status->setText(QString("Index: %1 entries.").arg(m_indexMap.size()));
        } else {
            m_status->setText("Index not available (" + reply->errorString() + "); using the templates only.");
        }
        nextGame();
        return;
    }
    if (reply->error() == QNetworkReply::NoError) {
        const QByteArray data = reply->readAll();
        const QImage img = QImage::fromData(data);
        if (!img.isNull()) {
            const GameEntry &g = m_games[m_pos];
            const QString key = CoverDelegate::coverKey(g.sha1, g.title);
            QDir().mkpath(m_coversDir);
            if (img.save(m_coversDir + "/" + key + ".png", "PNG")) {
                m_found++; m_done++;
                m_progress->setValue(m_pos + 1);
                emit coverReady(key);
                nextGame();
                return;
            }
        }
    }
    tryNextUrl();
}

void CoverDownloadDialog::finish(const QString &summary) {
    m_running = false;
    m_status->setText(summary);
    m_start->setEnabled(true);
    m_close->setText("Close");
}
