#include <gtest/gtest.h>

#include "NetworkManager.hpp"

#include "Logger.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace
{

constexpr const char* test_endpoint = "tcp://127.0.0.1:*";
constexpr int receive_timeout_ms = 2000;

// Points the log at a file for one test. It is read through a stream of its own, so reading does not
// race with the broker thread that writes.
class CapturedLog
{
public:
    explicit CapturedLog(logger::Level minimum = logger::Level::Debug)
        : path{std::filesystem::temp_directory_path() /
               ("network_manager_test_" + std::to_string(::getpid()) + ".log")},
          stream{std::fopen(path.c_str(), "w")}
    {
        logger::Logger::Instance().SetMinimum(minimum);
        logger::Logger::Instance().SetConsole(stream, false);
    }

    ~CapturedLog()
    {
        logger::Logger::Instance().SetConsole(nullptr, false);
        logger::Logger::Instance().SetMinimum(logger::Level::Debug);
        std::fclose(stream);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;

    std::string Text() const
    {
        std::ifstream file{path};
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    // Waits up to two seconds for @p text to be logged.
    bool WaitFor(const std::string& text) const
    {
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            if (Text().find(text) != std::string::npos)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        return false;
    }

    std::size_t Count(const std::string& text) const
    {
        const std::string all = Text();
        std::size_t count = 0;
        for (std::size_t at = all.find(text); at != std::string::npos; at = all.find(text, at + text.size()))
        {
            ++count;
        }

        return count;
    }

private:
    std::filesystem::path path;
    std::FILE* stream;
};

// A bound manager that forwards on a thread of its own until it is stopped or the test ends.
class RunningManager
{
public:
    explicit RunningManager(network_manager::MonitorSettings monitorSettings = network_manager::MonitorSettings{})
        : manager{test_endpoint, monitorSettings}
    {
        std::string error;
        bound = manager.Bind(error) == network_manager::BindCode::Ok;
        if (bound)
        {
            loop = std::thread{[this] {
                std::string runError;
                result = manager.Run(runError);
            }};
        }
    }

    ~RunningManager()
    {
        Stop();
    }

    RunningManager(const RunningManager&) = delete;
    RunningManager& operator=(const RunningManager&) = delete;

    network_manager::RunCode Stop()
    {
        manager.RequestStop();
        if (loop.joinable())
        {
            loop.join();
        }

        return result;
    }

    bool Bound() const
    {
        return bound;
    }

    std::string Endpoint() const
    {
        return manager.Endpoint();
    }

    std::string StatusEndpoint() const
    {
        return manager.StatusEndpoint();
    }

private:
    network_manager::NetworkManager manager;
    bool bound = false;
    network_manager::RunCode result = network_manager::RunCode::NotBound;
    std::thread loop;
};

zmq::socket_t MakeDealer(zmq::context_t& context, const std::string& identity, const std::string& endpoint)
{
    zmq::socket_t dealer{context, zmq::socket_type::dealer};
    dealer.set(zmq::sockopt::linger, 0);
    dealer.set(zmq::sockopt::rcvtimeo, receive_timeout_ms);
    dealer.set(zmq::sockopt::routing_id, identity);
    dealer.connect(endpoint);
    return dealer;
}

void Send(zmq::socket_t& dealer, const std::vector<std::string>& frames)
{
    std::vector<zmq::message_t> messages;
    for (const std::string& frame : frames)
    {
        messages.emplace_back(frame.data(), frame.size());
    }
    zmq::send_multipart(dealer, messages);
}

// The frames of the next message; empty when none arrives within the receive timeout.
std::vector<std::string> Receive(zmq::socket_t& dealer)
{
    std::vector<zmq::message_t> messages;
    if (!zmq::recv_multipart(dealer, std::back_inserter(messages)))
    {
        return {};
    }

    std::vector<std::string> frames;
    for (const zmq::message_t& message : messages)
    {
        frames.emplace_back(static_cast<const char*>(message.data()), message.size());
    }

    return frames;
}

// Messages to a dealer the broker has not accepted yet are dropped, so ping itself until one comes back.
bool WaitUntilRoutable(zmq::socket_t& dealer, const std::string& identity)
{
    zmq::pollitem_t item{dealer.handle(), 0, ZMQ_POLLIN, 0};
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        Send(dealer, {identity, "ping"});
        if (zmq::poll(&item, 1, std::chrono::milliseconds{100}) == 0)
        {
            continue;
        }
        while (zmq::poll(&item, 1, std::chrono::milliseconds{50}) > 0)
        {
            Receive(dealer);
        }

        return true;
    }

    return false;
}

zmq::socket_t MakeSubscriber(zmq::context_t& context, const std::string& endpoint)
{
    zmq::socket_t subscriber{context, zmq::socket_type::sub};
    subscriber.set(zmq::sockopt::linger, 0);
    subscriber.set(zmq::sockopt::rcvtimeo, receive_timeout_ms);
    subscriber.set(zmq::sockopt::subscribe, "");
    subscriber.connect(endpoint);
    return subscriber;
}

// Reads published messages until one on @p topic contains @p text; gives up after 30 messages or a timeout.
bool WaitForPublished(zmq::socket_t& subscriber, const std::string& topic, const std::string& text,
                      std::string* json = nullptr)
{
    for (int message = 0; message < 30; ++message)
    {
        const std::vector<std::string> frames = Receive(subscriber);
        if (frames.empty())
        {
            return false;
        }
        if (frames.size() == 2 && frames[0] == topic && frames[1].find(text) != std::string::npos)
        {
            if (json != nullptr)
            {
                *json = frames[1];
            }
            return true;
        }
    }

    return false;
}

network_manager::MonitorSettings Publishing()
{
    network_manager::MonitorSettings settings{};
    settings.statusEndpoint = "tcp://127.0.0.1:*";
    settings.statusPeriod = std::chrono::milliseconds{100};
    return settings;
}

} // namespace

TEST(NetworkManagerTest, ParsesDealerMessage)
{
    network_manager::Envelope out{};
    EXPECT_EQ(network_manager::ParseEnvelope({"A", "B", "x"}, out), network_manager::EnvelopeCode::Ok);
    EXPECT_EQ(out.source, "A");
    EXPECT_EQ(out.destination, "B");
    ASSERT_EQ(out.payload.size(), 1u);
    EXPECT_EQ(out.payload[0], "x");
}

TEST(NetworkManagerTest, SkipsEmptyDelimiterAfterSource)
{
    network_manager::Envelope out{};
    EXPECT_EQ(network_manager::ParseEnvelope({"auth", "", "ledger", "hello"}, out), network_manager::EnvelopeCode::Ok);
    EXPECT_EQ(out.source, "auth");
    EXPECT_EQ(out.destination, "ledger");
    ASSERT_EQ(out.payload.size(), 1u);
    EXPECT_EQ(out.payload[0], "hello");
}

TEST(NetworkManagerTest, KeepsMultipartPayloadInOrder)
{
    network_manager::Envelope out{};
    EXPECT_EQ(network_manager::ParseEnvelope({"A", "", "B", "p1", "p2"}, out), network_manager::EnvelopeCode::Ok);
    ASSERT_EQ(out.payload.size(), 2u);
    EXPECT_EQ(out.payload[0], "p1");
    EXPECT_EQ(out.payload[1], "p2");
}

TEST(NetworkManagerTest, AcceptsEmptyPayload)
{
    network_manager::Envelope out{};
    EXPECT_EQ(network_manager::ParseEnvelope({"A", "", "B"}, out), network_manager::EnvelopeCode::Ok);
    EXPECT_EQ(out.destination, "B");
    EXPECT_TRUE(out.payload.empty());
}

TEST(NetworkManagerTest, MissingDestinationReturnsNoDestination)
{
    network_manager::Envelope out{};
    EXPECT_EQ(network_manager::ParseEnvelope({"A"}, out), network_manager::EnvelopeCode::NoDestination);
    EXPECT_EQ(network_manager::ParseEnvelope({"A", ""}, out), network_manager::EnvelopeCode::NoDestination);
    EXPECT_EQ(network_manager::ParseEnvelope({"A", "", ""}, out), network_manager::EnvelopeCode::NoDestination);
}

TEST(NetworkManagerTest, EmptySourceReturnsEmptySource)
{
    network_manager::Envelope out{};
    EXPECT_EQ(network_manager::ParseEnvelope({}, out), network_manager::EnvelopeCode::EmptySource);
    EXPECT_EQ(network_manager::ParseEnvelope({"", "", "B", "x"}, out), network_manager::EnvelopeCode::EmptySource);
}

TEST(NetworkManagerTest, FailedParseLeavesOutUnchanged)
{
    network_manager::Envelope out{};
    out.source = "kept";
    EXPECT_EQ(network_manager::ParseEnvelope({"A"}, out), network_manager::EnvelopeCode::NoDestination);
    EXPECT_EQ(out.source, "kept");
}

TEST(NetworkManagerTest, BuildDeliveryPutsDestinationBeforeSource)
{
    const std::vector<std::string> frames = network_manager::BuildDelivery({"auth", "ledger", {"hello"}});
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0], "ledger");
    EXPECT_EQ(frames[1], "auth");
    EXPECT_EQ(frames[2], "hello");
}

