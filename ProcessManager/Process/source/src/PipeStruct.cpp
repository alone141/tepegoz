#include <PipeStruct.h>
#include <fcntl.h>
#include <unistd.h>
void process_manager::PipeStruct::ConstructPipe()
{
    CloseRead();
    CloseWrite();

    // O_CLOEXEC: no other child inherits these ends; dup2 onto stdout clears the flag for the one that needs it.
    int pipeFd[2];
    if (::pipe2(pipeFd, O_CLOEXEC) == -1)
    {
        return;
    }
    this->readFd = pipeFd[0];
    this->writeFd = pipeFd[1];
}

void process_manager::PipeStruct::CloseRead()
{
    if (readFd >= 0) { ::close(readFd); readFd = -1; }
}

void process_manager::PipeStruct::CloseWrite()
{
    if (writeFd >= 0) { ::close(writeFd); writeFd = -1; }
}

process_manager::PipeStruct::PipeStruct(PipeStruct&& other) noexcept
    : readFd(other.readFd), writeFd(other.writeFd)
{
    other.readFd = -1;
    other.writeFd = -1;
}

process_manager::PipeStruct& process_manager::PipeStruct::operator=(PipeStruct&& other) noexcept
{
    if (this != &other)
    {
        CloseRead();
        CloseWrite();
        readFd = other.readFd;
        writeFd = other.writeFd;
        other.readFd = -1;
        other.writeFd = -1;
    }
    return *this;
}

process_manager::PipeStruct::~PipeStruct()
{
    CloseRead();
    CloseWrite();
}
