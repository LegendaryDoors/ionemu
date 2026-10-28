#include "main_window.h"
#include "app.h"
#include "camera_source.h"
#include "cover_delegate.h"
#include "display_widget.h"
#include "emu_thread.h"
#include "scanner.h"
#include "settings_dialog.h"
#include "cover_download.h"
#include "icons.h"
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTableView>
#include <QTextStream>
#include <QPainter>
#include <QSvgRenderer>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QThread>
#include "ioncore/ioncore.h"
#include <QPushButton>
#include <QSettings>
#include <QDateTime>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QToolBar>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QString("%1 - %2").arg(app::kName, app::kTagline));
    resize(1100, 760);
    m_settings = Settings::load();
    m_emu = new EmuThread(this);
    m_camera = new CameraSource(this);
    m_emu->setCameraSource(m_camera);
    m_model = new GameListModel(this);
    m_delegate = new CoverDelegate(this);
    m_delegate->setCoversDir(app::dataDir() + "/covers");
    QDir().mkpath(app::dataDir() + "/covers");

    buildActions();
    buildMenus();
    buildToolbar();
    buildPages();
    buildStatusBar();

    connect(m_emu, &EmuThread::started, this, &MainWindow::onEmuStarted);
    connect(m_emu, &EmuThread::failed, this, &MainWindow::onEmuFailed);
    connect(m_emu, &EmuThread::finished, this, &MainWindow::onEmuFinished);
    connect(m_emu, &EmuThread::message, this, [this](const QString &t) { statusBar()->showMessage(t, 4000); });
    connect(m_emu, &EmuThread::stats, this, [this](double fps, const QString &audio, int moving) {
        m_fpsLabel->setText(QString("%1 fps").arg(fps, 0, 'f', 0));
        m_audioLabel->setText(audio);
        m_movingCells = moving;
        updateCameraLabel();
    });
    connect(m_camera, &CameraSource::stateChanged, this, [this](bool active) {
        if (!active) { m_emu->setCameraEnabled(false); m_display->setCameraOn(false); m_actCamera->setChecked(false); }   // unplugged or failed
        updateCameraLabel();
    });
    connect(m_camera, &CameraSource::errorOccurred, this, [this](const QString &msg) { statusBar()->showMessage("Camera: " + msg, 6000); updateCameraLabel(); });

    QSettings qs(app::settingsFile(), QSettings::IniFormat);
    restoreGeometry(qs.value("window/geometry").toByteArray());
    restoreState(qs.value("window/state").toByteArray());
    applySettings(m_settings);
    m_model->loadCache(app::cacheFile());
    updateEmptyState();
    updateActions();
    if (!m_settings.discFolders.isEmpty()) rescan();
    else if (!m_settings.welcomeShown) QTimer::singleShot(0, this, &MainWindow::showWelcome);
    // The daily update check, a few seconds after the window is up so it never delays the start.
    if (m_settings.checkUpdates) {
        const QDateTime last = QDateTime::fromString(m_settings.lastUpdateCheck, Qt::ISODate);
        if (!last.isValid() || last.secsTo(QDateTime::currentDateTime()) > 20 * 3600) QTimer::singleShot(4000, this, [this] { checkForUpdates(false); });
    }
}

void MainWindow::checkForUpdates(bool manual) {
    if (!m_updates) m_updates = new UpdateChecker(this);
    auto *checker = m_updates;
    disconnect(checker, nullptr, this, nullptr);
    connect(checker, &UpdateChecker::finished, this, [this, manual](const ReleaseInfo &info) { onUpdateResult(info, manual); });
    if (manual) statusBar()->showMessage("Checking for updates...", 4000);
    checker->check();
}

void MainWindow::onUpdateResult(const ReleaseInfo &info, bool manual) {
    if (!manual) { m_settings.lastUpdateCheck = QDateTime::currentDateTime().toString(Qt::ISODate); m_settings.save(); }
    if (!info.ok) {
        if (manual) QMessageBox::warning(this, "Check for Updates", QString("Could not check for updates: %1.").arg(info.error));
        return;
    }
    const bool newer = UpdateChecker::compareVersions(info.version, app::kVersion) > 0;
    if (!newer) {
        if (manual) QMessageBox::information(this, "Check for Updates", QString("%1 %2 is the latest version.").arg(app::kName, app::kVersion));
        return;
    }
    if (!manual && info.version == m_settings.skippedVersion) return;
    if (gameRunning() && !manual) { m_settings.lastUpdateCheck.clear(); m_settings.save(); return; }   // never over a game; ask again next start
    UpdateDialog dlg(info, this);
    dlg.exec();
    if (dlg.choice() == UpdateDialog::Skip) { m_settings.skippedVersion = info.version; m_settings.save(); }
}

void MainWindow::devUpdateFromFile(const QString &path, bool autoInstall) {
    if (!m_updates) m_updates = new UpdateChecker(this);
    connect(m_updates, &UpdateChecker::finished, this, [this, autoInstall](const ReleaseInfo &info) {
        if (!info.ok) { QMessageBox::warning(this, "Check for Updates", info.error); return; }
        UpdateDialog dlg(info, this);
        if (autoInstall) dlg.devAutoInstall(700);
        dlg.exec();
    });
    m_updates->checkFromFile(path);
}

MainWindow::~MainWindow() {
    if (m_scanner) { m_scanner->cancel(); m_scanner->wait(); }
    m_emu->stopAndWait();
    m_camera->disconnect(this);      // children are deleted after this body; nothing may call back into us
    m_camera->stop();
}

// ---- construction ---------------------------------------------------------------------------------------------

