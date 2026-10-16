#pragma once 
#include <chrono>
namespace process_manager
{
	struct HealthPolicyStruct
	{
		void UpdateArgs(const char* sectionOrder);

		bool CheckHealth = false; //enable health checks //todo ENABLE HEARTBEAT meliksahs
		std::chrono::milliseconds GracePeriodMs{ 5'000 }; //startup period

		std::chrono::milliseconds HeartbeatPeriodMs{ 1'000 };
		int MissedHeartbeatThreshold = 5; //take action after threshold
	};
}