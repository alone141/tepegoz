#include "NetworkManager.hpp"

#include "Logger.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <zmq.h>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace network_manager
{

namespace
{

// How long Run() waits for a message before it looks at RequestStop() again.
constexpr std::chrono::milliseconds poll_period{200};
constexpr int monitored_events = ZMQ_EVENT_ACCEPTED | ZMQ_EVENT_DISCONNECTED;
constexpr const char* unknown_peer = "address=unknown port=0";

std::string Describe(const Envelope& envelope)
{
    std::size_t bytes = 0;
    for (const std::string& frame : envelope.payload)
    {
        bytes += frame.size();
    }

    return "source=" + FormatIdentity(envelope.source) + " destination=" + FormatIdentity(envelope.destination) +
           " frames=" + std::to_string(envelope.payload.size()) + " bytes=" + std::to_string(bytes);
}

// "address=<ip> port=<port>" of the far end of a connected socket; empty when it cannot be read.
std::string PeerAddress(int fd)
{
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (fd < 0 || ::getpeername(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
    {
        return {};
    }

    char address[INET6_ADDRSTRLEN] = {};
    std::uint16_t port = 0;
    if (storage.ss_family == AF_INET)
    {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&storage);
        ::inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address));
        port = ntohs(ipv4->sin_port);
    }
    else if (storage.ss_family == AF_INET6)
    {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&storage);
        ::inet_ntop(AF_INET6, &ipv6->sin6_addr, address, sizeof(address));
        port = ntohs(ipv6->sin6_port);
    }
    else
    {
        return {};
    }

    return std::string{"address="} + address + " port=" + std::to_string(port);
}

std::vector<std::string> ToStrings(const std::vector<zmq::message_t>& messages)
{
    std::vector<std::string> frames;
    frames.reserve(messages.size());
    for (const zmq::message_t& message : messages)
    {
        frames.emplace_back(static_cast<const char*>(message.data()), message.size());
    }

    return frames;
}

const char* EnvelopeCodeText(EnvelopeCode code)
{
    switch (code)
    {
    case EnvelopeCode::Ok:
        return "ok";
    case EnvelopeCode::EmptySource:
        return "empty_source";
    case EnvelopeCode::NoDestination:
        return "no_destination";
    }

    return "unknown";
}

} // namespace

EnvelopeCode ParseEnvelope(const std::vector<std::string>& frames, Envelope& out)
{
    if (frames.empty() || frames[0].empty())
    {
        return EnvelopeCode::EmptySource;
    }

    std::size_t index = 1;
    if (index < frames.size() && frames[index].empty())
    {
        ++index;
    }
    if (index >= frames.size() || frames[index].empty())
    {
        return EnvelopeCode::NoDestination;
    }

    Envelope envelope{};
    envelope.source = frames[0];
    envelope.destination = frames[index];
    envelope.payload.assign(frames.begin() + static_cast<std::ptrdiff_t>(index + 1), frames.end());
    out = std::move(envelope);
    return EnvelopeCode::Ok;
}

std::vector<std::string> BuildDelivery(const Envelope& envelope)
{
    std::vector<std::string> frames;
    frames.reserve(envelope.payload.size() + 2);
    frames.push_back(envelope.destination);
    frames.push_back(envelope.source);
    frames.insert(frames.end(), envelope.payload.begin(), envelope.payload.end());
    return frames;
}

std::string FormatIdentity(std::string_view identity)
{
    if (identity.empty())
    {
        return empty_identity_text;
    }

    bool printable = true;
    for (const char c : identity)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7E)
        {
            printable = false;
            break;
        }
    }
    if (printable)
    {
        return std::string{identity};
    }

    constexpr const char* hex_digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(identity.size() * 2);
    for (const char c : identity)
    {
        const auto byte = static_cast<unsigned char>(c);
        hex.push_back(hex_digits[byte >> 4]);
        hex.push_back(hex_digits[byte & 0x0F]);
    }

    return hex;
}

struct NetworkManager::State
{
    explicit State(std::string endpoint)
        : endpoint{std::move(endpoint)}
    {
    }

