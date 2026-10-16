#pragma once 
namespace process_manager
{
	enum class RestartModeEnum
	{
		Never,
		OnFailure,
		Always,
		UnlessStopped
	};

}
