// iONemu entry point. usage: ionemu [disc] [--entry N] [--fullscreen] [--grab FILE --grab-after MS]
#include "app.h"
#include "camera_source.h"
#include "cover_generator.h"
#include "settings.h"
#include <vector>
#include "ioncore/ioncore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include "game_list.h"
#include "main_window.h"
#include "updater.h"
#include "settings_dialog.h"
#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QNetworkProxyFactory>
#include <QTimer>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// PCSX2-style "Dark Fusion" look, identical on every platform.
void applyDarkFusion(QApplication &a) {
    a.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor window(45, 45, 48), base(30, 30, 33), text(228, 228, 228), accent(42, 130, 218), disabled(120, 120, 124);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, QColor(60, 60, 64));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(58, 58, 62));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, disabled);
    for (QPalette::ColorRole r : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText, QPalette::HighlightedText})
        p.setColor(QPalette::Disabled, r, disabled);
    a.setPalette(p);
}

}  // namespace

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {   // a GUI subsystem exe: show --help/--list-cameras output in the calling console
        FILE *f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
    QApplication a(argc, argv);
    QMediaDevices::defaultAudioOutput();          // create Qt Multimedia's device monitor on the GUI thread, not the emu thread
    QNetworkProxyFactory::setUseSystemConfiguration(true);   // update check and cover downloads follow the system proxy
    UpdateChecker::cleanupLeftovers();                        // what the last Download and Install left behind
    a.setApplicationName(app::kId);                // no organisation: ~/.config/ionemu and ~/.local/share/ionemu
    // No applicationDisplayName: Qt would append the name to every window and dialog title with an em dash
    // separator on Linux/Windows. The main window sets its own title.
    a.setApplicationVersion(app::kVersion);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    qRegisterMetaType<GameEntry>();
    applyDarkFusion(a);
    a.setWindowIcon(QIcon(":/branding/icon.svg"));

    QCommandLineParser cli;
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addPositionalArgument("disc", "disc image to start (.cue / .mdx / .bin) or an extracted directory");
    cli.addOption({"entry", "directory entry to boot (default 1)", "N", "1"});
    cli.addOption({"fullscreen", "start in fullscreen"});
    cli.addOption({"add-folder", "add a disc folder and scan it", "DIR"});
    cli.addOption({"list-cameras", "print the camera devices Qt sees and exit"});
    auto addDev = [&cli](QCommandLineOption o) { o.setFlags(QCommandLineOption::HiddenFromHelp); cli.addOption(o); };   // for tests and screenshots
    addDev({"render-cover", "development: render the generated cover of the disc argument to FILE and exit", "FILE"});
    addDev({"plain", "development: with --render-cover, no title inside the card"});
    addDev({"render-player", "development: render the stand-in player (default look, or player/* from the settings) to FILE", "FILE"});
    addDev({"grab", "development: save a screenshot of the window to FILE and exit", "FILE"});
    addDev({"grab-after", "development: milliseconds to wait before --grab", "MS", "1500"});
    addDev({"grab-menu", "development: open the pause menu before --grab"});
    addDev({"grab-settings", "development: open the settings dialog on page N before --grab", "N"});
    addDev({"grab-hold", "development: milliseconds between opening the menu/dialog and --grab", "MS", "600"});
    addDev({"restart-after", "development: start the disc argument again after MS (tests a restart while running)", "MS"});
    addDev({"repair-test", "development: run Repair Damaged Disc... on FILE with the library as donors", "FILE"});
    addDev({"offer-test", "development: show the damaged-disc prompt for FILE", "FILE"});
    addDev({"grab-dialog", "development: open a dialog before --grab: welcome, licence, covers, about, update", "NAME"});
    addDev({"update-json", "development: show the update dialog for a saved GitHub release reply", "FILE"});
    addDev({"update-install", "development: with --update-json, press Download and Install by itself"});
    addDev({"player-hand", "development: with --grab-settings 4, move the stand-in hand to X,Y and then a little further before the grab", "X,Y"});
    cli.process(a);

    if (cli.isSet("list-cameras")) {
        for (const CameraSource::DeviceInfo &d : CameraSource::devices()) printf("%s\t%s%s\n", qPrintable(d.id), qPrintable(d.description), d.isDefault ? " (default)" : "");
        return 0;
    }
    if (cli.isSet("render-player")) {
        const Settings st = Settings::load();
        std::vector<uint32_t> px(size_t(ion::SCREEN_W) * ion::SCREEN_H);
        ion::render_player(st.player, 210, 96, true, px.data());
        for (uint32_t &v : px) v |= 0xff000000u;
        const QImage img(reinterpret_cast<const uchar *>(px.data()), ion::SCREEN_W, ion::SCREEN_H, ion::SCREEN_W * 4, QImage::Format_RGB32);
        return img.copy().save(cli.value("render-player"), "PNG") ? 0 : 2;
    }
    if (cli.isSet("render-cover")) {
        const QStringList pa = cli.positionalArguments();
        if (pa.isEmpty()) { fprintf(stderr, "--render-cover needs a disc argument\n"); return 1; }
        const ion::DiscInfo info = ion::identify_disc(pa.first().toStdString());
        const QString title = info.title.empty() ? QFileInfo(pa.first()).completeBaseName() : QString::fromStdString(info.title);
        const bool ok = covers::generate(pa.first(), QString::fromStdString(info.sha1), info.entries, title, cli.value("render-cover"), !cli.isSet("plain"));
        fprintf(stderr, "%s\n", ok ? "cover written" : "could not render the disc");
        return ok ? 0 : 2;
    }
    MainWindow w;
    if (cli.isSet("add-folder")) w.addFolder(cli.value("add-folder"));
    w.show();
    if (cli.isSet("fullscreen")) w.setFullscreen(true);
    if (cli.isSet("repair-test")) QTimer::singleShot(500, &w, [&] { w.devRepair(cli.value("repair-test"), false); });
    if (cli.isSet("offer-test")) QTimer::singleShot(500, &w, [&] { w.devRepair(cli.value("offer-test"), true); });
    if (cli.isSet("grab-dialog")) QTimer::singleShot(800, &w, [&] { w.devDialog(cli.value("grab-dialog")); });
    if (cli.isSet("update-json")) QTimer::singleShot(800, &w, [&] { w.devUpdateFromFile(cli.value("update-json"), cli.isSet("update-install")); });
    const QStringList args = cli.positionalArguments();
    if (!args.isEmpty()) w.startDisc(args.first(), cli.value("entry").toInt());
    if (cli.isSet("restart-after") && !args.isEmpty())
        QTimer::singleShot(cli.value("restart-after").toInt(), &w, [&, args] { w.startDisc(args.first(), cli.value("entry").toInt()); });
    if (cli.isSet("grab")) {
        const QString file = cli.value("grab");
        const bool menu = cli.isSet("grab-menu");
        const int settingsPage = cli.isSet("grab-settings") ? cli.value("grab-settings").toInt() : -1;
        QTimer::singleShot(cli.value("grab-after").toInt(), &w, [&, file, menu, settingsPage] {
            if (menu) w.openPauseMenu();
            if (settingsPage >= 0) w.openSettings(settingsPage);
            QTimer::singleShot(cli.value("grab-hold").toInt(), &w, [&, file, settingsPage] {
                QWidget *target = QApplication::activeModalWidget();   // a dialog being exec()ed
                if (!target && settingsPage >= 0) for (QWidget *top : QApplication::topLevelWidgets()) if (top->isVisible() && top != &w && top->isWindow()) target = top;
                if (!target) target = &w;
                if (cli.isSet("player-hand"))
                    if (auto *dlg = qobject_cast<SettingsDialog *>(target)) {
                        const QStringList xy = cli.value("player-hand").split(',');
                        if (xy.size() == 2) { dlg->devHand(xy[0].toInt(), xy[1].toInt()); dlg->devHand(xy[0].toInt() + 24, xy[1].toInt() - 16); }
                    }
                target->grab().save(file);
                a.quit();
            });
        });
    }
    return a.exec();
}

#ifdef Q_OS_WIN
// GUI-subsystem entry point (WIN32_EXECUTABLE) without Qt's entry-point library, which does not link with a UCRT MinGW.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
