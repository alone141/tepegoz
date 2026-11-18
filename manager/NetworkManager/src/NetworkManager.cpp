#include "NetworkManager.hpp"

#include "Logger.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unordered_map>
#include <utility>
#include <vector>
#include <zmq.h>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace network_manager
{

namespace
{

using Clock = std::chrono::steady_clock;
using WallClock = std::chrono::system_clock;

// How long Run() waits for a message before it looks at RequestStop(), the status and the log again.
constexpr std::chrono::milliseconds poll_period{200};
constexpr int max_messages_per_round = 1000;
constexpr int monitored_events = ZMQ_EVENT_ACCEPTED | ZMQ_EVENT_DISCONNECTED;
constexpr const char* unknown_address = "unknown address";
// Width of the name column, so that the events line up.
constexpr int name_width = 20;

// "<ip>:<port>" of the far end of a connected socket; unknown_address when it cannot be read.
std::string PeerAddress(int fd)
{
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (fd < 0 || ::getpeername(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
    {
        return unknown_address;
    }

    char address[INET6_ADDRSTRLEN] = {};
    if (storage.ss_family == AF_INET)
    {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&storage);
        ::inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address));
        return std::string{address} + ":" + std::to_string(ntohs(ipv4->sin_port));
    }
    if (storage.ss_family == AF_INET6)
    {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&storage);
        ::inet_ntop(AF_INET6, &ipv6->sin6_addr, address, sizeof(address));
        return "[" + std::string{address} + "]:" + std::to_string(ntohs(ipv6->sin6_port));
    }

    return unknown_address;
}

// The descriptor of the connection a message came in on; -1 when libzmq cannot tell. Every frame is asked:
// after a poll the router rebuilds the name frame without it, while the frames read from the
// connection keep it.
int SourceFd(const std::vector<zmq::message_t>& frames)
{
#ifdef ZMQ_SRCFD
    for (const zmq::message_t& frame : frames)
    {
        const int fd = ::zmq_msg_get(frame.handle(), ZMQ_SRCFD);
        if (fd >= 0)
        {
            return fd;
        }
    }
#else
    (void)frames;
#endif
    return -1;
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

std::string Count(std::uint64_t count, const char* one, const char* many)
{
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// The text of the name column, padded so that what follows lines up.
std::string Column(std::string text)
{
    if (text.size() < static_cast<std::size_t>(name_width))
    {
        text.resize(static_cast<std::size_t>(name_width), ' ');
    }

    return text;
}

// 45s, 12m03s, 5h07m or 2d03h.
std::string FormatDuration(Clock::duration elapsed)
{
    const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    char text[32] = {};
    if (seconds < 60)
    {
        std::snprintf(text, sizeof(text), "%llds", seconds);
    }
    else if (seconds < 3600)
    {
        std::snprintf(text, sizeof(text), "%lldm%02llds", seconds / 60, seconds % 60);
    }
    else if (seconds < 86400)
    {
        std::snprintf(text, sizeof(text), "%lldh%02lldm", seconds / 3600, seconds % 3600 / 60);
    }
    else
    {
        std::snprintf(text, sizeof(text), "%lldd%02lldh", seconds / 86400, seconds % 86400 / 3600);
    }

    return text;
}

std::string JsonString(std::string_view text)
{
    std::string json{"\""};
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            json += '\\';
            json += c;
        }
        else if (byte < 0x20)
        {
            char escaped[8] = {};
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned int>(byte));
            json += escaped;
        }
        else
        {
            json += c;
        }
    }
    json += '"';
    return json;
}

// Milliseconds since the Unix epoch, as JSON; 0 for a time never set.
std::string JsonTime(WallClock::time_point when)
{
    if (when == WallClock::time_point{})
    {
        return "0";
    }

    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count());
}

