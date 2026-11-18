#pragma once
#include "ITepegoz.hpp"
#include "Process.hpp"
#include "TepegozParams.hpp"
#include <zmq.hpp>
#include "TepegozCommands.hpp"
#include "ServiceRegistry.hpp"

#include <atomic>

namespace tepegoz
{

class Tepegoz : public ITepegoz
{
public:
    int StartUp();

    ~Tepegoz();
    Tepegoz(std::unique_ptr<TepegozParams> args);
    Tepegoz(Tepegoz&&) = delete;
    Tepegoz(const Tepegoz&) = delete;
    Tepegoz& operator=(const Tepegoz&) = delete;

    void Run() override;
    void RequestStop() override;
    int TearDown() override;

    static bool SortByDependencies(std::vector<std::string>& err, std::vector<Process>& pList);
    // Problems in the configuration go to errors when given, else to the log.
    static std::unique_ptr<TepegozParams> GenerateConfig(const config_section::ConfigValue& document, std::vector<std::string>* errors = nullptr);
    /**
     * @brief Build the services again from the document @p params was built from, as --start all does.
     * @param params The current parameters. Their document, source and origins are kept.
     * @return New parameters, with services that have not been started.
     */
    static std::unique_ptr<TepegozParams> RebuildConfig(const TepegozParams& params);
    /**
     * @brief Read params' source again and apply what changed, without stopping a service that stays (ReloadConfig).
     * @param params The current parameters. On success the services, document, origins and settings are the new ones.
     * @param retiring Receives the removed services that still have to be stopped.
     * @return What was done, or why nothing was: then params and retiring are unchanged.
     */
    static ReloadReport Reload(TepegozParams& params, std::vector<Process>& retiring);
    /**
     * @brief Answer one request of the service registry: show, add, change or remove a registered service.
     * @param params The current parameters. A change that is written is applied with Reload.
     * @param retiring Receives the services a change removed that still have to be stopped.
     * @param frames The request's frames, without the envelope: the operation, then its parameters.
     * @return The reply's status and its JSON object. Anything but ReplyStatus::Ok leaves the files as they were.
     */
    static service_registry::Reply HandleRegistry(TepegozParams& params, std::vector<Process>& retiring,
                                                  const std::vector<std::string>& frames);
private:
    void StopAll();
    static void RetireProcess(Process& process, std::vector<Process>& retiring);
    bool ApplyToAll(CommandCode command);
    bool ApplyCommandServerMessage(const CommandMessage& message);
    bool ProcessMessage(const std::vector<zmq::message_t>& messages);
    bool CheckProcessConfig(std::vector<std::string>& err) const;
    bool StartSequentialAsync() const;
    void StartDependencies(const Process& process) const;
    bool IsStopInProgress() const;
    std::vector<Process>::iterator GetProcess(std::string_view processName) const;
    void BindRegistry();
    void ServeRegistry();


    std::unique_ptr<TepegozParams> processParams;
    std::vector<Process> retiring; // disabled or replaced, still being stopped
    std::vector<std::string> errorList;
    std::atomic<bool> enabled{ true };

    zmq::context_t zmqContext;
    zmq::socket_t listener;
    zmq::socket_t statusPublisher;
    zmq::socket_t registrar; // ServiceRegistry requests, answered in the supervision loop
};

} // namespace tepegoz