TEST(NetworkManagerTest, BuildDeliveryKeepsMultipartPayload)
{
    const std::vector<std::string> frames = network_manager::BuildDelivery({"A", "B", {"p1", "p2"}});
    ASSERT_EQ(frames.size(), 4u);
    EXPECT_EQ(frames[2], "p1");
    EXPECT_EQ(frames[3], "p2");
}

TEST(NetworkManagerTest, FormatIdentityKeepsPrintableText)
{
    EXPECT_EQ(network_manager::FormatIdentity("auth"), "auth");
    EXPECT_EQ(network_manager::FormatIdentity(" "), " ");
}

TEST(NetworkManagerTest, FormatIdentityWritesOtherBytesAsHex)
{
    EXPECT_EQ(network_manager::FormatIdentity(std::string{"\x00\x01\xff", 3}), "0001ff");
    EXPECT_EQ(network_manager::FormatIdentity("\t"), "09");
}

TEST(NetworkManagerTest, FormatIdentityNamesEmptyIdentity)
{
    EXPECT_EQ(network_manager::FormatIdentity(""), network_manager::empty_identity_text);
}

TEST(NetworkManagerTest, BindResolvesWildcardPort)
{
    network_manager::NetworkManager manager{test_endpoint};
    std::string error;
    ASSERT_EQ(manager.Bind(error), network_manager::BindCode::Ok);
    const std::string endpoint = manager.Endpoint();
    EXPECT_EQ(endpoint.rfind("tcp://127.0.0.1:", 0), 0u);
    EXPECT_EQ(endpoint.find('*'), std::string::npos);
}

