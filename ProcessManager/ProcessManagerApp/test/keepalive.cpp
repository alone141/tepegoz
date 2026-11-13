#include <iostream>
#include <thread>
#include <zmq.hpp>
#include <zmq_addon.hpp>
struct CitTemplate
{
    bool temp = 42;
};
int main()
{
    constexpr std::string_view PROCESS_MANAGER_ID = "BPM";
    constexpr std::string_view keepalive2 = "keepalive";

    zmq::context_t c(1);
    zmq::socket_t s(c, zmq::socket_type::dealer);
    //std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    s.set(zmq::sockopt::routing_id, keepalive2);
    s.connect("tcp://localhost:5557");
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        s.send(zmq::buffer(PROCESS_MANAGER_ID), zmq::send_flags::sndmore);
        s.send(zmq::buffer(keepalive2), zmq::send_flags::dontwait);
        //if (i == 2) {
        //	return 5;
        //}
    }
}