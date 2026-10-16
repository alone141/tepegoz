#pragma once

#include <string>

// Returns the exit code when the command line was a one-shot command, or -1 to run the manager
// with configPath (--config, empty for the installed config).
int ParseInputFlags(const int& argc, char**& argv, std::string& configPath);
void GenerateTemplateConfig();