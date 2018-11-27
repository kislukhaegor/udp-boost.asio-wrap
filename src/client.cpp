#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <future>
#include <iostream>

#include "MRUDPSocket.hpp"


using mrudp::Message;
using mrudp::MRUDPSocket;
using boost::asio::ip::udp;
using mrudp::Connection;
using boost::shared_ptr;


void receive(std::vector<char>& data, const udp::endpoint& recv) {
    std::cout << "Received from " << recv << data.data() << std::endl; 
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 3)
        {
          std::cerr << "Usage: client <host send> <host recv>" << std::endl;
          return 1;
        }

        shared_ptr<MRUDPSocket> socket = boost::make_shared<MRUDPSocket> (atoi(argv[1]), atoi(argv[2]));
        
        socket->open();
        
        shared_ptr<Connection> con;
        bool complete = socket->connect(udp::endpoint(udp::v4(), 12346), con, std::chrono::milliseconds(5000));
        if (!complete) {
            return 1;
        }
        
        std::string msg("Hello");
        while(con->is_open()) {
            std::string data;
            bool complete = con->recv_data(data, std::chrono::milliseconds(1000));
            if (complete) {
                std::cout << data << std::endl;
                con->send_req(msg + data);
            }
        }
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }

    return 0;
}