void MainWindow::buildActions() {
    m_actAddFolder = new QAction(icons::get("folder-plus"), "Add Disc Folder...", this);
    m_actOpenDisc = new QAction("Open Disc...", this);
    m_actOpenDisc->setShortcut(QKeySequence::Open);
    m_actRescan = new QAction(icons::get("refresh-cw"), "Rescan Folders", this);
    m_actExit = new QAction("Exit", this);
    m_actExit->setShortcut(QKeySequence::Quit);

    m_actStart = new QAction(icons::get("play"), "Start", this);
    m_actPause = new QAction(icons::get("pause"), "Pause", this);
    m_actPause->setCheckable(true);
    m_actReset = new QAction(icons::get("rotate-ccw"), "Reset", this);
    m_actStop = new QAction(icons::get("square"), "Stop", this);
    m_actFastForward = new QAction(icons::get("fast-forward"), "Fast Forward", this);
    m_actFastForward->setCheckable(true);
    m_actFastForward->setToolTip("Run as fast as possible, without sound");
    m_actFullscreen = new QAction(icons::get("maximize"), "Fullscreen", this);
    m_actFullscreen->setCheckable(true);
    m_actCameraSetup = new QAction(icons::get("camera"), "Camera Setup", this);
    m_actSettings = new QAction(icons::get("settings"), "Settings", this);
    m_actListView = new QAction(icons::get("list"), "List View", this);
    m_actListView->setCheckable(true);
    m_actShowTitles = new QAction("Show Titles Under Covers", this);
    m_actShowTitles->setCheckable(true);
    m_actDownloadCovers = new QAction(icons::get("image-down"), "Download Covers...", this);
    m_actLicence = new QAction("Licence and Notices...", this);
    m_actWelcome = new QAction("Getting Started...", this);
    m_actCheckUpdates = new QAction("Check for Updates...", this);

    m_actMute = new QAction("Mute", this);
    m_actMute->setCheckable(true);
    m_actGainUp = new QAction("Volume Up", this);
    m_actGainDown = new QAction("Volume Down", this);
    m_actNextFilter = new QAction("Next Video Filter", this);
    m_actSaveState = new QAction("Save State (current slot)", this);
    m_actLoadState = new QAction("Load State (current slot)", this);
    m_actOverlay = new QAction("Motion Overlay", this);
    m_actOverlay->setCheckable(true);
    m_actScreenshot = new QAction("Screenshot", this);
    m_actCamera = new QAction("Camera", this);
    m_actCamera->setCheckable(true);
    m_actAbout = new QAction(icons::get("info"), "About...", this);

    m_filterGroup = new QActionGroup(this);
    for (const char *name : {"Composite (TV-like)", "Sharp", "Smooth"}) m_filterGroup->addAction(name)->setCheckable(true);
    m_filterGroup->actions()[0]->setChecked(true);
    m_slotGroup = new QActionGroup(this);
    for (int i = 1; i <= 5; i++) {
        QAction *a = m_slotGroup->addAction(QString("Slot %1").arg(i));
        a->setCheckable(true);
        a->setData(i);
    }
    m_slotGroup->actions()[0]->setChecked(true);

    connect(m_actAddFolder, &QAction::triggered, this, &MainWindow::chooseFolder);
    connect(m_actOpenDisc, &QAction::triggered, this, &MainWindow::openDiscFile);
    connect(m_actRescan, &QAction::triggered, this, &MainWindow::rescan);
    connect(m_actExit, &QAction::triggered, this, &QWidget::close);
    connect(m_actStart, &QAction::triggered, this, [this] { startSelected(1); });
    connect(m_actPause, &QAction::triggered, this, &MainWindow::togglePause);
    connect(m_actReset, &QAction::triggered, this, &MainWindow::resetGame);
    connect(m_actStop, &QAction::triggered, this, &MainWindow::stopGame);
    connect(m_actFastForward, &QAction::triggered, this, [this](bool on) {
        m_emu->setFastForwardToggle(on);
        statusBar()->showMessage(on ? "Fast forward on" : "Fast forward off", 2000);
    });
    connect(m_actFullscreen, &QAction::triggered, this, [this](bool on) { setFullscreen(on); });
    connect(m_actCameraSetup, &QAction::triggered, this, [this] { openSettings(SettingsDialog::Camera); });
    connect(m_actSettings, &QAction::triggered, this, [this] { openSettings(SettingsDialog::General); });
    connect(m_actMute, &QAction::triggered, this, &MainWindow::setMute);
    connect(m_actGainUp, &QAction::triggered, this, [this] { setGainDb(m_settings.gainDb + 1); });
    connect(m_actGainDown, &QAction::triggered, this, [this] { setGainDb(m_settings.gainDb - 1); });
    connect(m_actNextFilter, &QAction::triggered, this, &MainWindow::nextFilter);
    connect(m_filterGroup, &QActionGroup::triggered, this, [this](QAction *a) { setFilter(m_filterGroup->actions().indexOf(a)); });
    connect(m_slotGroup, &QActionGroup::triggered, this, [this](QAction *a) { setCurrentSlot(a->data().toInt()); });
    connect(m_actSaveState, &QAction::triggered, this, [this] { if (gameRunning()) m_emu->requestSaveState(m_currentSlot); });
    connect(m_actLoadState, &QAction::triggered, this, [this] { if (gameRunning()) m_emu->requestLoadState(m_currentSlot); });
    connect(m_actOverlay, &QAction::triggered, this, &MainWindow::setOverlay);
    connect(m_actScreenshot, &QAction::triggered, this, [this] { if (gameRunning()) m_emu->requestScreenshot(); });
    connect(m_actCamera, &QAction::triggered, this, &MainWindow::toggleCamera);
    connect(m_actAbout, &QAction::triggered, this, &MainWindow::showAbout);
    connect(m_actListView, &QAction::triggered, this, &MainWindow::setListView);
    connect(m_actShowTitles, &QAction::triggered, this, [this](bool on) {
        m_settings.showTitles = on;
        m_settings.save();
        m_delegate->setShowTitles(on);
        m_grid->reset();                               // item sizes changed
    });
    connect(m_actDownloadCovers, &QAction::triggered, this, &MainWindow::downloadCovers);
    connect(m_actLicence, &QAction::triggered, this, &MainWindow::showLicence);
    connect(m_actWelcome, &QAction::triggered, this, &MainWindow::showWelcome);
    connect(m_actCheckUpdates, &QAction::triggered, this, [this] { checkForUpdates(true); });
    // Shortcuts need a visible associated widget: the menu bar and toolbar are hidden in fullscreen, so the window
    // itself holds the hotkey actions too.
    addActions({m_actFullscreen, m_actNextFilter, m_actMute, m_actGainUp, m_actGainDown, m_actSaveState, m_actLoadState,
                m_actOverlay, m_actScreenshot, m_actCamera, m_actOpenDisc, m_actExit});
    for (QAction *a : {m_actFullscreen, m_actNextFilter, m_actMute, m_actSaveState, m_actLoadState, m_actOverlay, m_actScreenshot, m_actCamera})
        a->setAutoRepeat(false);                   // holding F5 must not rewrite the state file every repeat
}

