#pragma once
namespace process_manager
{
struct Pipe
{
    int readFd = -1;
    int writeFd = -1;

    Pipe() = default;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&& other) noexcept;
    Pipe& operator=(Pipe&& other) noexcept;
    ~Pipe();

    void ConstructPipe();
    void CloseRead();
    void CloseWrite();
};
}