TEST(NetworkManagerTest, BindFailsForUnknownTransport)
{
    network_manager::NetworkManager manager{"bogus://nowhere"};
    std::string error;
    EXPECT_EQ(manager.Bind(error), network_manager::BindCode::Failed);
    EXPECT_FALSE(error.empty());
}

TEST(NetworkManagerTest, BindTwiceFails)
{
    network_manager::NetworkManager manager{test_endpoint};
    std::string error;
    ASSERT_EQ(manager.Bind(error), network_manager::BindCode::Ok);
    EXPECT_EQ(manager.Bind(error), network_manager::BindCode::Failed);
    EXPECT_NE(error.find("already bound"), std::string::npos);
}

TEST(NetworkManagerTest, RunWithoutBindReturnsNotBound)
{
    network_manager::NetworkManager manager{test_endpoint};
    std::string error;
    EXPECT_EQ(manager.Run(error), network_manager::RunCode::NotBound);
    EXPECT_FALSE(error.empty());
}

TEST(NetworkManagerTest, RequestStopEndsRun)
{
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    EXPECT_EQ(broker.Stop(), network_manager::RunCode::Stopped);
}

TEST(NetworkManagerTest, ForwardsMessageAndReply)
{
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    zmq::socket_t b = MakeDealer(context, "B", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(a, "A"));
    ASSERT_TRUE(WaitUntilRoutable(b, "B"));

    Send(a, {"B", "hello"});
    EXPECT_EQ(Receive(b), (std::vector<std::string>{"A", "hello"}));
    Send(b, {"A", "ok"});
    EXPECT_EQ(Receive(a), (std::vector<std::string>{"B", "ok"}));
}

TEST(NetworkManagerTest, ForwardsMultipartPayload)
{
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    zmq::socket_t b = MakeDealer(context, "B", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(b, "B"));

    Send(a, {"B", "p1", "p2"});
    EXPECT_EQ(Receive(b), (std::vector<std::string>{"A", "p1", "p2"}));
}

TEST(NetworkManagerTest, DropsMessageForUnknownDestination)
{
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    zmq::socket_t b = MakeDealer(context, "B", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(b, "B"));

    Send(a, {"missing", "nope"});
    Send(a, {"B", "still"});
    EXPECT_EQ(Receive(b), (std::vector<std::string>{"A", "still"}));
}

TEST(NetworkManagerTest, ReconnectedDealerTakesOverIdentity)
{
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    zmq::socket_t first = MakeDealer(context, "B", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(first, "B"));
    first.close();

    zmq::socket_t second = MakeDealer(context, "B", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(second, "B"));
    Send(a, {"B", "after-reconnect"});
    EXPECT_EQ(Receive(second), (std::vector<std::string>{"A", "after-reconnect"}));
}

