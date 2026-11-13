#pragma once
namespace process_manager
{
    struct ExitStatusStruct
    {
        pid_t Pid = -1;
        bool IsTerminatedBySignal = true;
        int ExitCode = -1;
        int TermSignal = -1;
    };
}