#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <future>
#include <iostream>
#include <sstream>

#include "MRUDPSocket.hpp"


using mrudp::Message;
using mrudp::MRUDPSocket;
using boost::asio::ip::udp;
using mrudp::Connection;
using boost::shared_ptr;
using std::chrono::steady_clock;
using std::chrono::milliseconds;

void receive(std::string data, const udp::endpoint& recv) {
    std::cout << "Received msg from " << recv << " : " << data << std::endl; 
}

int main() {
    try {
        shared_ptr<MRUDPSocket> socket = boost::make_shared<MRUDPSocket>();
        
        socket->open();
        
        shared_ptr<Connection> con;
        bool complete = socket->connect(udp::endpoint(udp::v4(), 12345), con, std::chrono::milliseconds(5000));
        if (!complete) {
            return 1;
        }
        con->start_async_recv(std::bind(receive, std::placeholders::_1, con->get_send_ep()));
        std::string msg("Hello ");
        int i = 0;
        while(con->is_open()) {
            std::stringstream ss;
            ss << ++i;
            auto t = steady_clock::now();
            con->send_req(msg + ss.str());
            auto delta = std::chrono::duration_cast<milliseconds>(t - steady_clock::now());
            std::this_thread::sleep_for(std::chrono::milliseconds(1000/60) - delta);
        }
        std::cout << i << std::endl;
        // std::cout << "Close" << std::endl;
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }

    return 0;
}