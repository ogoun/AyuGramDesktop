// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

namespace AyuDiagnostics {

// Windows only, no-op elsewhere. Tracks USER objects (windows, timers,
// menus, icons, cursors, hooks) created through the executable's imports
// and periodically writes the call stacks of the live ones to
// <working dir>/user_objects.log, so a slow leak of the per-process
// 10 000 USER handles quota can be attributed to the code creating them.
void InstallUserObjectsTracker(const QString &workingDir);

} // namespace AyuDiagnostics
