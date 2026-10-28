#include "settings.h"
#include "app.h"
#include <QSettings>

const QList<Settings::Action> &Settings::actions() {
    static const QList<Action> list = {
        // console buttons
        {"play_pause", "Play / Pause", "Console buttons", "Space"},
        {"stop", "Stop", "Console buttons", "Backspace"},
        {"next", "Next", "Console buttons", "Right"},
        {"previous", "Previous", "Console buttons", "Left"},
        {"brightness", "Brightness", "Console buttons", "B"},
        {"open", "Open (exits the game)", "Console buttons", "O"},
        // hotkeys
        {"fast_forward", "Fast-forward (hold)", "Hotkeys", "Tab"},
        {"fullscreen", "Fullscreen", "Hotkeys", "F11"},
        {"filter", "Next video filter", "Hotkeys", "F"},
        {"mute", "Mute", "Hotkeys", "M"},
        {"gain_up", "Volume up", "Hotkeys", "="},
        {"gain_down", "Volume down", "Hotkeys", "-"},
        {"save_state", "Save state (current slot)", "Hotkeys", "F5"},
        {"load_state", "Load state (current slot)", "Hotkeys", "F8"},
        {"overlay", "Motion overlay", "Hotkeys", "D"},
        {"screenshot", "Screenshot", "Hotkeys", "S"},
        {"camera", "Camera on / off", "Hotkeys", "W"},
    };
    return list;
}

QHash<QString, QKeySequence> Settings::defaultKeys() {
    QHash<QString, QKeySequence> h;
    for (const Action &a : actions()) h.insert(a.id, QKeySequence(a.defaultKey));
    return h;
}

QKeySequence Settings::key(const QString &id) const { return keys.value(id); }

Settings Settings::load() {
    QSettings s(app::settingsFile(), QSettings::IniFormat);
    Settings st;
    st.discFolders = s.value("folders/list").toStringList();
    // These keys lived in a group called "general" until 0.1.0. QSettings treats that name as its special ungrouped
    // section: the file gets "[%General]", which reads back as "General/...", and the case-sensitive lookup on Linux
    // and macOS never found it again (the Getting Started dialog came back on every start). Read the old spelling once.
    st.startFullscreen = s.value("launcher/start_fullscreen", s.value("General/start_fullscreen", false)).toBool();
    st.pauseOnFocusLoss = s.value("launcher/pause_on_focus_loss", s.value("General/pause_on_focus_loss", false)).toBool();
    st.filter = s.value("video/filter", 0).toInt();
    st.integerScaling = s.value("video/integer_scaling", false).toBool();
    st.showHand = s.value("video/show_hand", true).toBool();
    st.gainDb = s.value("audio/gain_db", 12.0).toDouble();
    st.mute = s.value("audio/mute", false).toBool();
    st.bufferMs = s.value("audio/buffer_ms", 80).toInt();
    st.cameraId = s.value("camera/device").toString();
    st.mirror = s.value("camera/mirror", true).toBool();
    st.zoom = s.value("camera/zoom", 1.0).toDouble();
    st.sensitivity = s.value("camera/sensitivity", 1.0).toDouble();
    auto colour = [&](const char *key, float *rgb) {
        const QColor c(s.value(key).toString());
        if (c.isValid()) { rgb[0] = c.red(); rgb[1] = c.green(); rgb[2] = c.blue(); }
    };
    colour("player/skin", st.player.skin); colour("player/hair", st.player.hair); colour("player/shirt", st.player.shirt);
    colour("player/wall", st.player.wall); colour("player/floor", st.player.floor);
    st.player.style = s.value("player/style", 0).toInt();
    st.player.hair_style = s.value("player/hair_style", 0).toInt();
    st.player.window = s.value("player/window", true).toBool();
    st.keys = defaultKeys();
    s.beginGroup("keys");
    for (const Action &a : actions())
        if (s.contains(a.id)) st.keys.insert(a.id, QKeySequence(s.value(a.id).toString()));
    s.endGroup();
    st.adpcm36Model = s.value("advanced/adpcm36_model", 3).toInt();
    st.motionOverlay = s.value("advanced/motion_overlay", false).toBool();
    st.logEvents = s.value("advanced/log_events", false).toBool();
    st.listView = s.value("view/list", false).toBool();
    st.showTitles = s.value("view/show_titles", false).toBool();
    st.coverTemplates = s.value("covers/templates").toStringList();
    st.coverIndexUrl = s.value("covers/index_url", QString()).toString();
    st.welcomeShown = s.value("launcher/welcome_shown", s.value("General/welcome_shown", false)).toBool();
    st.checkUpdates = s.value("updates/check_on_start", true).toBool();
    st.skippedVersion = s.value("updates/skipped_version").toString();
    st.lastUpdateCheck = s.value("updates/last_check").toString();
    return st;
}

void Settings::save() const {
    QSettings s(app::settingsFile(), QSettings::IniFormat);
    s.setValue("folders/list", discFolders);
    s.setValue("launcher/start_fullscreen", startFullscreen);
    s.setValue("launcher/pause_on_focus_loss", pauseOnFocusLoss);
    s.setValue("video/filter", filter);
    s.setValue("video/integer_scaling", integerScaling);
    s.setValue("video/show_hand", showHand);
    s.setValue("audio/gain_db", gainDb);
    s.setValue("audio/mute", mute);
    s.setValue("audio/buffer_ms", bufferMs);
    s.setValue("camera/device", cameraId);
    s.setValue("camera/mirror", mirror);
    s.setValue("camera/zoom", zoom);
    s.setValue("camera/sensitivity", sensitivity);
    auto colour = [&](const char *key, const float *rgb) { s.setValue(key, QColor(int(rgb[0]), int(rgb[1]), int(rgb[2])).name()); };
    colour("player/skin", player.skin); colour("player/hair", player.hair); colour("player/shirt", player.shirt);
    colour("player/wall", player.wall); colour("player/floor", player.floor);
    s.setValue("player/style", player.style);
    s.setValue("player/hair_style", player.hair_style);
    s.setValue("player/window", player.window);
    s.beginGroup("keys");
    for (const Action &a : actions()) s.setValue(a.id, keys.value(a.id).toString(QKeySequence::PortableText));
    s.endGroup();
    s.setValue("advanced/adpcm36_model", adpcm36Model);
    s.setValue("advanced/motion_overlay", motionOverlay);
    s.setValue("advanced/log_events", logEvents);
    s.setValue("view/list", listView);
    s.setValue("view/show_titles", showTitles);
    s.setValue("covers/templates", coverTemplates);
    s.setValue("covers/index_url", coverIndexUrl);
    s.setValue("launcher/welcome_shown", welcomeShown);
    s.setValue("updates/check_on_start", checkUpdates);
    s.setValue("updates/skipped_version", skippedVersion);
    s.setValue("updates/last_check", lastUpdateCheck);
}
