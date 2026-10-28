// Update check and one-click install, modelled on PCSX2's automatic updater: once a day at start (General page:
// off switch) and from Help > Check for Updates, the launcher asks the project's GitHub releases for the latest
// version. The only thing sent is the version in the User-Agent header. "Download and Install" fetches the release
// for this platform with a progress bar, swaps it in place of the running copy and restarts:
//   Linux    the AppImage file is replaced ($APPIMAGE);
//   Windows  the release zip is unpacked next to the running files, which are renamed *.old (a running exe may be
//            renamed, not overwritten) and deleted at the next start;
//   macOS    the release zip's .app replaces the bundle, which is kept as *.app.old until the next start.
#pragma once
#include <QDialog>
#include <QFile>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

class QLabel;
class QNetworkReply;
class QProgressBar;
class QPushButton;

struct ReleaseInfo {
    struct Asset { QString name, url; qint64 size = 0; };
    bool ok = false;
    QString error;
    QString version;          // "0.2.0" (the tag without its leading "v")
    QString date;             // ISO date of the release
    QString url;              // the release page
    QString notes;            // the release body (markdown)
    QList<Asset> assets;
};

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);
    void check();                                      // GET https://api.github.com/repos/<repo>/releases/latest
    void checkFromFile(const QString &path);           // development: a saved response
    static ReleaseInfo parse(const QByteArray &json);
    static int compareVersions(const QString &a, const QString &b);   // <0, 0, >0 for dotted numbers
    static ReleaseInfo::Asset pickAsset(const ReleaseInfo &r);        // the installable download for this platform (empty name if none)
    static QString appImagePath();                                    // the running AppImage, or empty
    static QString bundlePath();                                      // the running .app on macOS, or empty
    static QString prettyDate(const QString &iso);                     // in the user's locale, long form
    static QString ageText(const QString &fromIso, const QString &toIso);   // "3 days", "2 weeks", "4 months", "a year"; empty if not older
    static void cleanupLeftovers();                                   // delete what the previous update left behind (call at start)

signals:
    void finished(const ReleaseInfo &info);

private:
    QNetworkAccessManager m_net;
};

// "Update Available": the versions with their dates, the size, the notes, and Download and Install / Skip This
// Version / Remind Me Later. The download and the install happen inside the dialog with a progress bar; on success
// it shows a green check and restarts the launcher.
class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    enum Choice { Later = 0, Skip = 1, Installed = 2 };
    UpdateDialog(const ReleaseInfo &info, QWidget *parent = nullptr);
    Choice choice() const { return m_choice; }
    void devAutoInstall(int afterMs);        // development: press Download and Install by itself

private:
    void startDownload();
    void onDownloadFinished();
    void install();
    bool installAppImage(QString *error);
    bool installZipWindows(QString *error);
    bool installZipMac(QString *error);
    void finishOk();
    void finishError(const QString &message);
    void setBusy(bool busy);
    ReleaseInfo m_info;
    ReleaseInfo::Asset m_asset;
    bool m_canInstall = false;
    QNetworkAccessManager m_net;
    QNetworkReply *m_reply = nullptr;
    QFile m_file;                       // the download in progress
    QString m_downloadPath;
    QLabel *m_status = nullptr, *m_statusIcon = nullptr;
    QProgressBar *m_bar = nullptr;
    QPushButton *m_get = nullptr, *m_skip = nullptr, *m_later = nullptr;
    Choice m_choice = Later;
};