const char* LevelName(logger::Level level)
{
    switch (level)
    {
    case logger::Level::Verbose:
        return "verbose";
    case logger::Level::Debug:
        return "debug";
    case logger::Level::Info:
        return "info";
    case logger::Level::Warning:
        return "warning";
    case logger::Level::Error:
        return "error";
    }

    return "info";
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
    // One TCP connection of a dealer, by its file descriptor.
    struct Connection
    {
        std::string address;
        // Empty until the connection sends its first message.
        std::string identity;
        Clock::time_point since;
        WallClock::time_point connectedAt;
        WallClock::time_point lastMessageAt;
        std::uint64_t messages = 0;
    };

    // A warning that may repeat: when it was last written, and how many were held back since.
    struct Repeat
    {
        bool written = false;
        Clock::time_point lastWritten;
        std::uint64_t heldBack = 0;
    };

    State(std::string endpoint, MonitorSettings monitorSettings)
        : endpoint{std::move(endpoint)},
          settings{std::move(monitorSettings)}
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
    bool ForwardOne();
    void ApplyPendingCloses(Clock::time_point now);
    void Accepted(int fd, Clock::time_point now);
    void Disconnected(int fd, Clock::time_point now);
    void Seen(const std::string& identity, int fd, Clock::time_point now);
    void Left(const Connection& connection, Clock::time_point now);
    void Note(logger::Level level, char mark, const char* kind, const std::string& name, const std::string& text);
    void Warn(const std::string& key, const char* kind, const std::string& name, const std::string& text,
              Clock::time_point now);
    void WriteSummaryIfDue(Clock::time_point now);
    void PublishStatusIfDue(Clock::time_point now);
    void Publish(const char* topic, const std::string& json);

    std::string endpoint;
    MonitorSettings settings;
    std::string statusEndpoint;
    std::atomic<bool> stopRequested{false};
    // Declared in this order so the sockets close before their context.
    std::unique_ptr<zmq::context_t> context;
    std::unique_ptr<zmq::socket_t> router;
    std::unique_ptr<zmq::socket_t> monitor;
    std::unique_ptr<zmq::socket_t> status;
    std::unordered_map<int, Connection> connections;
    // Connections whose disconnect event was read: closed once the messages already received are forwarded,
    // since a dealer that sends and closes at once has its last message read after that event.
    std::vector<int> pendingCloses;
    // Name -> descriptor of the connection that has it; -1 when libzmq cannot tell connections apart.
    std::unordered_map<std::string, int> owners;
    std::unordered_map<std::string, std::uint64_t> sentThisPeriod;
    // Messages per name while libzmq cannot tell connections apart.
    std::unordered_map<std::string, std::uint64_t> sentWithoutConnection;
    std::unordered_map<std::string, Repeat> repeats;
    WallClock::time_point startedAt;
    Clock::time_point periodStart;
    Clock::time_point lastStatus;
    // Since the last status line in the log.
    std::uint64_t forwarded = 0;
    std::uint64_t notDelivered = 0;
    std::uint64_t rejected = 0;
    // Since Bind(), for the status publisher.
    std::uint64_t forwardedTotal = 0;
    std::uint64_t notDeliveredTotal = 0;
    std::uint64_t rejectedTotal = 0;
};

void NetworkManager::State::Note(logger::Level level, char mark, const char* kind, const std::string& name,
                                 const std::string& text)
{
    logger::Logger::Instance().Write(level, std::string{"Broker: "} + mark + " " + Column(name) + " " + text);
    if (status)
    {
        Publish("event", "{\"version\":" + std::to_string(status_version) + ",\"time\":" + JsonTime(WallClock::now()) +
                             ",\"level\":" + JsonString(LevelName(level)) + ",\"kind\":" + JsonString(kind) +
                             ",\"name\":" + JsonString(name) + ",\"text\":" + JsonString(text) + "}");
    }
}

void NetworkManager::State::Publish(const char* topic, const std::string& json)
{
    try
    {
        // A PUB socket drops what a slow reader cannot take; it never waits.
        (void)status->send(zmq::buffer(topic, std::strlen(topic)), zmq::send_flags::sndmore | zmq::send_flags::dontwait);
        (void)status->send(zmq::buffer(json), zmq::send_flags::dontwait);
    }
    catch (const zmq::error_t& e)
    {
        logger::Error("Broker: publishing the status failed: %s", e.what());
    }
}

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
        std::uint32_t value = 0;
        std::memcpy(&event, frames[0].data(), sizeof(event));
        std::memcpy(&value, static_cast<const char*>(frames[0].data()) + sizeof(event), sizeof(value));

        const int fd = static_cast<int>(value);
        if (event == ZMQ_EVENT_ACCEPTED)
        {
            // A descriptor is reused only once closed: that close comes first.
            const auto pending = std::find(pendingCloses.begin(), pendingCloses.end(), fd);
            if (pending != pendingCloses.end())
            {
                pendingCloses.erase(pending);
                Disconnected(fd, Clock::now());
            }
            Accepted(fd, Clock::now());
        }
        else if (event == ZMQ_EVENT_DISCONNECTED)
        {
            pendingCloses.push_back(fd);
        }
    }
}

