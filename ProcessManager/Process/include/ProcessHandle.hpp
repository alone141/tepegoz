#pragma once
#include <sys/types.h>
namespace process_manager
{

struct ProcessHandle
{
    pid_t pid = -1;
    pid_t pgid = -1; // process group id
};

} // namespace process_manager
