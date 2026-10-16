#include <PipeStruct.h>
#include <unistd.h>
void process_manager::PipeStruct::ConstructPipe()
{
	int pipeFd[2];
	if (::pipe(pipeFd) == -1)
	{
		return;
	}
	this->readFd = pipeFd[0];
	this->writeFd = pipeFd[1];
}

process_manager::PipeStruct::~PipeStruct()
{
	if (readFd >= 0) { ::close(readFd); readFd = -1; }
	if (writeFd >= 0) { ::close(writeFd); writeFd = -1; }
}