    ~State()
    {
        if (router)
        {
            ::zmq_socket_monitor(router->handle(), nullptr, 0);
        }
    }

    void ReadMonitorEvents();
    void ForwardOne();
    void Remember(const std::string& identity);
    void Forget(const std::string& identity);

    std::string endpoint;
    std::atomic<bool> stopRequested{false};
    // Declared in this order so the sockets close before their context.
    std::unique_ptr<zmq::context_t> context;
    std::unique_ptr<zmq::socket_t> router;
    std::unique_ptr<zmq::socket_t> monitor;
    std::unordered_set<std::string> knownIdentities;
    std::unordered_map<std::uint32_t, std::string> peers;
};

void NetworkManager::State::ReadMonitorEvents()
{
    while (true)
    {
        std::vector<zmq::message_t> frames;
        try
        {
            if (!zmq::recv_multipart(*monitor, std::back_inserter(frames), zmq::recv_flags::dontwait) ||
                frames.empty())
            {
                return;
            }
        }
        catch (const zmq::error_t& e)
        {
            logger::Error("Broker: reading connection events failed: %s", e.what());
            return;
        }

        // First frame: the event as uint16, then its value as uint32, here the connection's file descriptor.
        if (frames[0].size() < sizeof(std::uint16_t) + sizeof(std::uint32_t))
        {
            continue;
        }
        std::uint16_t event = 0;
        std::uint32_t fd = 0;
        std::memcpy(&event, frames[0].data(), sizeof(event));
        std::memcpy(&fd, static_cast<const char*>(frames[0].data()) + sizeof(event), sizeof(fd));

        if (event == ZMQ_EVENT_ACCEPTED)
        {
            std::string peer = PeerAddress(static_cast<int>(fd));
            if (peer.empty())
            {
                peer = unknown_peer;
            }
            logger::Info("Broker: dealer connected %s", peer.c_str());
            peers[fd] = std::move(peer);
        }
        else if (event == ZMQ_EVENT_DISCONNECTED)
        {
            std::string peer = unknown_peer;
            const auto found = peers.find(fd);
            if (found != peers.end())
            {
                peer = std::move(found->second);
                peers.erase(found);
            }
            logger::Info("Broker: dealer disconnected %s", peer.c_str());
        }
    }
}

void NetworkManager::State::ForwardOne()
{
    std::vector<zmq::message_t> received;
    try
    {
        if (!zmq::recv_multipart(*router, std::back_inserter(received), zmq::recv_flags::dontwait))
        {
            return;
        }
    }
    catch (const zmq::error_t& e)
    {
        logger::Error("Broker: receiving failed: %s", e.what());
        return;
    }

    const std::vector<std::string> frames = ToStrings(received);
    Envelope envelope{};
    const EnvelopeCode code = ParseEnvelope(frames, envelope);
    if (code != EnvelopeCode::Ok)
    {
        const std::string source = FormatIdentity(frames.empty() ? std::string_view{} : std::string_view{frames[0]});
        logger::Warning("Broker: message rejected reason=%s source=%s", EnvelopeCodeText(code), source.c_str());
        return;
    }
    Remember(envelope.source);

    std::vector<zmq::message_t> delivery;
    for (const std::string& frame : BuildDelivery(envelope))
    {
        delivery.emplace_back(frame.data(), frame.size());
    }
    const std::string summary = Describe(envelope);
    try
    {
        // Never wait: a destination that stopped reading must not hold up everyone else's messages.
        if (zmq::send_multipart(*router, delivery, zmq::send_flags::dontwait))
        {
            logger::Verbose("Broker: message forwarded %s", summary.c_str());
        }
        else
        {
            logger::Warning("Broker: message undeliverable %s reason=send_queue_full", summary.c_str());
        }
    }
    catch (const zmq::error_t& e)
    {
        if (e.num() == EHOSTUNREACH)
        {
            logger::Warning("Broker: message undeliverable %s reason=destination_unavailable", summary.c_str());
            Forget(envelope.destination);
            return;
        }
        logger::Error("Broker: sending failed: %s", e.what());
    }
}

