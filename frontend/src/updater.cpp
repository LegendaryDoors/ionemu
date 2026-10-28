#include "updater.h"
#include "app.h"
#include "icons.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDate>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QRegularExpression>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#if __has_include(<QtCore/private/qzipreader_p.h>)
#include <QtCore/private/qzipreader_p.h>
#else
#include <QtGui/private/qzipreader_p.h>   // Qt 6.5
#endif

static QNetworkRequest githubRequest(const QUrl &url) {
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", QString("%1/%2").arg(app::kId, app::kVersion).toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);
    return req;
}

static QString megabytes(qint64 bytes) { return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1); }

// ---------------------------------------------------------------------------------------------------------------

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent) {}

void UpdateChecker::check() {
    QNetworkReply *reply = m_net.get(githubRequest(QUrl(QString("https://api.github.com/repos/%1/releases/latest").arg(app::kRepo))));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        ReleaseInfo info;
        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            info.error = status == 404 ? "no release has been published yet" : reply->errorString();
        } else {
            info = parse(reply->readAll());
        }
        emit finished(info);
    });
}

void UpdateChecker::checkFromFile(const QString &path) {
    QFile f(path);
    ReleaseInfo info;
    if (f.open(QIODevice::ReadOnly)) info = parse(f.readAll());
    else info.error = "cannot read " + path;
    emit finished(info);
}

ReleaseInfo UpdateChecker::parse(const QByteArray &json) {
    ReleaseInfo r;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (!doc.isObject()) { r.error = "unexpected reply: " + err.errorString(); return r; }
    const QJsonObject o = doc.object();
    QString tag = o.value("tag_name").toString();
    if (tag.isEmpty()) { r.error = o.value("message").toString("no release in the reply"); return r; }
    if (tag.startsWith('v') || tag.startsWith('V')) tag.remove(0, 1);
    r.version = tag;
    r.date = o.value("published_at").toString().left(10);
    r.url = o.value("html_url").toString();
    r.notes = o.value("body").toString();
    for (const QJsonValue &v : o.value("assets").toArray()) {
        const QJsonObject a = v.toObject();
        r.assets.push_back({a.value("name").toString(), a.value("browser_download_url").toString(), qint64(a.value("size").toDouble())});
    }
    r.ok = true;
    return r;
}

