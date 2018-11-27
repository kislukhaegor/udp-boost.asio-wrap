#include <ctime>
#include <iostream>
#include <string>
#include <boost/array.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/asio.hpp>

#include "Message.hpp"
#include "MRUDPSocket.hpp"

using mrudp::Message;
using mrudp::MRUDPSocket;
using mrudp::Connection;
using boost::asio::ip::udp;
using boost::shared_ptr;

void answer(std::list<shared_ptr<Connection>>& cons) {
    while (true) {
        std::cout << "Wait for msg" << std::endl;
        std::string msg;
        std::cin >> msg;
        if (msg == "shutdown") {
            return;
        }
        bool open = false;
        for (auto& con : cons) {
            open = open || con->is_open();
            con->send_def(msg);
        }
        if (!open) {
            break;
        }
    }
}

void receive(std::string data, const udp::endpoint& recv) {
    std::cout << "Received from " << recv << " : " << data << std::endl; 
}

int main() {
    try {
        shared_ptr<MRUDPSocket> socket = boost::make_shared<MRUDPSocket>(12345, 12346);
        socket->open();

        std::list<shared_ptr<Connection>> cons;
        for (int i = 0; i < 5; ++i) {
            shared_ptr<Connection> con;
            bool complete = socket->listen(con, std::chrono::milliseconds(5000));
            if (!complete) {
                return 1;
            }
            complete = socket->accept(con, std::chrono::milliseconds(5000));
            if (!complete) {
                return 1;
            }
            con->start_async_recv(std::bind(receive, std::placeholders::_1, con->get_send_ep()));
            cons.push_back(con);
        }
        std::thread t1(answer, std::ref(cons));
        t1.join();
        socket->close();
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
    return 0;
}