// Crash reporting: when the game process dies from an unhandled exception, write
// szModern-crash.txt (exception, registers, call stack guess, loaded modules) and
// szModern-crash.dmp (minidump for a debugger) next to the game.
#pragma once

namespace crash {
void Install();             // safe to call more than once (re-arms the filter if the game replaced it)
void LogSystemInfo();       // Windows version etc. into szModern.log
void NoteFrame(unsigned long long frame);  // last presented frame, shown in the report
}  // namespace crash