TEST(NetworkManagerTest, MovedFromManagerIsLeftWithoutSocket)
{
    network_manager::NetworkManager first{test_endpoint};
    network_manager::NetworkManager second{std::move(first)};
    std::string error;
    EXPECT_EQ(second.Bind(error), network_manager::BindCode::Ok);
    EXPECT_FALSE(second.Endpoint().empty());
    EXPECT_TRUE(first.Endpoint().empty());
    EXPECT_EQ(first.Bind(error), network_manager::BindCode::Failed);
    EXPECT_EQ(first.Run(error), network_manager::RunCode::NotBound);
}

TEST(NetworkManagerTest, LogsJoinAndLeaveByName)
{
    const CapturedLog log;
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t dealer = MakeDealer(context, "svc", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(dealer, "svc"));
    EXPECT_TRUE(log.WaitFor("Broker: + svc"));
    EXPECT_TRUE(log.WaitFor(" joined from 127.0.0.1:"));

    dealer.close();
    EXPECT_TRUE(log.WaitFor("Broker: - svc"));
    EXPECT_TRUE(log.WaitFor(" left 127.0.0.1:"));
    EXPECT_TRUE(log.WaitFor(" message")); // 1 message or more: the pings it took to be routable
}

TEST(NetworkManagerTest, DealerThatSendsAndClosesAtOnceKeepsItsAddress)
{
    const CapturedLog log;
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t b = MakeDealer(context, "B", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(b, "B"));

    // Like the CLI: one command, then gone. Its disconnect event can be read before its message.
    zmq::socket_t quick{context, zmq::socket_type::dealer};
    quick.set(zmq::sockopt::linger, 1000);
    quick.set(zmq::sockopt::routing_id, "quick");
    quick.connect(broker.Endpoint());
    Send(quick, {"B", "once"});
    quick.close();

    EXPECT_EQ(Receive(b), (std::vector<std::string>{"quick", "once"}));
    EXPECT_TRUE(log.WaitFor("Broker: - quick"));
    EXPECT_TRUE(log.WaitFor(", 1 message\n"));
    EXPECT_EQ(log.Text().find("unknown address"), std::string::npos);
    EXPECT_EQ(log.Text().find("closed before sending anything"), std::string::npos);
}

TEST(NetworkManagerTest, LogsTakeoverWhileOldConnectionIsOpen)
{
    const CapturedLog log;
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t first = MakeDealer(context, "dup", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(first, "dup"));
    zmq::socket_t second = MakeDealer(context, "dup", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(second, "dup"));

    EXPECT_TRUE(log.WaitFor("Broker: ~ dup"));
    EXPECT_TRUE(log.WaitFor("is still connected (restarted, or two dealers use this name)"));
}

TEST(NetworkManagerTest, ConnectionThatNeverSendsStaysUnnamed)
{
    const CapturedLog log;
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t silent = MakeDealer(context, "silent", broker.Endpoint());
    EXPECT_TRUE(log.WaitFor("connected, no name yet"));

    silent.close();
    EXPECT_TRUE(log.WaitFor("closed before sending anything"));
    EXPECT_EQ(log.Text().find("Broker: + "), std::string::npos);
}

TEST(NetworkManagerTest, WritesRepeatedWarningOncePerInterval)
{
    const CapturedLog log;
    network_manager::MonitorSettings settings{};
    settings.repeatInterval = std::chrono::milliseconds{60000};
    RunningManager broker{settings};
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    zmq::socket_t b = MakeDealer(context, "B", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(b, "B"));

    for (int i = 0; i < 5; ++i)
    {
        Send(a, {"missing", "x"});
    }
    Send(a, {"B", "after"});
    EXPECT_EQ(Receive(b), (std::vector<std::string>{"A", "after"}));
    EXPECT_EQ(log.Count("not delivered: missing is not connected"), 1u);
}

TEST(NetworkManagerTest, CountsHeldBackWarningsWhenWritingAgain)
{
    const CapturedLog log;
    network_manager::MonitorSettings settings{};
    settings.repeatInterval = std::chrono::milliseconds{300};
    RunningManager broker{settings};
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(a, "A"));

    for (int i = 0; i < 3; ++i)
    {
        Send(a, {"missing", "x"});
    }
    ASSERT_TRUE(log.WaitFor("! A -> missing"));
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
    Send(a, {"missing", "x"});
    EXPECT_TRUE(log.WaitFor("not delivered: missing is not connected (2 more like it not shown)"));
}