void MainWindow::buildMenus() {
    QMenu *file = menuBar()->addMenu("&File");
    file->addAction(m_actAddFolder);
    file->addAction(m_actOpenDisc);
    file->addAction(m_actRescan);
    file->addSeparator();
    file->addAction(m_actExit);

    QMenu *emu = menuBar()->addMenu("&Emulation");
    emu->addAction(m_actStart);
    emu->addAction(m_actPause);
    emu->addAction(m_actReset);
    emu->addAction(m_actStop);
    emu->addAction(m_actFastForward);
    emu->addSeparator();
    m_saveSlotMenu = emu->addMenu("Save State");
    m_loadSlotMenu = emu->addMenu("Load State");
    for (int i = 1; i <= 5; i++) {
        m_saveSlotMenu->addAction(QString("Slot %1").arg(i), this, [this, i] { if (gameRunning()) m_emu->requestSaveState(i); });
        m_loadSlotMenu->addAction(QString("Slot %1").arg(i), this, [this, i] { if (gameRunning()) m_emu->requestLoadState(i); });
    }
    emu->addMenu("Current Slot")->addActions(m_slotGroup->actions());
    emu->addAction(m_actSaveState);
    emu->addAction(m_actLoadState);
    emu->addSeparator();
    emu->addAction(m_actScreenshot);

    QMenu *view = menuBar()->addMenu("&View");
    view->addAction(m_actListView);
    view->addAction(m_actShowTitles);
    view->addAction(m_actFullscreen);
    QMenu *filter = view->addMenu("Video Filter");
    filter->addActions(m_filterGroup->actions());
    filter->addSeparator();
    filter->addAction(m_actNextFilter);
    view->addSeparator();
    view->addAction(m_actOverlay);

    QMenu *settings = menuBar()->addMenu("&Settings");
    const char *pages[] = {"General...", "Graphics...", "Audio...", "Camera...", "Player...", "Controls...", "Advanced..."};
    for (int i = 0; i < 7; i++) settings->addAction(pages[i], this, [this, i] { openSettings(i); });
    settings->addSeparator();
    settings->addAction(m_actDownloadCovers);
    settings->addSeparator();
    settings->addAction(m_actCamera);
    settings->addAction(m_actMute);
    settings->addAction(m_actGainUp);
    settings->addAction(m_actGainDown);

    QMenu *help = menuBar()->addMenu("&Help");
    help->addAction(m_actWelcome);
    help->addAction("iONemu Wiki", this, [] { QDesktopServices::openUrl(QUrl(app::webUrl("/wiki"))); });
    help->addAction("Where to Find the Games", this, [] { QDesktopServices::openUrl(QUrl(app::webUrl("/wiki/Finding-The-Games"))); });
    help->addSeparator();
    help->addAction(m_actCheckUpdates);
    help->addAction(m_actLicence);
    help->addAction(m_actAbout);
}

void MainWindow::buildToolbar() {
    m_toolbar = addToolBar("Main");
    m_toolbar->setObjectName("mainToolbar");
    m_toolbar->setMovable(false);
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_toolbar->setIconSize({24, 24});
    m_toolbar->addAction(m_actStart);
    m_toolbar->addAction(m_actPause);
    m_toolbar->addAction(m_actReset);
    m_toolbar->addAction(m_actStop);
    m_toolbar->addAction(m_actFastForward);
    m_toolbar->addSeparator();
    m_toolbar->addAction(m_actFullscreen);
    m_toolbar->addSeparator();
    m_toolbar->addAction(m_actCameraSetup);
    m_toolbar->addAction(m_actSettings);
    m_toolbar->addSeparator();
    m_toolbar->addAction(m_actAddFolder);
    m_toolbar->addAction(m_actRescan);
    m_toolbar->addAction(m_actListView);
}

void MainWindow::buildStatusBar() {
    m_stateLabel = new QLabel("Stopped", this);
    m_fpsLabel = new QLabel(this);
    m_audioLabel = new QLabel(this);
    m_cameraLabel = new QLabel(this);
    for (QLabel *l : {m_stateLabel, m_fpsLabel, m_audioLabel, m_cameraLabel}) {
        l->setContentsMargins(8, 0, 8, 0);
        statusBar()->addPermanentWidget(l);
    }
    updateCameraLabel();
}

