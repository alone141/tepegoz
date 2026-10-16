#pragma once 
#include <RestartModeEnum.h>
#include <chrono>
namespace process_manager
{
	struct RestartPolicyStruct
	{
		void UpdateArgs(const char* sectionOrder);
		RestartModeEnum RestartMode = RestartModeEnum::OnFailure;
		int MaxRestarts = 10; // in a row; a run of RestartResetMs or longer starts the count again
		double BackoffMultiplier = 2.0;
		std::chrono::milliseconds RestartDelayMs{ 1'000 };
		std::chrono::milliseconds RestartResetMs{ 60'000 };
	};
}