void NetworkManager::State::Remember(const std::string& identity)
{
    if (knownIdentities.insert(identity).second)
    {
        logger::Info("Broker: dealer registered identity=%s", FormatIdentity(identity).c_str());
    }
}

void NetworkManager::State::Forget(const std::string& identity)
{
    if (knownIdentities.erase(identity) > 0)
    {
        logger::Info("Broker: dealer unregistered identity=%s", FormatIdentity(identity).c_str());
    }
}

NetworkManager::NetworkManager(std::string endpoint)
    : state{std::make_unique<State>(std::move(endpoint))}
{
}

NetworkManager::~NetworkManager() = default;

NetworkManager::NetworkManager(NetworkManager&& other) noexcept = default;

NetworkManager& NetworkManager::operator=(NetworkManager&& other) noexcept = default;

BindCode NetworkManager::Bind(std::string& error)
{
    if (!state)
    {
        error = "the network manager was moved from";
        return BindCode::Failed;
    }
    if (state->router)
    {
        error = "already bound to " + state->endpoint;
        return BindCode::Failed;
    }

    try
    {
        auto context = std::make_unique<zmq::context_t>(1);
        auto router = std::make_unique<zmq::socket_t>(*context, zmq::socket_type::router);
        // Sending to an identity that is not connected fails, so it can be logged, instead of vanishing.
        router->set(zmq::sockopt::router_mandatory, 1);
        // A dealer that reconnects under the same identity, such as a restarted service, takes it over.
        router->set(zmq::sockopt::router_handover, 1);
        router->set(zmq::sockopt::linger, 0);

        const std::string monitorEndpoint =
            "inproc://network-manager-monitor-" + std::to_string(reinterpret_cast<std::uintptr_t>(state.get()));
        if (::zmq_socket_monitor(router->handle(), monitorEndpoint.c_str(), monitored_events) != 0)
        {
            error = ::zmq_strerror(::zmq_errno());
            return BindCode::Failed;
        }
        auto monitor = std::make_unique<zmq::socket_t>(*context, zmq::socket_type::pair);
        monitor->set(zmq::sockopt::linger, 0);
        monitor->connect(monitorEndpoint);

        router->bind(state->endpoint);
        state->endpoint = router->get(zmq::sockopt::last_endpoint);
        state->context = std::move(context);
        state->router = std::move(router);
        state->monitor = std::move(monitor);
    }
    catch (const zmq::error_t& e)
    {
        error = e.what();
        return BindCode::Failed;
    }

    logger::Info("Broker: listening address=%s", state->endpoint.c_str());
    return BindCode::Ok;
}

RunCode NetworkManager::Run(std::string& error)
{
    State* const current = state.get();
    if (current == nullptr || !current->router)
    {
        error = "the network manager is not bound";
        return RunCode::NotBound;
    }

    while (!current->stopRequested.load())
    {
        zmq::pollitem_t items[] = {
            {current->router->handle(), 0, ZMQ_POLLIN, 0},
            {current->monitor->handle(), 0, ZMQ_POLLIN, 0},
        };
        try
        {
            zmq::poll(items, 2, poll_period);
        }
        catch (const zmq::error_t& e)
        {
            if (e.num() == EINTR)
            {
                continue;
            }
            error = e.what();
            logger::Error("Broker: polling failed, no longer forwarding: %s", e.what());
            return RunCode::Failed;
        }

        if ((items[1].revents & ZMQ_POLLIN) != 0)
        {
            current->ReadMonitorEvents();
        }
        if ((items[0].revents & ZMQ_POLLIN) != 0)
        {
            current->ForwardOne();
        }
    }

    logger::Info("Broker: stopped");
    return RunCode::Stopped;
}

void NetworkManager::RequestStop()
{
    if (state)
    {
        state->stopRequested.store(true);
    }
}

std::string NetworkManager::Endpoint() const
{
    return state ? state->endpoint : std::string{};
}

} // namespace network_manager