void MainWindow::buildPages() {
    m_pages = new QStackedWidget(this);
    setCentralWidget(m_pages);

    auto *gridPage = new QWidget(this);
    auto *v = new QVBoxLayout(gridPage);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_grid = new QListView(gridPage);
    m_grid->setModel(m_model);
    m_grid->setItemDelegate(m_delegate);
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setFlow(QListView::LeftToRight);
    m_grid->setWrapping(true);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setSpacing(12);
    m_grid->setUniformItemSizes(true);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setFrameShape(QFrame::NoFrame);
    m_grid->setMouseTracking(true);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_grid, &QListView::activated, this, [this] { startSelected(1); });
    connect(m_grid, &QListView::customContextMenuRequested, this, [this](const QPoint &pos) {
        gameContextMenu(m_grid->viewport()->mapToGlobal(pos), m_grid->indexAt(pos));
    });
    connect(m_grid->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::updateActions);
    v->addWidget(m_grid, 1);

    m_table = new QTableView(gridPage);                    // the list view: same model and selection as the grid
    m_table->setModel(m_model);
    m_table->setSelectionModel(m_grid->selectionModel());
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(GameListModel::ColTitle, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(GameListModel::ColFile, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(GameListModel::ColEntries, QHeaderView::ResizeToContents);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::activated, this, [this] { startSelected(1); });
    connect(m_table, &QTableView::customContextMenuRequested, this, [this](const QPoint &pos) {
        gameContextMenu(m_table->viewport()->mapToGlobal(pos), m_table->indexAt(pos));
    });
    m_table->hide();
    v->addWidget(m_table, 1);

    m_empty = new QWidget(gridPage);
    auto *ev = new QVBoxLayout(m_empty);
    ev->addStretch(2);
    m_emptyLabel = new QLabel(m_empty);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    QFont f = m_emptyLabel->font(); f.setPointSizeF(f.pointSizeF() * 1.2); m_emptyLabel->setFont(f);
    ev->addWidget(m_emptyLabel);
    m_emptyButton = new QPushButton("Add Disc Folder...", m_empty);
    m_emptyButton->setMinimumSize(200, 36);
    ev->addWidget(m_emptyButton, 0, Qt::AlignHCenter);
    auto *findGames = new QLabel(QString("<a href=\"%1\">No disc images? Where to find the games</a>").arg(app::webUrl("/wiki/Finding-The-Games")), m_empty);
    findGames->setOpenExternalLinks(true);
    findGames->setAlignment(Qt::AlignCenter);
    ev->addSpacing(12);
    ev->addWidget(findGames);
    ev->addStretch(3);
    connect(m_emptyButton, &QPushButton::clicked, this, [this] { if (m_settings.discFolders.isEmpty()) chooseFolder(); else rescan(); });
    v->addWidget(m_empty, 1);
    m_pages->addWidget(gridPage);

    m_display = new DisplayWidget(m_emu, this);
    connect(m_display, &DisplayWidget::escapePressed, this, &MainWindow::openPauseMenu);
    connect(m_display, &DisplayWidget::fullscreenRequested, this, [this] { setFullscreen(!isFullScreen()); });
    connect(m_display->pauseMenu(), &PauseMenu::resumeClicked, this, &MainWindow::resumeGame);
    connect(m_display->pauseMenu(), &PauseMenu::resetClicked, this, [this] { resumeGame(); resetGame(); });
    connect(m_display->pauseMenu(), &PauseMenu::stopClicked, this, &MainWindow::stopGame);
    connect(m_display->pauseMenu(), &PauseMenu::fullscreenClicked, this, [this] { setFullscreen(!isFullScreen()); });
    m_pages->addWidget(m_display);
    m_pages->setCurrentIndex(GridPage);
}

// ---- settings -------------------------------------------------------------------------------------------------

void MainWindow::applySettings(const Settings &s) {
    const bool foldersChanged = s.discFolders != m_settings.discFolders;
    const QString oldCamera = m_settings.cameraId;
    m_settings = s;
    m_settings.save();
    // emulation
    m_emu->setGainDb(s.gainDb);
    m_emu->setMute(s.mute);
    m_emu->setFilter(qBound(0, s.filter, 2));
    m_emu->setAudioBufferMs(s.bufferMs);
    m_emu->setAdpcm36Model(s.adpcm36Model);
    m_emu->setLogEvents(s.logEvents);
    m_emu->setMirror(s.mirror);
    m_emu->setZoom(s.zoom);
    m_emu->setSensitivity(s.sensitivity);
    m_emu->setOverlay(s.motionOverlay);
    m_emu->setPlayerLook(s.player);
    // display
    m_display->setShowHand(s.showHand);
    m_display->setIntegerScaling(s.integerScaling);
    m_display->setOverlay(s.motionOverlay);
    m_display->setBindings(s.keys);
    m_display->update();
    // menus
    m_filterGroup->actions()[qBound(0, s.filter, 2)]->setChecked(true);
    m_actMute->setChecked(s.mute);
    m_actOverlay->setChecked(s.motionOverlay);
    m_actListView->setChecked(s.listView);
    m_actShowTitles->setChecked(s.showTitles);
    if (m_delegate->showTitles() != s.showTitles) { m_delegate->setShowTitles(s.showTitles); m_grid->reset(); }
    applyShortcuts();
    // camera: the device from the settings (empty = off)
    if (s.cameraId != oldCamera || (!s.cameraId.isEmpty() && !m_camera->active()) || (s.cameraId.isEmpty() && m_camera->active()))
        applyCamera(s.cameraId);
    updateCameraLabel();
    if (foldersChanged) {
        if (m_scanner) { m_scanner->cancel(); m_scanner->wait(); m_scanner = nullptr; }
        if (m_settings.discFolders.isEmpty()) m_model->clear();
        rescan();
    }
    updateEmptyState();
    updateActions();
}

void MainWindow::applyShortcuts() {
    const Settings &s = m_settings;
    m_actFullscreen->setShortcuts({s.key("fullscreen"), QKeySequence(Qt::ALT | Qt::Key_Return)});
    m_actNextFilter->setShortcut(s.key("filter"));
    m_actMute->setShortcut(s.key("mute"));
    m_actGainUp->setShortcut(s.key("gain_up"));
    m_actGainDown->setShortcut(s.key("gain_down"));
    m_actSaveState->setShortcut(s.key("save_state"));
    m_actLoadState->setShortcut(s.key("load_state"));
    m_actOverlay->setShortcut(s.key("overlay"));
    m_actScreenshot->setShortcut(s.key("screenshot"));
    m_actCamera->setShortcut(s.key("camera"));
}

void MainWindow::applyCamera(const QString &deviceId) {
    if (!deviceId.isEmpty() && m_camera->active() && m_camera->deviceId() == deviceId) {   // already on that device
        m_emu->setCameraEnabled(true);
        m_display->setCameraOn(true);
        m_actCamera->setChecked(true);
        return;
    }
    if (deviceId.isEmpty()) {
        m_camera->stop();
        m_emu->setCameraEnabled(false);
        m_display->setCameraOn(false);
        m_actCamera->setChecked(false);
        return;
    }
    const bool ok = m_camera->start(deviceId);
    m_emu->setCameraEnabled(ok);
    m_display->setCameraOn(ok);
    m_actCamera->setChecked(ok);
    if (!ok) statusBar()->showMessage("Camera: " + m_camera->lastError(), 6000);
}

