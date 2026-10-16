#include <StdioConfigStruct.h>
#include <ConfigHelper.h>
#include <string>
using namespace process_manager;
void process_manager::StdioPolicyStruct::UpdateArgs(const char* sectionOrder)
{
	const char* temp;

	ConfigHelper::ReadConfiguration(sectionOrder, "StdioMode", temp, "Inherit");
	if (!strcmp(temp, "Inherit")) { this->StdioMode = StdioModeEnum::Inherit; }
	else if (!strcmp(temp, "Pipe")) { this->StdioMode = StdioModeEnum::Pipe; }
	else if (!strcmp(temp, "File")) { this->StdioMode = StdioModeEnum::File; }
	else if (!strcmp(temp, "NullDevice")) { this->StdioMode = StdioModeEnum::NullDevice; }

	ConfigHelper::ReadConfiguration(sectionOrder, "StdioFilePath", temp, "/tmp/output.txt"); //todo isme gore
	StdioFilePath = std::filesystem::path(temp);

	ConfigHelper::ReadValue(sectionOrder, "StdioAppend", StdioAppend, false, 0, 1);

}