void NetworkManager::State::ApplyPendingCloses(Clock::time_point now)
{
    for (const int fd : pendingCloses)
    {
        Disconnected(fd, now);
    }
    pendingCloses.clear();
}

void NetworkManager::State::Accepted(int fd, Clock::time_point now)
{
    std::string address = PeerAddress(fd);
    const auto found = connections.find(fd);
    if (found != connections.end())
    {
        if (found->second.address == address)
        {
            return; // its first message was read before this event
        }
        // The descriptor belonged to a connection whose disconnect event never came.
        const auto owner = owners.find(found->second.identity);
        if (owner != owners.end() && owner->second == fd)
        {
            owners.erase(owner);
            Left(found->second, now);
        }
        connections.erase(found);
    }

    Note(logger::Level::Debug, ' ', "connected", address, "connected, no name yet");
    connections.emplace(fd, Connection{std::move(address), {}, now, WallClock::now(), {}, 0});
}

void NetworkManager::State::Disconnected(int fd, Clock::time_point now)
{
    const auto found = connections.find(fd);
    if (found == connections.end())
    {
        Note(logger::Level::Debug, ' ', "closed", unknown_address, "closed");
        return;
    }

    const Connection connection = std::move(found->second);
    connections.erase(found);
    if (connection.identity.empty())
    {
        Note(logger::Level::Debug, ' ', "closed", connection.address, "closed before sending anything");
        return;
    }

    const std::string name = FormatIdentity(connection.identity);
    const auto owner = owners.find(connection.identity);
    if (owner == owners.end() || owner->second != fd)
    {
        Note(logger::Level::Debug, ' ', "closed", name,
             "old connection " + connection.address + " closed; the name had moved");
        return;
    }

    owners.erase(owner);
    Left(connection, now);
}

void NetworkManager::State::Seen(const std::string& identity, int fd, Clock::time_point now)
{
    ++sentThisPeriod[identity];
    const std::string name = FormatIdentity(identity);
    if (fd < 0)
    {
        ++sentWithoutConnection[identity];
        if (owners.emplace(identity, -1).second)
        {
            Note(logger::Level::Info, '+', "joined", name, "joined (libzmq does not tell its connection)");
        }
        return;
    }

    auto found = connections.find(fd);
    if (found != connections.end() && !found->second.identity.empty() && found->second.identity != identity)
    {
        // The descriptor was reused before the old connection's disconnect event was read.
        const auto owner = owners.find(found->second.identity);
        if (owner != owners.end() && owner->second == fd)
        {
            owners.erase(owner);
            Left(found->second, now);
        }
        connections.erase(found);
        found = connections.end();
    }
    if (found == connections.end())
    {
        found = connections.emplace(fd, Connection{PeerAddress(fd), {}, now, WallClock::now(), {}, 0}).first;
    }

    Connection& connection = found->second;
    ++connection.messages;
    connection.lastMessageAt = WallClock::now();
    if (!connection.identity.empty())
    {
        return;
    }

    connection.identity = identity;
    const auto owner = owners.find(identity);
    if (owner != owners.end() && owner->second != fd)
    {
        const auto previous = connections.find(owner->second);
        const std::string previousAddress = previous != connections.end() ? previous->second.address : unknown_address;
        owner->second = fd;
        Note(logger::Level::Warning, '~', "takeover", name,
             "taken over by " + connection.address + " while " + previousAddress +
                 " is still connected (restarted, or two dealers use this name)");
        return;
    }

    owners[identity] = fd;
    Note(logger::Level::Info, '+', "joined", name, "joined from " + connection.address);
}

void NetworkManager::State::Left(const Connection& connection, Clock::time_point now)
{
    Note(logger::Level::Info, '-', "left", FormatIdentity(connection.identity),
         "left " + connection.address + " after " + FormatDuration(now - connection.since) + ", " +
             Count(connection.messages, "message", "messages"));
}

void NetworkManager::State::Warn(const std::string& key, const char* kind, const std::string& name,
                                 const std::string& text, Clock::time_point now)
{
    Repeat& repeat = repeats[key];
    if (repeat.written && now - repeat.lastWritten < settings.repeatInterval)
    {
        ++repeat.heldBack;
        return;
    }

    const std::string held = repeat.heldBack > 0 ? " (" + Count(repeat.heldBack, "more", "more") + " like it not shown)" : "";
    Note(logger::Level::Warning, '!', kind, name, text + held);
    repeat = Repeat{true, now, 0};
}

