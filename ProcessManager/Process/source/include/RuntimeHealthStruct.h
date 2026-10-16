#pragma once 
#include <chrono>
#include <ctype.h>
namespace process_manager
{
	enum class RuntimeStateEnum : uint8_t
	{
		Unknown,
		Starting,
		Running,
		Stopped,
		Unhealthy
	};
	[[maybe_unused]]
	static const char* RuntimeStateToString(RuntimeStateEnum e)
	{
		switch (e)
		{
		case process_manager::RuntimeStateEnum::Unknown:
			return "Unknown";
			break;
		case process_manager::RuntimeStateEnum::Starting:
			return "Starting";
			break;
		case process_manager::RuntimeStateEnum::Running:
			return "Running";
			break;
		case process_manager::RuntimeStateEnum::Stopped:
			return "Stopped";
			break;
		case process_manager::RuntimeStateEnum::Unhealthy:
			return "Unhealthy";
			break;
		default:
			return "Unknown";
			break;
		}
	}
	struct RuntimeHealthStruct
	{
		RuntimeStateEnum state = RuntimeStateEnum::Unknown;

		std::chrono::high_resolution_clock::time_point startTime;
		std::chrono::high_resolution_clock::time_point lastSeen;

		int missedBeats = 0;
		int restatCount = 0;


	};
	struct DetailedHealthReportStruct
	{
		char processName[64];
		pid_t pid;
		uint64_t memoryUsageInBytes;
		uint64_t cpuUsageInUsec;

		RuntimeHealthStruct runtimeHealth;

		std::chrono::high_resolution_clock::time_point snapshotTime;
	};

	struct SimpleHealthReportStruct
	{
		process_manager::RuntimeStateEnum SDR = process_manager::RuntimeStateEnum::Unknown;
		process_manager::RuntimeStateEnum GGM = process_manager::RuntimeStateEnum::Unknown;
		process_manager::RuntimeStateEnum YAM = process_manager::RuntimeStateEnum::Unknown;
		process_manager::RuntimeStateEnum TIM = process_manager::RuntimeStateEnum::Unknown;
	};
}