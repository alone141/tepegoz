#pragma once
namespace tepegoz
{

class ITepegoz
{
public:
    virtual int StartUp() = 0;
    virtual int TearDown() = 0;
    virtual void Run() = 0;
    virtual void RequestStop() = 0; // thread-safe: Run() stops the services and returns

    virtual ~ITepegoz() = default;
};

} // namespace tepegoz