void NetworkManager::State::WriteSummaryIfDue(Clock::time_point now)
{
    if (now - periodStart < settings.summaryPeriod)
    {
        return;
    }

    std::vector<std::string> names;
    names.reserve(owners.size());
    for (const auto& owner : owners)
    {
        names.push_back(owner.first);
    }
    std::sort(names.begin(), names.end());

    std::string sent;
    for (const std::string& name : names)
    {
        const auto count = sentThisPeriod.find(name);
        sent += (sent.empty() ? "" : ", ") + FormatIdentity(name) + " " +
                std::to_string(count != sentThisPeriod.end() ? count->second : 0);
    }
    // A dealer is named by its first message; one that only receives stays unnamed.
    std::string waiting;
    for (const auto& entry : connections)
    {
        if (entry.second.identity.empty())
        {
            waiting += (waiting.empty() ? " (without a name: " : ", ") + entry.second.address;
        }
    }
    waiting += waiting.empty() ? "" : ")";
    const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(now - periodStart).count();

    logger::Info("Broker: = %zu connected%s | last %llds: %s, %llu not delivered, %llu rejected | sent: %s",
                 names.size(), waiting.c_str(), seconds, Count(forwarded, "forwarded", "forwarded").c_str(),
                 static_cast<unsigned long long>(notDelivered), static_cast<unsigned long long>(rejected),
                 sent.empty() ? "-" : sent.c_str());

    periodStart = now;
    forwarded = 0;
    notDelivered = 0;
    rejected = 0;
    sentThisPeriod.clear();
    for (auto entry = repeats.begin(); entry != repeats.end();)
    {
        const bool quiet = entry->second.heldBack == 0 && now - entry->second.lastWritten >= settings.repeatInterval;
        entry = quiet ? repeats.erase(entry) : std::next(entry);
    }
}

void NetworkManager::State::PublishStatusIfDue(Clock::time_point now)
{
    if (!status || now - lastStatus < settings.statusPeriod)
    {
        return;
    }
    lastStatus = now;

    std::vector<std::string> names;
    names.reserve(owners.size());
    for (const auto& owner : owners)
    {
        names.push_back(owner.first);
    }
    std::sort(names.begin(), names.end());

    std::string dealers;
    for (const std::string& name : names)
    {
        const auto connection = connections.find(owners[name]);
        const bool known = connection != connections.end();
        const auto sent = sentWithoutConnection.find(name);
        const std::uint64_t messages = known ? connection->second.messages
                                             : (sent != sentWithoutConnection.end() ? sent->second : 0);
        dealers += std::string{dealers.empty() ? "" : ","} + "{\"name\":" + JsonString(FormatIdentity(name)) +
                   ",\"address\":" + JsonString(known ? connection->second.address : unknown_address) +
                   ",\"since\":" + JsonTime(known ? connection->second.connectedAt : WallClock::time_point{}) +
                   ",\"messages\":" + std::to_string(messages) +
                   ",\"lastMessage\":" + JsonTime(known ? connection->second.lastMessageAt : WallClock::time_point{}) +
                   "}";
    }
    std::string unnamed;
    for (const auto& entry : connections)
    {
        if (entry.second.identity.empty())
        {
            unnamed += std::string{unnamed.empty() ? "" : ","} + "{\"address\":" + JsonString(entry.second.address) +
                       ",\"since\":" + JsonTime(entry.second.connectedAt) + "}";
        }
    }

    Publish("status", "{\"version\":" + std::to_string(status_version) + ",\"endpoint\":" + JsonString(endpoint) +
                          ",\"started\":" + JsonTime(startedAt) + ",\"now\":" + JsonTime(WallClock::now()) +
                          ",\"totals\":{\"forwarded\":" + std::to_string(forwardedTotal) +
                          ",\"notDelivered\":" + std::to_string(notDeliveredTotal) +
                          ",\"rejected\":" + std::to_string(rejectedTotal) + "},\"dealers\":[" + dealers +
                          "],\"unnamed\":[" + unnamed + "]}");
}