void MainWindow::openSettings(int page) {
    if (m_dialog) { m_dialog->showPage(page); m_dialog->raise(); m_dialog->activateWindow(); return; }
    m_dialog = new SettingsDialog(m_settings, m_emu, m_camera, this);
    m_dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_dialog->setWindowModality(Qt::WindowModal);   // hotkeys on the main window would be undone by OK/Apply
    m_cameraBeforeDialog = m_camera->active() ? m_camera->deviceId() : QString();
    connect(m_dialog, &SettingsDialog::applied, this, &MainWindow::applySettings);
    connect(m_dialog, &SettingsDialog::cameraChangeRequested, this, [this](const QString &id) {
        if (id.isEmpty()) m_camera->stop();                         // live preview follows the combo box
        else if (m_camera->deviceId() != id || !m_camera->active()) m_camera->start(id);
        m_emu->setCameraEnabled(m_camera->active());
        m_display->setCameraOn(m_camera->active());
        updateCameraLabel();
    });
    connect(m_dialog, &QDialog::rejected, this, [this] { applyCamera(m_cameraBeforeDialog); });   // cancel: back to what was running
    m_dialog->showPage(page);
    m_dialog->show();
    m_dialog->syncCameraPreview();
}

void MainWindow::devRepair(const QString &path, bool offerOnly) {
    GameEntry g;
    if (!Scanner::identify(path, g)) { QMessageBox::critical(this, app::kName, "Cannot identify " + path); return; }
    if (offerOnly) { offerRepair(g, 1); return; }
    repairDisc(g, 1);
}

void MainWindow::devDialog(const QString &name) {
    if (name == "welcome") showWelcome();
    else if (name == "about") showAbout();
    else if (name == "licence") showLicence();
    else if (name == "covers") downloadCovers();
    else if (name == "update") checkForUpdates(true);
}

bool MainWindow::event(QEvent *e) {
    if (e->type() == QEvent::WindowDeactivate && m_settings.pauseOnFocusLoss && gameRunning() && !m_emu->isPaused() && !m_dialog) {
        m_autoPaused = true;
        m_emu->setPaused(true);
        m_actPause->setChecked(true);
        setStateText("Paused");
    } else if (e->type() == QEvent::WindowActivate && m_autoPaused) {
        m_autoPaused = false;
        if (gameRunning() && !m_menuPause) { m_emu->setPaused(false); m_actPause->setChecked(false); setStateText("Running"); }
    }
    return QMainWindow::event(e);
}

void MainWindow::closeEvent(QCloseEvent *e) {
    QSettings qs(app::settingsFile(), QSettings::IniFormat);
    if (!isFullScreen()) {
        qs.setValue("window/geometry", saveGeometry());
        qs.setValue("window/state", saveState());
    }
    m_settings.save();
    m_model->saveCache(app::cacheFile());
    if (m_scanner) { m_scanner->cancel(); m_scanner->wait(); }
    m_emu->stopAndWait();
    m_camera->stop();
    e->accept();
}

// ---- state ----------------------------------------------------------------------------------------------------

bool MainWindow::gameRunning() const { return m_emu->isRunning(); }

void MainWindow::setStateText(const QString &text) { m_stateLabel->setText(text); }

void MainWindow::updateActions() {
    const bool running = gameRunning();
    const bool selected = m_grid->selectionModel() && m_grid->selectionModel()->hasSelection();
    m_actStart->setEnabled(!running && selected);
    m_actPause->setEnabled(running);
    m_actReset->setEnabled(running);
    m_actStop->setEnabled(running);
    m_actFastForward->setEnabled(running);
    m_actSaveState->setEnabled(running);
    m_actLoadState->setEnabled(running);
    m_actScreenshot->setEnabled(running);
    m_saveSlotMenu->setEnabled(running);
    m_loadSlotMenu->setEnabled(running);
    m_actRescan->setEnabled(!m_scanner && !m_settings.discFolders.isEmpty());
}

void MainWindow::updateEmptyState() {
    const bool empty = m_model->rowCount() == 0;
    m_empty->setVisible(empty);
    m_grid->setVisible(!empty && !m_settings.listView);
    m_table->setVisible(!empty && m_settings.listView);
    if (!empty) return;
    if (m_settings.discFolders.isEmpty()) {
        m_emptyLabel->setText("No discs yet.\n\nAdd the folder with your disc images (.cue with .bin tracks, or .mdx).");
        m_emptyButton->setText("Add Disc Folder...");
    } else if (m_scanner) {
        m_emptyLabel->setText("Scanning...");
        m_emptyButton->setText("Rescan");
    } else {
        m_emptyLabel->setText("No disc images were found in:\n" + m_settings.discFolders.join("\n") +
                              "\n\niONemu reads a .cue with its .bin tracks, an .mdx or a single .bin. Unpack zip and 7z archives first.");
        m_emptyButton->setText("Rescan");
    }
}

void MainWindow::updateCameraLabel() {
    QString t;
    if (!m_camera->active()) t = "Camera off";
    else {
        t = "Camera: " + (m_camera->formatDescription().isEmpty() ? "starting" : m_camera->formatDescription());
        if (m_camera->fps() > 0) t += QString(" %1 fps").arg(m_camera->fps(), 0, 'f', 0);
    }
    if (gameRunning() && m_settings.motionOverlay) t += m_movingCells < 0 ? "  |  game not reading the camera" : QString("  |  %1 squares moving").arg(m_movingCells);
    m_cameraLabel->setText(t);
}

// ---- scanning -------------------------------------------------------------------------------------------------

void MainWindow::rescan() {
    if (m_scanner) return;
    m_delegate->clearCache();                      // the user may have dropped new cover files in
    m_scanner = new Scanner(m_settings.discFolders, m_model->byPath(), app::dataDir() + "/covers", this);
    connect(m_scanner, &Scanner::found, this, &MainWindow::onScanFound);
    connect(m_scanner, &Scanner::coverReady, this, [this](const QString &key) { m_delegate->invalidate(key); m_grid->viewport()->update(); });
    connect(m_scanner, &Scanner::progress, this, &MainWindow::onScanProgress);
    connect(m_scanner, &Scanner::done, this, &MainWindow::onScanDone);
    connect(m_scanner, &Scanner::finished, m_scanner, &QObject::deleteLater);
    m_scanner->start();
    updateActions();
    updateEmptyState();
}

