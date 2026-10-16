#pragma once 
namespace process_manager
{
	struct PipeStruct
	{
		int readFd = -1;
		int writeFd = -1;

		void ConstructPipe();
		~PipeStruct();
	};
}