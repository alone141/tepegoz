#pragma once
#include <sys/types.h>
namespace tepegoz
{

struct ProcessHandle
{
    pid_t pid = -1;
    pid_t pgid = -1; // process group id
};

} // namespace tepegoz
