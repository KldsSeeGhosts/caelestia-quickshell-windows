#pragma once

namespace qs::launch {

#ifdef _WIN32
extern void* DAEMON_PIPE; // NOLINT
bool spawnDaemon(int argc, char** argv, int* exitCode);
#else
extern int DAEMON_PIPE; // NOLINT
#endif

void exitDaemon(int code);

} // namespace qs::launch
