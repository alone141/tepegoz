#pragma once 
#include <string_view>
#include <string>

namespace process_manager
{
	enum ProcessManagerCommandEnum : char
	{
		Unknown = -1,
		StartServiceWithArgs = 77,
		StartService,
		StopService,
		DisableService,
		RestartService,
		GetVersionInfo
	};
	struct ProcessManagerCommandStruct
	{
		ProcessManagerCommandEnum command;
		char ServiceName[32];
		char Args[32];
	};

	struct __attribute__((packed)) VersionInfo
	{
		uint8_t major = 1;
		uint8_t minor = 0;
		uint16_t patch = 0;
	};
	struct __attribute__((packed)) VersionReportStruct
	{
		ProcessManagerCommandEnum command; //1 bit
		VersionInfo gsm;
		VersionInfo ggm;
		VersionInfo dam;
		VersionInfo yam;
		VersionInfo syy;
		VersionInfo bpm;
		VersionInfo dam2;
		VersionInfo tim;
		uint8_t reserved[3];
	};
	namespace comm
	{
		constexpr std::string_view COMMAND_SERVER_ID = "PMC";
		constexpr std::string_view GUI_ID = "GUI";
		//constexpr std::string_view SIMPLIFIED_HEALTH_TOPIC = "SHR";
		//constexpr std::string_view DETAILED_HEALTH_TOPIC = "DHR";
		inline const std::string PROCESS_MANAGER_ID = "BPM"; //loglayabilmek icin std::string 
		inline const char* COMMAND_SERVER_PORT = "tcp://localhost:5557";
		inline const char* HEALTH_PUBLISH_PORT_DETAILED = "tcp://*:6667";
		inline const char* HEALTH_PUBLISH_PORT_SIMPLE = "tcp://*:6668";
		inline const char* HEALTH_LISTEN_PORT_DETAILED = "tcp://localhost:6667";
		inline const char* HEALTH_LISTEN_PORT_SIMPLE = "tcp://localhost:6667";
		// Journal reads for a monitor GUI on another machine (JournalServer).
		inline const char* JOURNAL_SERVER_PORT = "tcp://*:6670";
	}
}