bool NetworkManager::State::ForwardOne()
{
    std::vector<zmq::message_t> received;
    try
    {
        if (!zmq::recv_multipart(*router, std::back_inserter(received), zmq::recv_flags::dontwait))
        {
            return false;
        }
    }
    catch (const zmq::error_t& e)
    {
        logger::Error("Broker: receiving failed: %s", e.what());
        return false;
    }

    const Clock::time_point now = Clock::now();
    const std::vector<std::string> frames = ToStrings(received);
    Envelope envelope{};
    const EnvelopeCode code = ParseEnvelope(frames, envelope);
    if (code != EnvelopeCode::Ok)
    {
        ++rejected;
        ++rejectedTotal;
        const std::string source = FormatIdentity(frames.empty() ? std::string_view{} : std::string_view{frames[0]});
        const std::string reason = code == EnvelopeCode::EmptySource ? "no sender name" : "no destination frame";
        Warn("rejected|" + reason + "|" + source, "rejected", source, "rejected: " + reason, now);
        return true;
    }

    // A connect event still on its way would otherwise be read after this message.
    ReadMonitorEvents();
    Seen(envelope.source, SourceFd(received), now);

    std::vector<zmq::message_t> delivery;
    std::size_t bytes = 0;
    for (const std::string& frame : BuildDelivery(envelope))
    {
        delivery.emplace_back(frame.data(), frame.size());
    }
    for (const std::string& frame : envelope.payload)
    {
        bytes += frame.size();
    }
    const std::string destination = FormatIdentity(envelope.destination);
    const std::string route = FormatIdentity(envelope.source) + " -> " + destination;
    try
    {
        // Never wait: a destination that stopped reading must not hold up everyone else's messages.
        if (zmq::send_multipart(*router, delivery, zmq::send_flags::dontwait))
        {
            ++forwarded;
            ++forwardedTotal;
            logger::Verbose("Broker:   %-*s %s, %s", name_width, route.c_str(),
                            Count(envelope.payload.size(), "frame", "frames").c_str(),
                            Count(bytes, "byte", "bytes").c_str());
            return true;
        }

        ++notDelivered;
        ++notDeliveredTotal;
        Warn("full|" + envelope.source + "|" + envelope.destination, "notDelivered", route,
             "not delivered: the queue of " + destination + " is full", now);
    }
    catch (const zmq::error_t& e)
    {
        if (e.num() != EHOSTUNREACH)
        {
            logger::Error("Broker: sending failed: %s", e.what());
            return true;
        }
        ++notDelivered;
        ++notDeliveredTotal;
        Warn("absent|" + envelope.source + "|" + envelope.destination, "notDelivered", route,
             "not delivered: " + destination + " is not connected", now);
    }
    return true;
}

NetworkManager::NetworkManager(std::string endpoint, MonitorSettings monitorSettings)
    : state{std::make_unique<State>(std::move(endpoint), std::move(monitorSettings))}
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

        if (!state->settings.statusEndpoint.empty())
        {
            auto status = std::make_unique<zmq::socket_t>(*context, zmq::socket_type::pub);
            status->set(zmq::sockopt::linger, 0);
            try
            {
                status->bind(state->settings.statusEndpoint);
                state->statusEndpoint = status->get(zmq::sockopt::last_endpoint);
                state->status = std::move(status);
            }
            catch (const zmq::error_t& e)
            {
                logger::Error("Broker: no status published, binding %s failed: %s",
                              state->settings.statusEndpoint.c_str(), e.what());
            }
        }

        state->context = std::move(context);
        state->router = std::move(router);
        state->monitor = std::move(monitor);
    }
    catch (const zmq::error_t& e)
    {
        state->status.reset();
        state->statusEndpoint.clear();
        error = e.what();
        return BindCode::Failed;
    }

    state->startedAt = WallClock::now();
    logger::Info("Broker: listening on %s", state->endpoint.c_str());
    if (state->status)
    {
        logger::Info("Broker: publishing its status on %s", state->statusEndpoint.c_str());
    }
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

    current->periodStart = Clock::now();
    current->lastStatus = current->periodStart - current->settings.statusPeriod; // the first status goes out now
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
        // Every message received so far, then the closes read before them; a bound keeps a flood of
        // messages from holding up RequestStop() and the status.
        int forwardedNow = 0;
        while (forwardedNow < max_messages_per_round && current->ForwardOne())
        {
            ++forwardedNow;
        }
        if (forwardedNow < max_messages_per_round)
        {
            current->ApplyPendingCloses(Clock::now());
        }
        const Clock::time_point now = Clock::now();
        current->WriteSummaryIfDue(now);
        current->PublishStatusIfDue(now);
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

std::string NetworkManager::StatusEndpoint() const
{
    return state ? state->statusEndpoint : std::string{};
}

} // namespace network_manager
