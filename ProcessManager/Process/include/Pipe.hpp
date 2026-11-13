#pragma once
namespace process_manager
{
    struct PipeStruct
    {
        int readFd = -1;
        int writeFd = -1;

        PipeStruct() = default;
        PipeStruct(const PipeStruct&) = delete;
        PipeStruct& operator=(const PipeStruct&) = delete;
        PipeStruct(PipeStruct&& other) noexcept;
        PipeStruct& operator=(PipeStruct&& other) noexcept;
        ~PipeStruct();

        void ConstructPipe();
        void CloseRead();
        void CloseWrite();
    };
}