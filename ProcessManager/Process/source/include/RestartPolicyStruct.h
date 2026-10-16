#pragma once 
#include <RestartModeEnum.h>
#include <chrono>
namespace process_manager
{
	struct RestartPolicyStruct
	{
		void UpdateArgs(const char* sectionOrder);
		RestartModeEnum RestartMode = RestartModeEnum::OnFailure;
		int MaxRestarts = 10;
		double BackoffMultiplier = 2.0;
	};
}
