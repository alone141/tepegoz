#pragma once
#include "IProcessManager.hpp"
#include "Process.hpp"
#include "ProcessManagerParams.hpp"
#include <zmq.hpp>
#include "ProcessManagerCommands.hpp"

#include <atomic>

namespace process_manager
{
    class ProcessManager : public IProcessManager
    {
    public:
        int StartUp();

        ~ProcessManager();
        ProcessManager(std::unique_ptr<ProcessManagerParams> args);
        ProcessManager(ProcessManager&&) = delete;
        ProcessManager(const ProcessManager&) = delete;
        ProcessManager& operator=(const ProcessManager&) = delete;

        void Run() override;
        void RequestStop() override;
        int TearDown() override;

        static bool SortByDependencies(std::vector<std::string>& err, std::vector<process_manager::Process>& pList);
        // Problems in the configuration go to errors when given, else to the log.
        static std::unique_ptr<ProcessManagerParams> GenerateConfig(const config_section::ConfigValue& document, std::vector<std::string>* errors = nullptr);
    private:
        void StopAll();
        void RetireProcess(Process& process);
        bool ApplyToAll(CommandCode command);
        bool ApplyCommandServerMessage(const CommandMessage& message);
        bool ProcessMessage(const std::vector<zmq::message_t>& messages);
        bool CheckProcessConfig(std::vector<std::string>& err) const;
        bool StartSequentialAsync() const;
        void StartDependencies(const Process& process) const;
        bool IsStopInProgress() const;
        std::vector<Process>::iterator GetProcess(std::string_view processName) const;


        std::unique_ptr<ProcessManagerParams> processParams;
        std::vector<Process> retiring; // disabled or replaced, still being stopped
        std::vector<std::string> errorList;
        std::atomic<bool> enabled{ true };

        zmq::context_t zmqContext;
        zmq::socket_t listener;
        zmq::socket_t statusPublisher;
    };
}
