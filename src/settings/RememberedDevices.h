#pragma once
#include <QStringList>

class QTextStream;

namespace airshow {

class AppSettings;

// Command-line management of remembered devices (AppSettings trusted devices:
// approved once, then admitted without the approval prompt):
//
//   airshow --list-remembered
//   airshow --forget-remembered <identifier | device name | all>
//
// These run instead of the receiver, before GStreamer or a display is needed,
// so they work over SSH. They act on the settings of the user running them,
// and may be used while AirShow is running.

// Whether args (without the program name) ask for one of these commands.
bool isRememberedDevicesCommand(const QStringList& args);

// Runs the command in args and returns the process exit code: 0 on success,
// 1 if nothing matched what was to be forgotten, 2 on a usage error.
int runRememberedDevicesCommand(const QStringList& args, AppSettings& settings,
                                QTextStream& out, QTextStream& err);

} // namespace airshow