void MainWindow::onScanFound(const GameEntry &entry) {
    if (sender() != m_scanner) return;             // a cancelled scanner's queued signals
    m_model->addOrReplace(entry);
    updateEmptyState();
}

void MainWindow::onScanProgress(int done, int total, const QString &path) {
    if (sender() != m_scanner) return;
    statusBar()->showMessage(QString("Scanning %1 of %2: %3").arg(done + 1).arg(total).arg(QFileInfo(path).fileName()));
}

void MainWindow::onScanDone(int found, int skipped, int archives, const QStringList &seen) {
    if (sender() != m_scanner) return;
    m_model->retainOnly(QSet<QString>(seen.begin(), seen.end()));
    m_model->saveCache(app::cacheFile());
    QString msg = QString("%1 disc%2 found").arg(found).arg(found == 1 ? "" : "s");
    if (skipped) msg += QString(", %1 file%2 skipped (not iON data tracks)").arg(skipped).arg(skipped == 1 ? "" : "s");
    if (archives) msg += QString(", %1 archive%2 ignored (extract .zip/.7z first)").arg(archives).arg(archives == 1 ? "" : "s");
    statusBar()->showMessage(msg, 8000);
    m_scanner = nullptr;
    updateActions();
    updateEmptyState();
}

void MainWindow::gameContextMenu(const QPoint &globalPos, const QModelIndex &idx) {
    if (!idx.isValid()) return;
    m_grid->setCurrentIndex(idx.siblingAtColumn(0));
    QMenu menu(this);
    menu.addAction(m_actStart);
    menu.addAction("Start From Entry...", this, &MainWindow::startFromEntryDialog);
    if (const GameEntry *g = m_model->entryAt(idx.row()); g && g->damaged) {
        const GameEntry copy = *g;
        menu.addAction("Repair Damaged Disc...", this, [this, copy] { repairDisc(copy, 1); });
    }
    menu.exec(globalPos);
}

void MainWindow::setListView(bool list) {
    m_settings.listView = list;
    m_settings.save();
    m_actListView->setChecked(list);
    updateEmptyState();
}

// First run: what the launcher needs and where the camera setup is.
void MainWindow::showWelcome() {
    m_settings.welcomeShown = true;
    m_settings.save();
    QDialog dlg(this);
    dlg.setWindowTitle("Getting Started");
    dlg.setMinimumWidth(520);
    auto *v = new QVBoxLayout(&dlg);
    auto *text = new QLabel(
        "<h3>Welcome to iONemu</h3>"
        "<p>iONemu plays Hasbro iON games from images of your own discs (a <b>.cue</b> with its <b>.bin</b> tracks, or an <b>.mdx</b>).</p>"
        "<ol>"
        "<li><b>Add the folder</b> with your disc images.</li>"
        "<li><b>Camera</b> (optional): set up a webcam on the Camera page. Without one, the mouse is your hand.</li>"
        "<li><b>Play</b>: double-click a cover. The games react to movement, so wave at things; with the mouse, "
        "move it or hold the left button. Esc opens the menu.</li>"
        "</ol>", &dlg);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    v->addWidget(text);
    auto *box = new QDialogButtonBox(&dlg);
    QPushButton *add = box->addButton("Add Disc Folder...", QDialogButtonBox::ActionRole);
    QPushButton *cam = box->addButton("Camera Setup...", QDialogButtonBox::ActionRole);
    box->addButton(QDialogButtonBox::Close);
    v->addWidget(box);
    connect(add, &QPushButton::clicked, &dlg, [&] { dlg.accept(); chooseFolder(); });
    connect(cam, &QPushButton::clicked, &dlg, [&] { dlg.accept(); openSettings(SettingsDialog::Camera); });
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.exec();
}

void MainWindow::showLicence() {
    QDialog dlg(this);
    dlg.setWindowTitle("Licence and Notices");
    dlg.resize(760, 560);
    auto *v = new QVBoxLayout(&dlg);
    auto *tabs = new QTabWidget(&dlg);
    for (const auto &[file, title] : {std::pair{":/licenses/GPL-3.0.txt", "GNU GPL v3"}, std::pair{":/licenses/THIRD_PARTY.txt", "Third-party notices"},
                                      std::pair{":/licenses/LGPL-3.0.txt", "GNU LGPL v3 (Qt)"}}) {
        auto *edit = new QPlainTextEdit(tabs);
        edit->setReadOnly(true);
        edit->setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont mono("Monospace");
        mono.setStyleHint(QFont::TypeWriter);
        edit->setFont(mono);
        QFile f(file);
        if (f.open(QIODevice::ReadOnly)) edit->setPlainText(QString::fromUtf8(f.readAll()));
        tabs->addTab(edit, title);
    }
    v->addWidget(tabs, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(box);
    dlg.exec();
}

void MainWindow::downloadCovers() {
    QList<GameEntry> games;
    for (int i = 0; i < m_model->rowCount(); i++) if (const GameEntry *g = m_model->entryAt(i)) games.push_back(*g);
    CoverDownloadDialog dlg(games, app::dataDir() + "/covers", m_settings.coverTemplates, m_settings.coverIndexUrl, this);
    connect(&dlg, &CoverDownloadDialog::coverReady, this, [this](const QString &key) { m_delegate->invalidate(key); m_grid->viewport()->update(); });
    dlg.exec();
    m_settings.coverTemplates = dlg.templates();
    m_settings.coverIndexUrl = dlg.indexUrl();
    m_settings.save();
}

void MainWindow::chooseFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Add Disc Folder");
    if (!dir.isEmpty()) addFolder(dir);
}

void MainWindow::addFolder(const QString &folder) {
    Settings s = m_settings;
    if (!s.discFolders.contains(folder)) s.discFolders << folder;
    applySettings(s);
    if (!m_scanner) rescan();
}

