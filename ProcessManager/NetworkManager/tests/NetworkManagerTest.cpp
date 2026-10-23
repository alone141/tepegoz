#include <gtest/gtest.h>

#include "NetworkManager.hpp"

#include <chrono>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace
{

constexpr const char* test_endpoint = "tcp://127.0.0.1:*";
constexpr int receive_timeout_ms = 2000;

// A bound manager that forwards on a thread of its own until it is stopped or the test ends.
class RunningManager
{
public:
    RunningManager()
        : manager{test_endpoint}
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
