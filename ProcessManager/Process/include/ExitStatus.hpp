#pragma once
#include <sys/types.h>
namespace process_manager
{

struct ExitStatus
{
    pid_t pid = -1;
    bool isTerminatedBySignal = true;
    int exitCode = -1;
    int termSignal = -1;
    bool duringStop = false; // reaped while a stop the manager began was under way
};

} // namespace process_manager