void MainWindow::openDiscFile() {
    const QString file = QFileDialog::getOpenFileName(this, "Open Disc", {}, "Disc images (*.cue *.mdx *.bin *.img);;All files (*)");
    if (!file.isEmpty()) startDisc(file, 1);
}

// ---- emulation ------------------------------------------------------------------------------------------------

void MainWindow::startSelected(int entry) {
    const GameEntry *g = m_model->entryAt(m_grid->currentIndex().row());
    if (!g) return;
    if (g->damaged && offerRepair(*g, entry)) return;
    startDisc(g->path, entry);
}

// A damaged dump (sectors whose stored checksum does not match): offer the repair before playing.
bool MainWindow::offerRepair(const GameEntry &g, int entry) {
    int donors = 0;
    for (int i = 0; i < m_model->rowCount(); i++) if (const GameEntry *o = m_model->entryAt(i); o && o->path != g.path && o->known) donors++;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle("Damaged disc image");
    box.setText(QString("<b>%1</b> was dumped with read errors.").arg(g.title.toHtmlEscaped()));
    box.setInformativeText(QString("It can usually be repaired from its own duplicate blocks and your other discs (%1 in your library). "
                                   "The repaired copy goes next to the original, which is left untouched.\n\n"
                                   "Played as it is, it may glitch where the damage is.").arg(donors));
    QPushButton *repair = box.addButton("Repair...", QMessageBox::AcceptRole);
    QPushButton *play = box.addButton("Play anyway", QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(repair);
    box.exec();
    if (box.clickedButton() == repair) { repairDisc(g, entry); return true; }
    if (box.clickedButton() == play) return false;
    return true;
}

namespace {
class RepairWorker : public QThread {
public:
    QString path, outDir;
    std::vector<std::string> donors;
    ion::RepairReport report;
    void run() override { report = ion::repair_disc(path.toStdString(), donors, outDir.toStdString()); }
};
}  // namespace

void MainWindow::repairDisc(const GameEntry &g, int entryAfter) {
    if (m_repairing) return;
    m_repairing = true;
    auto *w = new RepairWorker;
    w->path = g.path;
    w->outDir = QFileInfo(g.path).absolutePath();
    for (int i = 0; i < m_model->rowCount(); i++)
        if (const GameEntry *o = m_model->entryAt(i); o && o->path != g.path && o->known) w->donors.push_back(o->path.toStdString());
    auto *progress = new QProgressDialog("Looking for matching blocks on your discs and writing the repaired copy...", QString(), 0, 0, this);
    progress->setWindowTitle("Repairing " + g.title);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->show();
    connect(w, &QThread::finished, this, [this, w, progress, g, entryAfter] {
        progress->close();
        progress->deleteLater();
        m_repairing = false;
        const ion::RepairReport &r = w->report;
        QString notes;
        for (const std::string &n : r.notes) notes += "\n" + QString::fromStdString(n);
        if (!r.ok) {
            QMessageBox::critical(this, "Repair failed", QString::fromStdString(r.error) + notes);
        } else if (r.repaired == r.damaged) {
            QMessageBox::information(this, "Disc repaired",
                QString("All %1 damaged sector%2 replaced.%3\n\nThe repaired copy is\n%4\n\nThe original is kept but hidden from the list.")
                    .arg(r.damaged).arg(r.damaged == 1 ? " was" : "s were").arg(notes).arg(QString::fromStdString(r.output)));
            const QString out = QString::fromStdString(r.output);
            if (!m_scanner) rescan();                      // picks up the copy and hides the original
            startDisc(out, entryAfter);
        } else {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Information);
            box.setWindowTitle("Partly repaired");
            box.setText(QString("%1 of %2 damaged sectors could be replaced.").arg(r.repaired).arg(r.damaged));
            box.setInformativeText("The rest have no matching block on your discs; another iON disc may hold one. Add it to a disc folder and repair again." + notes);
            QPushButton *play = box.addButton("Play the original anyway", QMessageBox::AcceptRole);
            box.addButton(QMessageBox::Close);
            box.exec();
            if (box.clickedButton() == play) startDisc(g.path, entryAfter);
        }
        w->deleteLater();
    });
    w->start();
}

void MainWindow::startFromEntryDialog() {
    const GameEntry *g = m_model->entryAt(m_grid->currentIndex().row());
    if (!g) return;
    bool ok = false;
    const int entry = QInputDialog::getInt(this, "Start From Entry", QString("Directory entry (1 = system intro, %1 entries on this disc):").arg(g->entries), 1, 1, qMax(1, g->entries), 1, &ok);
    if (ok) startDisc(g->path, entry);
}

void MainWindow::startDisc(const QString &path, int entry) {
    if (gameRunning()) { m_emu->stopAndWait(); onEmuFinished(); }
    m_menuPause = false;
    m_autoPaused = false;
    m_actPause->setChecked(false);
    m_display->setMenuVisible(false);
    m_pages->setCurrentIndex(GamePage);
    setStateText("Starting...");
    m_actFastForward->setChecked(false);
    m_emu->setFastForwardToggle(false);
    m_emu->startDisc(path, entry);
    if (m_settings.startFullscreen) setFullscreen(true);
    updateActions();
}

void MainWindow::onEmuStarted(const QString &title) {
    m_pages->setCurrentIndex(GamePage);
    setWindowTitle(title + " - " + app::kName);
    setStateText("Running");
    // Short enough to fit beside the permanent labels; the full explanation is in Getting Started and About.
    statusBar()->showMessage(m_camera->active() ? "Wave your hand in front of things. Esc: menu."
                                                : "Wave: move the mouse or hold its button. Esc: menu.", 12000);
    m_display->setFocus();
    updateActions();
}

void MainWindow::onEmuFailed(const QString &error) {
    QMessageBox::critical(this, "Cannot start", error);
}

