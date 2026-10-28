// Application constants and per-user paths. Everything that shows or stores the name goes through these constants.
#pragma once
#include <QString>

namespace app {

inline constexpr const char *kName = "iONemu";                       // decided 2026-09-26; repo "ionemu"
inline constexpr const char *kTagline = "a Hasbro iON emulator";     // shown with the name for searchability
inline constexpr const char *kId = "ionemu";                         // folder and file names
inline constexpr const char *kVersion = "0.1.0";
inline constexpr const char *kReleaseDate = "2026-10-28";                 // when kVersion was released (the update dialog shows how far behind a user is)
inline constexpr const char *kRepo = "LegendaryDoors/ionemu";             // GitHub owner/name: releases are checked at api.github.com/repos/<kRepo>/releases/latest
inline QString webUrl(const QString &path = {}) { return QString("https://github.com/%1%2").arg(kRepo, path); }   // e.g. "/wiki/Finding-The-Games"

QString settingsFile();   // <config dir>/ionemu.ini
QString dataDir();        // per-user data root (created on demand)
QString savesDir();    // <dataDir>/saves: one .flash per disc (the console's save chip)
QString statesDir();   // <dataDir>/states: save states
QString cacheFile();   // <dataDir>/gamelist.json: the scanned disc list
QString safeFileName(const QString &s);   // strip characters that file systems reject

}  // namespace app
