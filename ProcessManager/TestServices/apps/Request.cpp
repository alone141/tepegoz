// pmt_request: sends one request to a ZeroMQ ROUTER, such as the manager's service registry, and prints the reply,
// one line per frame. The request's frames are the arguments after the options.

#include "TestService.hpp"

#include <chrono>
#include <cstdio>
#include <cxxopts.hpp>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace test_service
{

namespace
{

constexpr std::string_view default_registry_endpoint = "tcp://localhost:6671";
constexpr int no_reply_exit_code = 2;

} // namespace

} // namespace test_service

int main(int argc, char** argv)
{
    test_service::SetProgramName("pmt_request");
    cxxopts::Options options{"pmt_request", "Sends the frames given after the options as one request, and prints each "
                                            "frame of the reply on a line of its own."};
    options.add_options()
        ("endpoint", "Where the request goes", cxxopts::value<std::string>()->default_value(std::string{test_service::default_registry_endpoint}))
        ("timeout-ms", "Exit with 2 when no reply comes within this long", cxxopts::value<int>()->default_value("10000"))
        ("frames", "The request's frames", cxxopts::value<std::vector<std::string>>());
    options.parse_positional({"frames"});
    cxxopts::ParseResult result{};
    const test_service::OptionsCode parsed = test_service::ParseOptions(options, argc, argv, result);
    if (parsed != test_service::OptionsCode::Ok)
    {
        return test_service::ExitCodeOf(parsed);
    }

    if (result.count("frames") == 0)
    {
        std::fprintf(stderr, "pmt_request: give the request's frames after the options\n");
        return 1;
    }

    const std::vector<std::string> frames = result["frames"].as<std::vector<std::string>>();
    const int timeoutMs = result["timeout-ms"].as<int>();
    zmq::context_t context{1};
    zmq::socket_t dealer{context, zmq::socket_type::dealer};
    dealer.set(zmq::sockopt::linger, 0);
    dealer.connect(result["endpoint"].as<std::string>());
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        dealer.send(zmq::buffer(frames[i]), i + 1 < frames.size() ? zmq::send_flags::sndmore : zmq::send_flags::none);
    }

    zmq::pollitem_t items[] = {{dealer.handle(), 0, ZMQ_POLLIN, 0}};
    zmq::poll(items, 1, std::chrono::milliseconds{timeoutMs});
    if ((items[0].revents & ZMQ_POLLIN) == 0)
    {
        std::fprintf(stderr, "pmt_request: no reply within %d ms\n", timeoutMs);
        return test_service::no_reply_exit_code;
    }

    std::vector<zmq::message_t> reply;
    if (!zmq::recv_multipart(dealer, std::back_inserter(reply)).has_value())
    {
        std::fprintf(stderr, "pmt_request: the reply could not be read\n");
        return test_service::no_reply_exit_code;
    }

    for (const zmq::message_t& frame : reply)
    {
        std::printf("%s\n", frame.to_string().c_str());
    }

    return 0;
}
