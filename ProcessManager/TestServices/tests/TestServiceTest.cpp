#include <gtest/gtest.h>

#include "TestService.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cxxopts.hpp>
#include <string>
#include <utility>
#include <vector>
#include <zmq.hpp>

namespace
{

constexpr std::chrono::milliseconds receive_wait{100};
constexpr int beat_attempts = 30;

std::vector<char*> Arguments(std::vector<std::string>& texts)
{
    std::vector<char*> arguments{};
    for (std::string& text : texts)
    {
        arguments.push_back(text.data());
    }

    return arguments;
}

test_service::OptionsCode Parse(std::vector<std::string> texts, cxxopts::ParseResult& out)
{
    cxxopts::Options options{"pmt_test", "test"};
    options.add_options()("period-ms", "period", cxxopts::value<int>()->default_value("1000"));
    std::vector<char*> arguments = Arguments(texts);
    return test_service::ParseOptions(options, static_cast<int>(arguments.size()), arguments.data(), out);
}

} // namespace

TEST(TestServiceTest, ParseOptionsReadsKnownOption)
{
    cxxopts::ParseResult result{};
    EXPECT_EQ(Parse({"pmt_test", "--period-ms", "250"}, result), test_service::OptionsCode::Ok);
    EXPECT_EQ(result["period-ms"].as<int>(), 250);
}

TEST(TestServiceTest, ParseOptionsReportsHelp)
{
    cxxopts::ParseResult result{};
    const test_service::OptionsCode code = Parse({"pmt_test", "--help"}, result);
    EXPECT_EQ(code, test_service::OptionsCode::Help);
    EXPECT_EQ(test_service::ExitCodeOf(code), 0);
}

TEST(TestServiceTest, ParseOptionsRejectsUnknownOption)
{
    cxxopts::ParseResult result{};
    const test_service::OptionsCode code = Parse({"pmt_test", "--no-such-option"}, result);
    EXPECT_EQ(code, test_service::OptionsCode::Invalid);
    EXPECT_EQ(test_service::ExitCodeOf(code), test_service::exit_invalid_options);
}

TEST(TestServiceTest, ParseOptionsRejectsOptionWithoutValue)
{
    cxxopts::ParseResult result{};
    EXPECT_EQ(Parse({"pmt_test", "--period-ms"}, result), test_service::OptionsCode::Invalid);
}

TEST(TestServiceTest, SleepWithoutStopRunsItsTime)
{
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(test_service::SleepUnlessStopped(std::chrono::milliseconds{50}));
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds{50});
}

TEST(TestServiceTest, TermInStopModeRequestsStop)
{
    // In a child process: the stop flag stays set for the rest of the process.
    EXPECT_EXIT(
        {
            test_service::InstallSignalHandlers(test_service::TermMode::Stop);
            std::raise(SIGTERM);
            const bool woke = !test_service::SleepUnlessStopped(std::chrono::milliseconds{5000});
            std::exit(test_service::StopRequested() && woke ? 3 : 4);
        },
        testing::ExitedWithCode(3), "");
}

TEST(TestServiceTest, TermInCountModeIsCountedNotStopping)
{
    EXPECT_EXIT(
        {
            test_service::InstallSignalHandlers(test_service::TermMode::Count);
            std::raise(SIGTERM);
            std::raise(SIGTERM);
            std::exit(!test_service::StopRequested() && test_service::TermCount() == 2 ? 3 : 4);
        },
        testing::ExitedWithCode(3), "");
}

TEST(TestServiceTest, IntInCountModeStillRequestsStop)
{
    EXPECT_EXIT(
        {
            test_service::InstallSignalHandlers(test_service::TermMode::Count);
            std::raise(SIGINT);
            std::exit(test_service::StopRequested() && test_service::TermCount() == 0 ? 3 : 4);
        },
        testing::ExitedWithCode(3), "");
}

TEST(TestServiceTest, HeartbeatReachesManagerThroughRouter)
{
    zmq::context_t context{1};
    zmq::socket_t router{context, zmq::socket_type::router};
    router.set(zmq::sockopt::linger, 0);
    router.set(zmq::sockopt::rcvtimeo, static_cast<int>(receive_wait.count()));
    router.bind("tcp://127.0.0.1:*");
    const std::string endpoint = router.get(zmq::sockopt::last_endpoint);

    test_service::HeartbeatSender sender{"svc_a", endpoint};
    // A beat sent before the connection is made is dropped, so beat until one arrives.
    zmq::message_t identity{};
    bool received = false;
    for (int attempt = 0; attempt < beat_attempts && !received; ++attempt)
    {
        sender.Beat();
        received = router.recv(identity).has_value();
    }

    ASSERT_TRUE(received);
    EXPECT_EQ(identity.to_string(), "svc_a");
    zmq::message_t destination{};
    zmq::message_t payload{};
    ASSERT_TRUE(router.recv(destination).has_value());
    ASSERT_TRUE(router.recv(payload).has_value());
    EXPECT_EQ(destination.to_string(), std::string{test_service::manager_id});
    EXPECT_EQ(payload.to_string(), "svc_a");
}

TEST(TestServiceTest, MovedFromSenderDropsBeats)
{
    test_service::HeartbeatSender first{"svc_b", "tcp://127.0.0.1:1"};
    test_service::HeartbeatSender second{std::move(first)};
    EXPECT_EQ(first.Beat(), test_service::BeatCode::Dropped);
}
