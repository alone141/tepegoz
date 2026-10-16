#pragma once 
#include <Process.h>
#include <vector>
#include <WorkModeEnum.h>
namespace process_manager {
	struct ProcessManagerParams : legacy::type::Arguments {
		WorkModeEnum workMode;
		std::vector<Process> processList;
	};
}
