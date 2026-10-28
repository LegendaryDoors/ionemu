// All user settings in one struct, persisted with QSettings (ini). The dialog edits a copy and the main window applies
// the result to the running parts (emulation thread, display, camera, shortcuts).
#pragma once
#include "ioncore/ioncore.h"
#include <QColor>
#include <QHash>
#include <QKeySequence>
#include <QString>
#include <QStringList>

struct Settings {
    // General
    QStringList discFolders;
    bool startFullscreen = false;
    bool pauseOnFocusLoss = false;
    // Graphics
    int filter = 0;                  // 0 composite, 1 sharp, 2 smooth
    bool integerScaling = false;
    bool showHand = true;            // draw the virtual-hand marker over the picture
    // Audio
    double gainDb = 12.0;            // master gain; +12 dB matches real-console recordings
    bool mute = false;
    int bufferMs = 80;
    // Camera
    QString cameraId;                // QCameraDevice id; empty = off (the mouse is the hand)
    bool mirror = true;
    double zoom = 1.0;               // < 1 shrinks the picture (a close player looks far away)
    double sensitivity = 1.0;        // the game's threshold is divided by this
    // Player (the stand-in used without a webcam)
    ion::PlayerLook player;
    // Controls: action id -> key (see Settings::actions())
    QHash<QString, QKeySequence> keys;
    // Advanced
    int adpcm36Model = 3;            // 3 = fitted table (default), 0 = the old q6 guess
    bool motionOverlay = false;      // blue/red overlay in the game view
    bool logEvents = false;          // print the core's event log to stderr
    // View / covers / first run
    bool listView = false;           // list instead of the cover grid
    bool showTitles = false;         // title under each cover (the generated cards carry it already)
    QStringList coverTemplates;      // Download Covers... URL templates
    QString coverIndexUrl;           // Download Covers... index JSON (SHA-1 -> URL)
    bool welcomeShown = false;
    bool checkUpdates = true;        // ask GitHub for a newer release once a day at start
    QString skippedVersion;          // "Skip This Version" in the update dialog
    QString lastUpdateCheck;         // ISO date-time of the last automatic check

    struct Action { const char *id, *label, *group, *defaultKey; };
    static const QList<Action> &actions();
    static QHash<QString, QKeySequence> defaultKeys();
    QKeySequence key(const QString &id) const;

    static Settings load();
    void save() const;
};
