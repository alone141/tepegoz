#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace network_manager
{

// FormatIdentity's text for an identity with no bytes.
constexpr const char* empty_identity_text = "<empty>";
constexpr std::chrono::milliseconds default_summary_period{60000};
constexpr std::chrono::milliseconds default_repeat_interval{10000};

// How the broker logs. Every line starts with "Broker: " and a mark: + joined, - left, ~ taken over,
// ! not delivered or rejected, = status. A name column follows, so the lines read as a table.
struct LogSettings
{
    // Time between status lines: who is connected and how many messages each sent.
    std::chrono::milliseconds summaryPeriod = default_summary_period;
    // The same warning (same sender, destination and reason) is written at most once per interval, with
    // the count of the ones held back.
    std::chrono::milliseconds repeatInterval = default_repeat_interval;
};

// A message as the router receives it. A dealer that sends [destination][payload...] arrives as
// [source][destination][payload...]: the router puts the dealer's identity in front.
struct Envelope
{
    std::string source;
    std::string destination;
    std::vector<std::string> payload;
};

enum class EnvelopeCode
{
    Ok,
    EmptySource,
    NoDestination,
};

enum class BindCode
{
    Ok,
    Failed,
};

enum class RunCode
{
    Stopped,
    NotBound,
    Failed,
};

/**
 * @brief Split the frames the router received into source, destination and payload.
 * @param frames Frames as received: [source][destination][payload...]. An empty frame right after the
 *               source, the delimiter a REQ socket sends, is skipped.
 * @param out Receives the message on success. It is left unchanged on failure.
 * @return EnvelopeCode::Ok, EmptySource without a source, or NoDestination without a destination.
 */
EnvelopeCode ParseEnvelope(const std::vector<std::string>& frames, Envelope& out);

/**
 * @brief Build the frames that deliver @p envelope to its destination.
 * @param envelope Message to deliver.
 * @return [destination][source][payload...] for the router; the destination dealer receives
 *         [source][payload...] and replies the same way.
 */
std::vector<std::string> BuildDelivery(const Envelope& envelope);

/**
 * @brief Make an identity readable in a log line.
 * @param identity Identity bytes.
 * @return The identity when every byte is printable ASCII, otherwise its bytes in hex;
 *         empty_identity_text for an identity with no bytes.
 */
std::string FormatIdentity(std::string_view identity);

// The ZeroMQ broker of the process manager: a ROUTER socket that hands every message a dealer sends to
// the dealer it names. Services send their heartbeats through it, and the CLI and the GUI their commands.
// It logs each dealer by name: when it joins, from which address, when it leaves, and who takes a name
// over; undeliverable messages; and a status line every LogSettings::summaryPeriod.
class NetworkManager
{
public:
    /**
     * @brief Keep the endpoint to bind. Nothing is opened before Bind().
     * @param endpoint ZeroMQ endpoint, such as tcp://0.0.0.0:5557 or tcp://127.0.0.1:5557. A tcp port
     *                 of * takes a free port.
     * @param logSettings Status line period and how often a repeated warning is written.
     */
    explicit NetworkManager(std::string endpoint, LogSettings logSettings = LogSettings{});

    /**
     * @brief Close the socket. Call only once Run() has returned.
     */
    ~NetworkManager();

    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    /**
     * @brief Take over the socket of @p other. Not while Run() runs on @p other.
     * @param other Manager to move from. It is left without a socket.
     */
    NetworkManager(NetworkManager&& other) noexcept;

    /**
     * @brief Close this manager's socket and take over the one of @p other. Not while Run() runs on either.
     * @param other Manager to move from. It is left without a socket.
     * @return This manager.
     */
    NetworkManager& operator=(NetworkManager&& other) noexcept;

    /**
     * @brief Open the ROUTER socket and bind it to the endpoint.
     * @param error Receives the reason when binding fails.
     * @return BindCode::Ok on success, otherwise BindCode::Failed.
     */
    BindCode Bind(std::string& error);

    /**
     * @brief Forward messages between dealers until RequestStop() is called. One thread at a time.
     * @param error Receives the reason when Run() does not return RunCode::Stopped.
     * @return RunCode::Stopped after RequestStop(), NotBound without a successful Bind(), or Failed when
     *         ZeroMQ can no longer poll the socket.
     */
    RunCode Run(std::string& error);

    /**
     * @brief Ask Run() to return. Safe from any thread; Run() returns within one poll period, and a
     *        manager asked to stop does not run again.
     */
    void RequestStop();

    /**
     * @brief Return the endpoint: as given, or as bound once Bind() succeeds, with a * port resolved.
     * @return The endpoint; empty for a manager that was moved from.
     */
    std::string Endpoint() const;

private:
    struct State;

    // On the heap: the header names no ZeroMQ type, and the sockets stay put when the manager moves.
    std::unique_ptr<State> state;
};

} // namespace network_manager
