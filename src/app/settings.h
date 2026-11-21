// The drive's settings, kept in %APPDATA%\MLV Drive\mlvdrive.ini (UTF-8):
//
//   [mount]
//   folder = E:\mlv            ; the MLV folder (clips in its subfolders too)
//   drive = M:                 ; empty: the next free letter from Z: down
//   test_patterns = false      ; add the TestPatterns folder
//   mount_at_start = true      ; the tray app mounts when it starts
//   [dng]
//   format = 16                ; 16 (16-bit) or packed (to the clip's bit depth)
//   raw_fixes = true           ; stripes, focus and bad pixels
//   timecode = time            ; time (of day) or count (from 00:00:00:00)
//   [performance]
//   cache_mb = 512             ; finished frames kept in memory
//   read_ahead = auto          ; frames made ahead of the one read: auto (one per CPU) or a number
//
// The console program takes them as defaults (its options override them); the tray app reads and
// writes them.
#pragma once

#include "dng/dng_writer.h"

#include <string>
#include <vector>

namespace app {

struct Settings {
    std::wstring folder;
    std::wstring drive = L"M:";
    bool testPatterns = false;
    bool mountAtStart = true;
    dng::Output format = dng::Output::Uncompressed16;  // Uncompressed16 or Uncompressed (packed)
    bool rawFixes = true;
    dng::Timecode timecode = dng::Timecode::TimeOfDay;
    unsigned cacheMb = 512;
    int readAhead = -1;  // -1: one per CPU
};

// %APPDATA%\MLV Drive\mlvdrive.ini
std::wstring settingsPath();

// The settings in `path` over the defaults (a missing file: the defaults). Unknown keys are
// ignored; a bad value keeps its default and is reported in `warnings`.
Settings loadSettings(const std::wstring& path, std::vector<std::string>* warnings);
bool saveSettings(const std::wstring& path, const Settings& settings, std::string* error);

// "M" or "m:" -> "M:"; anything else as it is.
std::wstring normalizeDrive(std::wstring drive);

}  // namespace app
