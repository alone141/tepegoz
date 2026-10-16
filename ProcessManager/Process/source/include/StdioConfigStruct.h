#pragma once 
#include <StdioModeEnum.h>
#include <filesystem>
namespace process_manager
{
	struct StdioPolicyStruct
	{
		void UpdateArgs(const char* sectionOrder);


		StdioModeEnum StdioMode = StdioModeEnum::Inherit;
		std::filesystem::path StdioFilePath{};
		bool StdioAppend = true;
	};
}
