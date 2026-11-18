#include "Pipe.hpp"
#include <fcntl.h>
#include <unistd.h>

namespace tepegoz
{

void Pipe::ConstructPipe()
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

void Pipe::CloseRead()
{
    if (readFd >= 0) { ::close(readFd); readFd = -1; }
}

void Pipe::CloseWrite()
{
    if (writeFd >= 0) { ::close(writeFd); writeFd = -1; }
}

Pipe::Pipe(Pipe&& other) noexcept
    : readFd(other.readFd), writeFd(other.writeFd)
{
    other.readFd = -1;
    other.writeFd = -1;
}

Pipe& Pipe::operator=(Pipe&& other) noexcept
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

Pipe::~Pipe()
{
    CloseRead();
    CloseWrite();
}

} // namespace tepegoz