int UpdateChecker::compareVersions(const QString &a, const QString &b) {
    const QStringList pa = a.split('.'), pb = b.split('.');
    for (int i = 0; i < std::max(pa.size(), pb.size()); i++) {
        const int x = i < pa.size() ? pa[i].toInt() : 0, y = i < pb.size() ? pb[i].toInt() : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

ReleaseInfo::Asset UpdateChecker::pickAsset(const ReleaseInfo &r) {
    for (const ReleaseInfo::Asset &a : r.assets) {
        const QString n = a.name.toLower();
#if defined(Q_OS_WIN)
        if (n.contains("windows") && n.endsWith(".zip")) return a;
#elif defined(Q_OS_MACOS)
        if (n.contains("macos") && n.endsWith(".zip")) return a;
#else
        if (n.endsWith(".appimage")) return a;
#endif
    }
    return {};
}

QString UpdateChecker::appImagePath() {
#ifdef Q_OS_LINUX
    const QString p = qEnvironmentVariable("APPIMAGE");
    if (!p.isEmpty() && QFileInfo(p).isFile()) return p;
#endif
    return {};
}

QString UpdateChecker::bundlePath() {
#ifdef Q_OS_MACOS
    QDir d(QCoreApplication::applicationDirPath());   // <name>.app/Contents/MacOS
    if (d.cdUp() && d.cdUp() && d.absolutePath().endsWith(".app")) return d.absolutePath();
#endif
    return {};
}

QString UpdateChecker::prettyDate(const QString &iso) {
    const QDate d = QDate::fromString(iso.left(10), Qt::ISODate);
    return d.isValid() ? QLocale::system().toString(d, QLocale::LongFormat) : iso;
}

QString UpdateChecker::ageText(const QString &fromIso, const QString &toIso) {
    const QDate from = QDate::fromString(fromIso.left(10), Qt::ISODate), to = QDate::fromString(toIso.left(10), Qt::ISODate);
    if (!from.isValid() || !to.isValid()) return {};
    const qint64 days = from.daysTo(to);
    if (days < 1) return {};
    if (days == 1) return "one day";
    if (days < 14) return QString("%1 days").arg(days);
    if (days < 60) { const int w = int((days + 3) / 7); return w == 1 ? "one week" : QString("%1 weeks").arg(w); }
    if (days < 700) { const int m = int((days + 15) / 30.44); return m == 1 ? "one month" : QString("%1 months").arg(m); }
    const int y = int((days + 182) / 365.25); return y == 1 ? "one year" : QString("%1 years").arg(y);
}

// Release notes come as GitHub markdown, where a single line break is a line break (as on the release page); plain
// markdown would fold those lines into one paragraph, so they get hard breaks, and the document gets some air.
static QString notesMarkdown(QString body, const QString &version) {
    body.replace("\r\n", "\n");
    QStringList out;
    QStringList lines = body.split('\n');
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst();
    if (!lines.isEmpty()) {                                        // a leading heading that only names the version repeats the frame's title
        const QString first = lines.first().trimmed();
        if (first.startsWith('#') && first.mid(first.lastIndexOf('#') + 1).trimmed().remove(app::kName).trimmed().remove(QChar('v')) == version) lines.removeFirst();
    }
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        const bool block = t.isEmpty() || t.startsWith('#') || t.startsWith("- ") || t.startsWith("* ") || t.startsWith("+ ") || t.startsWith('>') ||
                           t.startsWith("```") || t.startsWith('|') || QRegularExpression("^\\d+[.)] ").match(t).hasMatch();
        out.push_back(block || line.endsWith("  ") ? line : line + "  ");
    }
    return out.join('\n');
}

void UpdateChecker::cleanupLeftovers() {
#if defined(Q_OS_WIN)
    const QString dir = QCoreApplication::applicationDirPath();
    QDirIterator it(dir, {"*.old"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) QFile::remove(it.next());        // the previous exe/DLLs; a file still in use stays for next time
    QDir(dir + "/update-staging").removeRecursively();
#elif defined(Q_OS_MACOS)
    const QString bundle = bundlePath();
    if (!bundle.isEmpty()) { QDir(bundle + ".old").removeRecursively(); QDir(QFileInfo(bundle).path() + "/.ionemu-update").removeRecursively(); }
#else
    const QString self = appImagePath();
    if (!self.isEmpty()) { QFile::remove(self + ".old"); QFile::remove(self + ".new"); }
#endif
}

// ---------------------------------------------------------------------------------------------------------------

UpdateDialog::UpdateDialog(const ReleaseInfo &info, QWidget *parent) : QDialog(parent), m_info(info) {
    setWindowTitle("Update Available");
    setMinimumWidth(600);
    m_asset = UpdateChecker::pickAsset(info);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    m_canInstall = !m_asset.name.isEmpty() && (
#ifdef Q_OS_MACOS
        !UpdateChecker::bundlePath().isEmpty()
#else
        true
#endif
    );
#else
    m_canInstall = !m_asset.name.isEmpty() && !UpdateChecker::appImagePath().isEmpty();
#endif

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(18, 18, 18, 14);
    v->setSpacing(12);

    // header: the orb and "Update Available"
    auto *head = new QHBoxLayout;
    head->setSpacing(14);
    auto *logo = new QLabel(this);
    logo->setPixmap(icons::coloured("app", 56, QColor()));
    logo->setFixedSize(56, 56);
    head->addWidget(logo, 0, Qt::AlignTop);
    auto *headText = new QVBoxLayout;
    headText->setSpacing(3);
    auto *title = new QLabel("Update Available", this);
    QFont tf = title->font(); tf.setPointSizeF(tf.pointSizeF() * 1.5); tf.setBold(true); title->setFont(tf);
    headText->addWidget(title);
    const QString age = UpdateChecker::ageText(app::kReleaseDate, info.date);
    auto *ask = new QLabel(age.isEmpty() ? QString("A newer version is available. Would you like to update?")
                                         : QString("Your version is <b>%1 out of date</b>. Would you like to update?").arg(age), this);
    ask->setTextFormat(Qt::RichText);
    headText->addWidget(ask);
    headText->addWidget(new QLabel(QString("The latest version, %1 %2, was released on %3.").arg(app::kName, info.version, UpdateChecker::prettyDate(info.date)), this));
    head->addLayout(headText, 1);
    v->addLayout(head);

    // the facts, as PCSX2 lists them
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(6);
    form->setContentsMargins(0, 2, 0, 2);
    auto bold = [this](const QString &t) { auto *l = new QLabel(t, this); l->setTextFormat(Qt::RichText); return l; };
    form->addRow("Current version:", bold(QString("<b>%1</b> (released %2)").arg(app::kVersion, UpdateChecker::prettyDate(app::kReleaseDate))));
    form->addRow("Latest version:", bold(QString("<b>%1</b> (released %2)").arg(info.version, UpdateChecker::prettyDate(info.date))));
    if (!m_asset.name.isEmpty()) form->addRow("Download size:", bold(QString("%1 (%2)").arg(megabytes(m_asset.size), m_asset.name)));
    else form->addRow("Download:", bold("no package for this platform in the release; the release page opens in your browser"));
    v->addLayout(form);

    auto *changes = new QGroupBox(QString("Changes in %1").arg(info.version), this);
    auto *cv = new QVBoxLayout(changes);
    cv->setContentsMargins(8, 10, 8, 8);
    auto *notes = new QTextBrowser(changes);
    notes->setOpenExternalLinks(true);
    notes->setFrameShape(QFrame::NoFrame);
    notes->document()->setDefaultStyleSheet("p { margin-top: 0px; margin-bottom: 8px; } li { margin-bottom: 4px; } h1, h2, h3 { margin-bottom: 6px; }");
    notes->setMarkdown(info.notes.isEmpty() ? QString("(no notes; see the release page)") : notesMarkdown(info.notes, info.version));
    notes->document()->setDocumentMargin(8);
    notes->setMinimumHeight(190);
    cv->addWidget(notes);
    v->addWidget(changes, 1);

    // progress and result, hidden until the download starts
    auto *progressRow = new QHBoxLayout;
    progressRow->setSpacing(10);
    m_statusIcon = new QLabel(this);
    m_statusIcon->setFixedSize(28, 28);
    m_statusIcon->hide();
    progressRow->addWidget(m_statusIcon, 0, Qt::AlignVCenter);
    auto *progressCol = new QVBoxLayout;
    progressCol->setSpacing(4);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->hide();
    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setTextVisible(true);
    m_bar->hide();
    progressCol->addWidget(m_status);
    progressCol->addWidget(m_bar);
    progressRow->addLayout(progressCol, 1);
    v->addLayout(progressRow);

    auto *buttons = new QHBoxLayout;
    m_skip = new QPushButton("Skip This Version", this);
    m_later = new QPushButton("Remind Me Later", this);
    m_get = new QPushButton(m_canInstall ? "Download and Install" : "Open Download Page", this);
    m_get->setIcon(icons::get(m_canInstall ? "download" : "info"));
    m_get->setDefault(true);
    buttons->addWidget(m_skip); buttons->addStretch(1); buttons->addWidget(m_later); buttons->addWidget(m_get);
    v->addLayout(buttons);
    connect(m_later, &QPushButton::clicked, this, [this] { m_choice = Later; reject(); });
    connect(m_skip, &QPushButton::clicked, this, [this] { m_choice = Skip; accept(); });
    connect(m_get, &QPushButton::clicked, this, [this] {
        if (m_reply) { m_reply->abort(); return; }                                          // the button reads Cancel while downloading
        if (!m_canInstall) { QDesktopServices::openUrl(QUrl(m_asset.url.isEmpty() ? m_info.url : m_asset.url)); m_choice = Later; accept(); return; }
        startDownload();
    });
}

void UpdateDialog::devAutoInstall(int afterMs) { QTimer::singleShot(afterMs, this, [this] { m_get->click(); }); }

void UpdateDialog::setBusy(bool busy) {
    m_skip->setEnabled(!busy);
    m_later->setEnabled(!busy);
    m_get->setText(busy ? "Cancel" : (m_canInstall ? "Download and Install" : "Open Download Page"));
    m_get->setIcon(busy ? QIcon() : icons::get(m_canInstall ? "download" : "info"));
}

void UpdateDialog::startDownload() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_downloadPath = dir + "/" + m_asset.name;
    m_file.setFileName(m_downloadPath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { finishError(QString("Cannot write %1: %2").arg(m_downloadPath, m_file.errorString())); return; }
    m_statusIcon->hide();
    m_status->setText(QString("Downloading %1...").arg(m_asset.name));
    m_status->setStyleSheet(QString());
    m_status->show();
    m_bar->setValue(0);
    m_bar->show();
    setBusy(true);
    m_reply = m_net.get(githubRequest(QUrl(m_asset.url)));
    connect(m_reply, &QNetworkReply::readyRead, this, [this] { m_file.write(m_reply->readAll()); });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        const qint64 size = total > 0 ? total : m_asset.size;
        if (size > 0) m_bar->setValue(int(got * 100 / size));
        m_status->setText(QString("Downloading %1: %2 of %3").arg(m_asset.name, megabytes(got), megabytes(size)));
    });
    connect(m_reply, &QNetworkReply::finished, this, &UpdateDialog::onDownloadFinished);
}

