#include <ctime>
#include <iostream>
#include <string>
#include <boost/array.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/asio.hpp>
#include <sstream>

#include "Message.hpp"
#include "MRUDPSocket.hpp"

using mrudp::Message;
using mrudp::MRUDPSocket;
using mrudp::Connection;
using boost::asio::ip::udp;
using boost::shared_ptr;
using std::chrono::steady_clock;
using std::chrono::milliseconds;

void answer(std::set<shared_ptr<Connection>>& cons) {
    size_t i = 0;
    std::string msg("Hello world!");
    while (!cons.empty()) {
        auto t = steady_clock::now();
        std::stringstream ss;
        ss << i++;
        for (auto& con : cons) {
            if (con->is_open()) {
                con->send_def(msg);
            } else {
                cons.erase(cons.find(con));
            }
        }

        auto delta = std::chrono::duration_cast<milliseconds>(t - steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(1000/60) - delta);
    }
}

void receive(std::string data, const udp::endpoint& recv) {
    std::cout << "Received from " << recv << " : " << data << std::endl; 
}
void accept_handler(std::set<shared_ptr<Connection>>& cons, shared_ptr<Connection> con) {
    con->start_async_recv(std::bind(receive, std::placeholders::_1, con->get_send_ep()));
    cons.insert(con);
}

void disconnect_handler(std::set<shared_ptr<Connection>>& cons, shared_ptr<Connection> con) {
    if (!con) {
        return;
    }
    std::cout << con->get_send_ep() << " " << "disconnected" << std::endl;
    auto it = cons.find(con);
    if (it != cons.end()) {
        cons.erase(it);
    }
}

int main() {
    try {
        shared_ptr<MRUDPSocket> socket = boost::make_shared<MRUDPSocket>();
        socket->open();

        std::set<shared_ptr<Connection>> cons;
        socket->set_accept_handler(std::bind(accept_handler, std::ref(cons), std::placeholders::_1));
        socket->set_disconnect_handler(std::bind(disconnect_handler, std::ref(cons), std::placeholders::_1));
        socket->async_listen_and_accept(5, milliseconds(10000));
        answer(cons);
        socket->close();
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
    return 0;
}