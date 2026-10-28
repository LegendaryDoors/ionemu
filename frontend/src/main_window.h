// Main window, PCSX2-style: menu bar + toolbar, a cover grid, and the emulation view that replaces the grid while a
// game runs; a status bar with the emulation, audio and camera state. Owns the settings, the emulation thread and the
// shared camera source.
#pragma once
#include "game_list.h"
#include "settings.h"
#include "updater.h"
#include <QMainWindow>
#include <QPointer>
#include <QStringList>

class CameraSource;
class CoverDelegate;
class DisplayWidget;
class EmuThread;
class QAction;
class QActionGroup;
class QLabel;
class QListView;
class QMenu;
class QPushButton;
class QStackedWidget;
class QTableView;
class QToolBar;
class Scanner;
class SettingsDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void startDisc(const QString &path, int entry = 1);
    void addFolder(const QString &folder);
    void setFullscreen(bool on);
    bool gameRunning() const;
    void openPauseMenu();          // what Esc does in the game view
    void openSettings(int page);   // SettingsDialog::Page
    void devRepair(const QString &path, bool offerOnly);   // development: run the damaged-disc flow on `path`
    void devDialog(const QString &name);                   // development: open welcome / licence / covers / about
    void devUpdateFromFile(const QString &path, bool autoInstall = false);   // development: the update dialog from a saved GitHub reply

protected:
    void closeEvent(QCloseEvent *) override;
    bool event(QEvent *e) override;

private:
    enum Page { GridPage = 0, GamePage = 1 };

    void buildActions();
    void buildMenus();
    void buildToolbar();
    void buildStatusBar();
    void buildPages();
    void applySettings(const Settings &s);
    void applyShortcuts();
    void applyCamera(const QString &deviceId);
    void updateActions();
    void updateEmptyState();
    void updateCameraLabel();
    void setStateText(const QString &text);

    void rescan();
    void onScanFound(const GameEntry &entry);
    void onScanProgress(int done, int total, const QString &path);
    void onScanDone(int found, int skipped, int archives, const QStringList &seen);

    void startSelected(int entry = 1);
    bool offerRepair(const GameEntry &g, int entry);   // damaged disc: Repair / Play anyway / Cancel; true = handled
    void repairDisc(const GameEntry &g, int entryAfter);
    void startFromEntryDialog();
    void togglePause();
    void resetGame();
    void stopGame();
    void onEmuStarted(const QString &title);
    void onEmuFailed(const QString &error);
    void onEmuFinished();
    void resumeGame();

    void chooseFolder();
    void showWelcome();
    void showLicence();
    void downloadCovers();
    void setListView(bool list);
    void gameContextMenu(const QPoint &globalPos, const QModelIndex &idx);
    void openDiscFile();
    void showAbout();
    void checkForUpdates(bool manual);
    void onUpdateResult(const ReleaseInfo &info, bool manual);
    void setFilter(int filter);
    void nextFilter();
    void setGainDb(double db);
    void setMute(bool mute);
    void setOverlay(bool on);
    void toggleCamera();
    void setCurrentSlot(int slot);

    Settings m_settings;
    EmuThread *m_emu = nullptr;
    CameraSource *m_camera = nullptr;
    GameListModel *m_model = nullptr;
    CoverDelegate *m_delegate = nullptr;
    QPointer<Scanner> m_scanner;
    QPointer<SettingsDialog> m_dialog;
    bool m_repairing = false;

    QStackedWidget *m_pages = nullptr;
    QListView *m_grid = nullptr;
    QTableView *m_table = nullptr;
    QWidget *m_empty = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QPushButton *m_emptyButton = nullptr;
    DisplayWidget *m_display = nullptr;
    QToolBar *m_toolbar = nullptr;
    QLabel *m_stateLabel = nullptr, *m_fpsLabel = nullptr, *m_audioLabel = nullptr, *m_cameraLabel = nullptr;

    QAction *m_actAddFolder = nullptr, *m_actOpenDisc = nullptr, *m_actRescan = nullptr, *m_actExit = nullptr;
    QAction *m_actStart = nullptr, *m_actPause = nullptr, *m_actReset = nullptr, *m_actStop = nullptr, *m_actFastForward = nullptr;
    QAction *m_actFullscreen = nullptr, *m_actMute = nullptr, *m_actGainUp = nullptr, *m_actGainDown = nullptr;
    QAction *m_actNextFilter = nullptr, *m_actSaveState = nullptr, *m_actLoadState = nullptr, *m_actAbout = nullptr;
    QAction *m_actOverlay = nullptr, *m_actScreenshot = nullptr, *m_actCamera = nullptr, *m_actCameraSetup = nullptr, *m_actSettings = nullptr;
    QAction *m_actListView = nullptr, *m_actShowTitles = nullptr, *m_actDownloadCovers = nullptr, *m_actLicence = nullptr, *m_actWelcome = nullptr;
    QAction *m_actCheckUpdates = nullptr;
    UpdateChecker *m_updates = nullptr;
    QActionGroup *m_filterGroup = nullptr, *m_slotGroup = nullptr;
    QMenu *m_saveSlotMenu = nullptr, *m_loadSlotMenu = nullptr;

    QString m_cameraBeforeDialog;   // runtime device when the dialog opened; Cancel goes back to it
    int m_currentSlot = 1, m_movingCells = -1;
    bool m_wasMaximized = false;
    bool m_menuPause = false;   // paused by the Esc menu (as opposed to the toolbar)
    bool m_autoPaused = false;  // paused because the window lost focus
};