void MainWindow::onEmuFinished() {
    if (m_emu->isRunning()) return;                // a previous run's queued finished(), after a restart
    m_display->setMenuVisible(false);
    m_pages->setCurrentIndex(GridPage);
    if (isFullScreen()) setFullscreen(false);
    setWindowTitle(QString("%1 - %2").arg(app::kName, app::kTagline));
    setStateText("Stopped");
    m_fpsLabel->clear();
    m_audioLabel->clear();
    m_movingCells = -1;
    m_actPause->setChecked(false);
    m_actFastForward->setChecked(false);
    m_grid->setFocus();
    updateActions();
    updateCameraLabel();
}

void MainWindow::togglePause() {
    if (!gameRunning()) return;
    const bool pause = m_actPause->isChecked();
    m_emu->setPaused(pause);
    setStateText(pause ? "Paused" : "Running");
    if (!pause && m_menuPause) { m_menuPause = false; m_display->setMenuVisible(false); }
}

void MainWindow::resetGame() {
    if (!gameRunning()) return;
    m_emu->requestReset();
}

void MainWindow::stopGame() {
    if (!gameRunning()) return;
    setStateText("Stopping...");
    m_emu->requestStop();
}

void MainWindow::openPauseMenu() {
    if (!gameRunning()) return;
    if (m_display->menuVisible()) { resumeGame(); return; }
    m_menuPause = true;
    m_emu->setPaused(true);
    m_actPause->setChecked(true);
    setStateText("Paused");
    m_display->pauseMenu()->setFullscreenLabel(isFullScreen());
    m_display->setMenuVisible(true);
}

void MainWindow::resumeGame() {
    m_menuPause = false;
    m_display->setMenuVisible(false);
    if (!gameRunning()) return;
    m_emu->setPaused(false);
    m_actPause->setChecked(false);
    setStateText("Running");
    m_display->setFocus();
}

void MainWindow::setFullscreen(bool on) {
    if (on == isFullScreen()) { m_actFullscreen->setChecked(on); return; }
    if (on) {
        m_wasMaximized = isMaximized();
        menuBar()->hide(); m_toolbar->hide(); statusBar()->hide();
        showFullScreen();
    } else {
        menuBar()->show(); m_toolbar->show(); statusBar()->show();
        if (m_wasMaximized) showMaximized(); else showNormal();
    }
    m_actFullscreen->setChecked(on);
    m_display->pauseMenu()->setFullscreenLabel(on);
    if (gameRunning() && !m_display->menuVisible()) m_display->setFocus();
}

// ---- quick settings (hotkeys and menus; the dialog covers the rest) ------------------------------------------

void MainWindow::setFilter(int filter) {
    m_settings.filter = qBound(0, filter, 2);
    m_settings.save();
    m_emu->setFilter(m_settings.filter);
    m_filterGroup->actions()[m_settings.filter]->setChecked(true);
    m_display->update();
}

void MainWindow::nextFilter() {
    setFilter((m_settings.filter + 1) % 3);
    static const char *names[] = {"composite", "sharp", "smooth"};
    statusBar()->showMessage(QString("Video filter: %1").arg(names[m_settings.filter]), 2000);
}

void MainWindow::setGainDb(double db) {
    m_settings.gainDb = qBound(-30.0, db, 30.0);
    m_settings.save();
    m_emu->setGainDb(m_settings.gainDb);
    statusBar()->showMessage(QString("Volume %1%2 dB").arg(m_settings.gainDb >= 0 ? "+" : "").arg(m_settings.gainDb, 0, 'f', 0), 2000);
}

void MainWindow::setMute(bool mute) {
    m_settings.mute = mute;
    m_settings.save();
    m_emu->setMute(mute);
    m_actMute->setChecked(mute);
    statusBar()->showMessage(mute ? "Muted" : "Unmuted", 2000);
}

void MainWindow::setOverlay(bool on) {
    m_settings.motionOverlay = on;
    m_settings.save();
    m_emu->setOverlay(on);
    m_display->setOverlay(on);
    m_actOverlay->setChecked(on);
    statusBar()->showMessage(on ? "Motion overlay on (blue: where the game looks, red: movement)" : "Motion overlay off", 3000);
    updateCameraLabel();
}

void MainWindow::toggleCamera() {
    if (m_camera->active()) {
        applyCamera(QString());
        statusBar()->showMessage("Camera off: the mouse is the hand", 3000);
        return;
    }
    if (m_settings.cameraId.isEmpty()) { openSettings(SettingsDialog::Camera); return; }
    applyCamera(m_settings.cameraId);
}

void MainWindow::setCurrentSlot(int slot) {
    m_currentSlot = qBound(1, slot, 5);
    statusBar()->showMessage(QString("Save-state slot %1").arg(m_currentSlot), 2000);
}

void MainWindow::showAbout() {
    QDialog dlg(this);
    dlg.setWindowTitle(QString("About %1").arg(app::kName));
    auto *v = new QVBoxLayout(&dlg);
    auto *logo = new QLabel(&dlg);
    QSvgRenderer svg(QString(":/branding/logo.svg"));
    const qreal dpr = devicePixelRatioF();
    QPixmap pm(int(480 * dpr), int(150 * dpr));
    pm.fill(Qt::transparent);
    { QPainter p(&pm); svg.render(&p, QRectF(0, 0, pm.width(), pm.height())); }
    pm.setDevicePixelRatio(dpr);
    logo->setPixmap(pm);
    logo->setAlignment(Qt::AlignCenter);
    v->addWidget(logo);
    auto *text = new QLabel(
        QString("<p><b>%1 %2</b>, %3.</p>"
                "<p>Plays the games of the 2006 Hasbro iON console on your computer. Your webcam is the controller, "
                "as it was on the console; without one, the mouse is your hand.</p>"
                "<p>Free software under the GNU GPL, version 3 or later. No game data is included: you need images of your own discs.</p>"
                "<p><a href=\"%4\">%5</a></p>"
                "<p>Not affiliated with Hasbro or Nickelodeon.</p>")
            .arg(app::kName, app::kVersion, app::kTagline, app::webUrl(), app::webUrl().mid(8)), &dlg);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setOpenExternalLinks(true);
    text->setMaximumWidth(480);
    v->addWidget(text);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(box);
    dlg.exec();
}