TEST(NetworkManagerTest, WritesStatusLineWithNamesAndCounts)
{
    const CapturedLog log;
    network_manager::MonitorSettings settings{};
    settings.summaryPeriod = std::chrono::milliseconds{300};
    RunningManager broker{settings};
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    zmq::socket_t b = MakeDealer(context, "B", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(a, "A"));
    ASSERT_TRUE(WaitUntilRoutable(b, "B"));
    for (int i = 0; i < 3; ++i)
    {
        Send(a, {"B", "x"});
        Receive(b);
    }

    EXPECT_TRUE(log.WaitFor("Broker: = 2 connected | last "));
    EXPECT_TRUE(log.WaitFor(" | sent: A "));
    EXPECT_TRUE(log.WaitFor(", B "));
}

TEST(NetworkManagerTest, LogsRejectedMessage)
{
    const CapturedLog log;
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(a, "A"));

    Send(a, {""});
    EXPECT_TRUE(log.WaitFor("! A"));
    EXPECT_TRUE(log.WaitFor("rejected: no destination frame"));
}

TEST(NetworkManagerTest, LogsForwardedMessagesAtVerbose)
{
    const CapturedLog log{logger::Level::Verbose};
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    zmq::socket_t b = MakeDealer(context, "B", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(b, "B"));

    Send(a, {"B", "hello"});
    EXPECT_EQ(Receive(b), (std::vector<std::string>{"A", "hello"}));
    EXPECT_TRUE(log.WaitFor("[V] Broker:   A -> B"));
    EXPECT_TRUE(log.WaitFor("1 frame, 5 bytes"));
}

TEST(NetworkManagerTest, PublishesNothingWithoutStatusEndpoint)
{
    RunningManager broker;
    ASSERT_TRUE(broker.Bound());
    EXPECT_TRUE(broker.StatusEndpoint().empty());
}

TEST(NetworkManagerTest, StatusBindFailureKeepsTheBroker)
{
    network_manager::MonitorSettings settings{};
    settings.statusEndpoint = "bogus://nowhere";
    RunningManager broker{settings};
    ASSERT_TRUE(broker.Bound());
    EXPECT_TRUE(broker.StatusEndpoint().empty());
    zmq::context_t context{1};
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    EXPECT_TRUE(WaitUntilRoutable(a, "A"));
}

TEST(NetworkManagerTest, PublishesStatusWithEveryDealer)
{
    RunningManager broker{Publishing()};
    ASSERT_TRUE(broker.Bound());
    EXPECT_EQ(broker.StatusEndpoint().rfind("tcp://127.0.0.1:", 0), 0u);
    zmq::context_t context{1};
    zmq::socket_t subscriber = MakeSubscriber(context, broker.StatusEndpoint());
    zmq::socket_t a = MakeDealer(context, "A", broker.Endpoint());
    zmq::socket_t silent = MakeDealer(context, "silent", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(a, "A"));

    std::string json;
    ASSERT_TRUE(WaitForPublished(subscriber, "status", "\"name\":\"A\"", &json));
    EXPECT_EQ(json.rfind("{\"version\":1,\"endpoint\":\"tcp://127.0.0.1:", 0), 0u);
    EXPECT_NE(json.find("\"address\":\"127.0.0.1:"), std::string::npos);
    EXPECT_NE(json.find("\"totals\":{\"forwarded\":"), std::string::npos);
    EXPECT_TRUE(WaitForPublished(subscriber, "status", "\"unnamed\":[{\"address\":\"127.0.0.1:"));
}

TEST(NetworkManagerTest, PublishesEventsAsTheyHappen)
{
    RunningManager broker{Publishing()};
    ASSERT_TRUE(broker.Bound());
    zmq::context_t context{1};
    zmq::socket_t subscriber = MakeSubscriber(context, broker.StatusEndpoint());
    ASSERT_TRUE(WaitForPublished(subscriber, "status", "\"version\":1")); // subscribed from here on

    zmq::socket_t dealer = MakeDealer(context, "ev", broker.Endpoint());
    ASSERT_TRUE(WaitUntilRoutable(dealer, "ev"));
    std::string json;
    ASSERT_TRUE(WaitForPublished(subscriber, "event", "\"kind\":\"joined\"", &json));
    EXPECT_NE(json.find("\"level\":\"info\""), std::string::npos);
    EXPECT_NE(json.find("\"name\":\"ev\""), std::string::npos);
    EXPECT_NE(json.find("\"text\":\"joined from 127.0.0.1:"), std::string::npos);

    Send(dealer, {"nobody", "x"});
    EXPECT_TRUE(WaitForPublished(subscriber, "event", "\"kind\":\"notDelivered\""));
    dealer.close();
    EXPECT_TRUE(WaitForPublished(subscriber, "event", "\"kind\":\"left\""));
}