void UpdateDialog::onDownloadFinished() {
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    m_file.write(reply->readAll());
    m_file.close();
    const bool cancelled = reply->error() == QNetworkReply::OperationCanceledError;
    const bool ok = reply->error() == QNetworkReply::NoError && (m_asset.size <= 0 || m_file.size() == m_asset.size);
    const QString err = reply->errorString();
    reply->deleteLater();
    if (!ok) {
        QFile::remove(m_downloadPath);
        if (cancelled) { m_status->hide(); m_bar->hide(); setBusy(false); return; }
        finishError(QString("The download failed: %1").arg(err));
        return;
    }
    m_bar->setValue(100);
    m_status->setText("Installing...");
    QApplication::processEvents();
    install();
}

void UpdateDialog::install() {
    QString error;
    bool ok = false;
#if defined(Q_OS_WIN)
    ok = installZipWindows(&error);
#elif defined(Q_OS_MACOS)
    ok = installZipMac(&error);
#else
    ok = installAppImage(&error);
#endif
    QFile::remove(m_downloadPath);
    if (ok) finishOk(); else finishError(error);
}

// Linux: the new AppImage goes next to the running one, then the files swap; the old one stays until the swap is done.
bool UpdateDialog::installAppImage(QString *error) {
    const QString target = UpdateChecker::appImagePath();
    const QString tmp = target + ".new", old = target + ".old";
    QFile::remove(tmp);
    if (!QFile::copy(m_downloadPath, tmp)) { *error = QString("Cannot write next to %1 (is the folder writable?).").arg(target); return false; }
    QFile::setPermissions(tmp, QFile::permissions(target) | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    QFile::remove(old);
    if (!QFile::rename(target, old) || !QFile::rename(tmp, target)) {
        if (QFile::exists(old) && !QFile::exists(target)) QFile::rename(old, target);
        *error = QString("Could not replace %1.").arg(target);
        return false;
    }
    QFile::remove(old);
    return true;
}

// Windows: unpack the zip into update-staging, then move every file into place; the files in use (exe, DLLs) are
// renamed *.old first, which Windows allows, and the next start deletes them.
bool UpdateDialog::installZipWindows(QString *error) {
    const QString dir = QCoreApplication::applicationDirPath();
    const QString staging = dir + "/update-staging";
    QDir(staging).removeRecursively();
    QZipReader zip(m_downloadPath);
    if (!zip.isReadable() || !zip.extractAll(staging)) { *error = QString("Could not unpack %1 into %2.").arg(m_asset.name, dir); return false; }
    QString src;
    {
        QDirIterator it(staging, {QFileInfo(QCoreApplication::applicationFilePath()).fileName()}, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) src = QFileInfo(it.next()).absolutePath();
    }
    if (src.isEmpty()) { QDir(staging).removeRecursively(); *error = "The downloaded package does not contain the launcher."; return false; }
    QDirIterator files(src, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString f = files.next();
        const QString rel = QDir(src).relativeFilePath(f), dest = dir + "/" + rel;
        QDir().mkpath(QFileInfo(dest).path());
        if (QFile::exists(dest)) {
            const QString old = dest + ".old";
            QFile::remove(old);
            if (!QFile::rename(dest, old)) { *error = QString("Could not replace %1.").arg(rel); return false; }
        }
        if (!QFile::rename(f, dest)) { *error = QString("Could not place %1.").arg(rel); return false; }
    }
    QDir(staging).removeRecursively();
    return true;
}

// macOS: unpack next to the bundle (same volume, so renames are atomic), swap the bundles, keep the old one as
// <name>.app.old until the next start.
bool UpdateDialog::installZipMac(QString *error) {
    const QString bundle = UpdateChecker::bundlePath();
    const QString staging = QFileInfo(bundle).path() + "/.ionemu-update";
    QDir(staging).removeRecursively();
    QZipReader zip(m_downloadPath);
    if (!zip.isReadable() || !zip.extractAll(staging)) { *error = QString("Could not unpack %1 next to %2 (is the folder writable?).").arg(m_asset.name, bundle); return false; }
    QString newApp;
    {
        QDirIterator it(staging, {"*.app"}, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        if (it.hasNext()) newApp = it.next();
    }
    if (newApp.isEmpty() || !QFileInfo(newApp + "/Contents/MacOS").isDir()) { QDir(staging).removeRecursively(); *error = "The downloaded package does not contain the app."; return false; }
    const QString old = bundle + ".old";
    QDir(old).removeRecursively();
    if (!QDir().rename(bundle, old)) { *error = QString("Could not move %1 aside (is the folder writable?).").arg(bundle); return false; }
    if (!QDir().rename(newApp, bundle)) { QDir().rename(old, bundle); *error = "Could not put the new app in place."; return false; }
    QDir(staging).removeRecursively();
    return true;
}

void UpdateDialog::finishOk() {
    m_choice = Installed;
    m_bar->hide();
    m_statusIcon->setPixmap(icons::coloured("circle-check", 28, QColor(72, 199, 116)));
    m_statusIcon->show();
    m_status->setText(QString("<b>%1 %2 is installed.</b> Restarting...").arg(app::kName, m_info.version));
    m_status->setTextFormat(Qt::RichText);
    m_get->setEnabled(false); m_skip->setEnabled(false); m_later->setEnabled(false);
    QTimer::singleShot(1500, this, [this] {
#if defined(Q_OS_WIN)
        QProcess::startDetached(QCoreApplication::applicationFilePath(), {}, QCoreApplication::applicationDirPath());
#elif defined(Q_OS_MACOS)
        QProcess::startDetached("/usr/bin/open", {"-n", UpdateChecker::bundlePath()});
#else
        QProcess::startDetached(UpdateChecker::appImagePath(), {});
#endif
        accept();
        QApplication::quit();
    });
}

void UpdateDialog::finishError(const QString &message) {
    m_bar->hide();
    m_statusIcon->setPixmap(icons::coloured("circle-x", 28, QColor(224, 82, 82)));
    m_statusIcon->show();
    m_status->setTextFormat(Qt::PlainText);
    m_status->setText(message + " You can download the release from its page instead.");
    m_status->show();
    m_canInstall = false;   // the button becomes Open Download Page
    setBusy(false);
}
