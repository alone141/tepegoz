#pragma once
namespace process_manager
{
enum class RestartMode
{
    Never,
    OnFailure,
    Always,
    UnlessStopped
};

}
