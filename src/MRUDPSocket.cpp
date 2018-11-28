#include <iostream>
#include <vector>

#include "MRUDPSocket.hpp"
#include "Message.hpp"

using mrudp::MRUDPSocket;
using mrudp::Message;
using mrudp::Connection;
using boost::asio::ip::udp;
using boost::asio::io_context;

static const std::chrono::milliseconds recv_timeout(3000);
static const std::chrono::milliseconds conn_timeout(12000);
static const uint32_t max_in_accepted_nums(1024);

Connection::Connection(const udp::endpoint& send_endpoint,
                       const udp::endpoint& recv_endpoint,
                       MRUDPSocket* socket)
    : open_(false),
      is_async_recv_(false),
      current_seq_(1),
      connection_seq_(1),
      last_seq_recv_(0),
      last_seq_send_(0),
      send_endpoint_(send_endpoint),
      recv_endpoint_(recv_endpoint),
      socket_(socket) {
}

Connection::~Connection() {
    close();
    async_handle_conn_.wait();
}

void Connection::close() {
    if (!open_) {
        return;
    }
    open_ = false;
    if (is_async_recv_) {
        stop_async_recv();
    } else {
        read_message_.notify_all();
    }
    not_accepted_cond_.notify_all();
    async_handle_not_accepted_.wait();
    recv_wait_.notify_all();
    async_wait_recv_.wait();
}

const udp::endpoint& Connection::get_send_ep() const {
    return send_endpoint_;
}

const udp::endpoint& Connection::get_recv_ep() const {
    return recv_endpoint_;
}

void Connection::start() {
    open_ = true;
    last_recv_time_ = std::chrono::steady_clock::now();
    last_send_time_ = last_recv_time_;
    async_wait_recv_ = std::async(std::launch::async, &Connection::wait_recv, this);
    async_handle_conn_ = std::async(std::launch::async, &Connection::handle_con, this);
    async_handle_not_accepted_ = std::async(std::launch::async, &Connection::handle_not_accepted_msg, this);
}


void Connection::wait_recv() {
    while (open_) {
        std::unique_lock<std::mutex> lock(recv_wait_m_);
        recv_wait_.wait(lock,
            [this] () {
                return !buf_recv_msgs_.empty() || !open_;   // !open need to stop wait in closing
            }
        );
        if (!open_) {
            return;
        }
        int i = 0;
        while (!buf_recv_msgs_.empty()) {
            ++i;
            handle_message(buf_recv_msgs_.front());
            std::unique_lock<std::mutex> lock(buf_recv_msgs_m_);
            buf_recv_msgs_.pop_front();
            lock.unlock();
            /* Насколько я понимаю, это передав управление другому потоку, я позволю ему захватить mutex и дописать в очередь */
            std::this_thread::yield();
        }
    }
}

void Connection::async_recv_data() {
    while (is_async_recv_ && open_) {
        std::unique_lock<std::mutex> lock(read_message_m_);
        read_message_.wait(lock,
            [this] () {
                return !unreaded_messages_.empty() || !is_async_recv_;
            }
        );
        if (!is_async_recv_) {
            return;
        }
        int i = 0;
        while (!unreaded_messages_.empty()) {
            ++i;
            msg_handler_(std::move(unreaded_messages_.front()->get_data()));
            std::unique_lock<std::recursive_mutex> queue_lock(unreaded_messages_m_);
            unreaded_messages_.pop_front();
            queue_lock.unlock();
            std::this_thread::yield();
        }
    }
}

bool Connection::start_async_recv(const std::function<void(std::string data)>& msg_handler) {
    if (!open_ || is_async_recv_) {
        return false;
    }
    std::unique_lock<std::recursive_mutex> lock(async_recv_m_);
    msg_handler_ = msg_handler;
    is_async_recv_ = true;
    async_recv_ = std::async(std::launch::async, &Connection::async_recv_data, this);
    return true;
}


void Connection::stop_async_recv() {
    std::unique_lock<std::recursive_mutex> lock(async_recv_m_);
    is_async_recv_ = false;
    read_message_.notify_all();
    async_recv_.wait();
}

bool Connection::is_async_recv() const {
    return is_async_recv_;
}

void Connection::send_def(const std::string& data) {
    message_ptr msg = boost::make_shared<Message>(data);
    msg->set_packet_type(Message::PacketType::DEF);
    send_msg(msg);
}

void Connection::send_req(const std::string& data) {
    message_ptr msg = boost::make_shared<Message>(data);
    msg->set_packet_type(Message::PacketType::REQ);
    send_msg(msg);
    std::unique_lock<std::recursive_mutex> lock(not_accepted_messages_m_);
    not_accepted_messages_.insert(msg);
}

void Connection::send_seq(const std::string& data) {
    message_ptr msg = boost::make_shared<Message>(data);
    msg->set_packet_type(Message::PacketType::SEQ);
    msg->set_params(last_seq_send_);
    send_msg(msg);
    last_seq_send_ = msg->get_seq();
    std::unique_lock<std::recursive_mutex> lock(not_accepted_messages_m_);
    not_accepted_messages_.insert(msg);
}

void Connection::handle_not_accepted_msg() {
    while (open_) {
        std::unique_lock<std::mutex> lock(not_accepted_cond_m_);
        not_accepted_cond_.wait(lock,
            [this] () {
                return !not_accepted_messages_.empty() || !open_;
            }
        );
        lock.unlock();
        if (!open_) {
            return;
        }
        while (!not_accepted_messages_.empty()) {
            auto now = std::chrono::steady_clock::now();
            std::unique_lock<std::recursive_mutex> nam_lock(not_accepted_messages_m_);
            auto& by_time_set = not_accepted_messages_.get<Message::ByTime>();
            std::chrono::milliseconds sleep_time = recv_timeout;
            for (auto msg : by_time_set) {
                if (now - msg->get_time() < recv_timeout) {
                    sleep_time = recv_timeout - std::chrono::duration_cast<std::chrono::milliseconds>(now - msg->get_time());
                    break;
                }
                send_msg(msg);
            }
            std::this_thread::sleep_for(sleep_time);
        }
    }
}

bool Connection::is_open() const {
    return open_;
}

void Connection::handle_message(const shared_ptr<Message>& msg) {
    std::unique_lock<std::recursive_mutex> lock(connection_seq_m_);
    last_recv_time_ = msg->get_time();
    lock.unlock();
    if (msg->get_flags()) {
        handle_serv(msg);
    } else {
        if (mrudp::calculate_crc32(msg->get_data().data(), msg->get_data().size()) != msg->get_crc32()) {
            return;
        }

        if (msg->get_seq() > connection_seq_) {
            std::unique_lock<std::recursive_mutex> lock_con(connection_seq_m_);
            connection_seq_ = msg->get_seq();
        }
        
        if (msg->get_packet_type() == Message::PacketType::DEF) {
            if (msg->get_seq() == connection_seq_) {
                std::unique_lock<std::recursive_mutex> lock(unreaded_messages_m_);
                unreaded_messages_.push_back(msg);
                read_message_.notify_one();
            }
            return;
        }

        if (msg->get_packet_type() == Message::PacketType::REQ) {
            send_serv_msg(Message::Flag::ACK, msg->get_seq());
            std::unique_lock<std::recursive_mutex> lock_list(unreaded_messages_m_);
            bool ret = accept_msg(msg->get_seq());
            if (!ret) {
                return;
            }
            std::unique_lock<std::recursive_mutex> lock(unreaded_messages_m_);
            unreaded_messages_.push_back(msg);
            read_message_.notify_one();
            return;
        }
        
        if (msg->get_packet_type() == Message::PacketType::SEQ) {
            send_serv_msg(Message::Flag::ACK, msg->get_seq());
            bool ret = accept_msg(msg->get_seq());
            if (ret) {
                handle_seq(msg);
            }
        }
    }
}

bool Connection::accept_msg(uint32_t seq) {
    /* Check dublicate */
    if (accepted_nums.find(seq) != accepted_nums.end()) {
        return false;
    }
    send_serv_msg(Message::Flag::ACK, seq);
    if (accepted_nums.size() == max_in_accepted_nums) {
        accepted_nums.erase(accepted_nums.begin());
    }
    accepted_nums.insert(seq);
    return true;
}

void Connection::handle_seq(const message_ptr& msg) {
    if (msg->get_params() != last_seq_recv_) {
        std::unique_lock<std::recursive_mutex> lock(defered_messages_m_);
        defered_messages_.insert(msg);
        return;
    }
    std::unique_lock<std::recursive_mutex> lock(unreaded_messages_m_);
    unreaded_messages_.push_back(msg);
    read_message_.notify_one();
    lock.unlock();
    last_seq_recv_ = msg->get_seq();
    auto& msg_set = defered_messages_.get<Message::BySeq>();
    auto it = msg_set.find(msg->get_seq());
    if (it != msg_set.end()) {
        std::unique_lock<std::recursive_mutex> defer_msg_lock(defered_messages_m_);
        msg_set.erase(it);
        handle_seq(*it);
    }
}

void Connection::handle_serv(const shared_ptr<Message>& msg) {
    if (msg->get_flags() == Message::FIN) {
        close();
        return;
    }
    if (msg->get_flags() == Message::ACK) {
        std::unique_lock<std::recursive_mutex> lock(not_accepted_messages_m_);
        auto& by_seq_send = not_accepted_messages_.get<Message::BySeq>();
        by_seq_send.erase(msg->get_params());
        return;
    }
}

void Connection::handle_con() {
    std::chrono::milliseconds cur_timeout(recv_timeout);
    while(open_) {
        std::this_thread::sleep_for(cur_timeout);
        std::chrono::time_point<std::chrono::steady_clock> now = std::chrono::steady_clock::now();
        if (now - last_recv_time_ > conn_timeout) {
            if (!open_) {
                return;
            }
            send_serv_msg(Message::Flag::FIN, 0);
            close();
            if (socket_->get_disconnect_handler()) {
                socket_->get_disconnect_handler()(shared_from_this());
            }
            return;
        }
        if (now - last_send_time_ > recv_timeout) {
            shared_ptr<Message> msg = boost::make_shared<Message>();
            msg->set_flags(Message::Flag::ACK);
            send_msg(msg);
            cur_timeout = recv_timeout;
        } else {
            cur_timeout = recv_timeout -
                          (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_recv_time_) -
                           recv_timeout);
        }
    }
}

void Connection::send_msg(const shared_ptr<Message>& msg) {
    if (!open_) {
        return;
    }
    msg->set_seq(current_seq_);
    size_t length;
    socket_->send_to_impl(msg, recv_endpoint_, length);
    std::unique_lock<std::recursive_mutex> lock(connection_seq_m_);
    last_send_time_ = std::chrono::steady_clock::now();
    msg->set_time(last_send_time_);
    ++current_seq_;
}

void Connection::send_serv_msg(Message::Flag flags, uint32_t params) {
    shared_ptr<Message> msg = boost::make_shared<Message>();
    msg->set_flags(flags);
    msg->set_params(params);
    send_msg(msg);
}

MRUDPSocket::MRUDPSocket(unsigned send_port, unsigned recv_port)
    : open_(false),
      listen_notify_(false),
      connecting_notify_(false),
      send_socket_(io_context_, udp::endpoint(udp::v4(), send_port)),
      recv_socket_(io_context_, udp::endpoint(udp::v4(), recv_port)) {
}

MRUDPSocket::~MRUDPSocket() {
    close();
}

bool MRUDPSocket::is_open() const {
    return open_;
}

void MRUDPSocket::open() {
    if (open_) {
        return;
    }
    open_ = true;
    start_receive();
    std::size_t (boost::asio::io_context::*run)() = &boost::asio::io_context::run;
 
    io_future_ = std::async(std::launch::async, std::bind(run, &io_context_));
}

void MRUDPSocket::close() {
    if (!open_) {
        return;
    }
    open_ = false;
    io_context_.stop();
    io_future_.wait();
    for (auto con : connections_) {
        con->close();
    }
}

void MRUDPSocket::start_receive() {
    recv_socket_.async_receive_from(
        boost::asio::buffer(tmp_data_), tmp_ep_,
        [this] (boost::system::error_code ec, size_t bytes_recvd) {
            if (!ec && bytes_recvd >= sizeof(Message::Header) + sizeof(Message::Header::id)) {
                handle_receive(bytes_recvd);
            }
            start_receive();
        }
    );
}

void MRUDPSocket::handle_receive(size_t bytes_recvd) {
    try {
        shared_ptr<Message> msg = boost::make_shared<Message>(tmp_data_.data(), bytes_recvd);
        msg->set_time(std::chrono::steady_clock::now());
        auto& by_send_set = connections_.get<Connection::BySend>();
        auto iter = connections_.find(tmp_ep_);
        if (iter != by_send_set.end()) {
            {
                std::unique_lock<std::mutex> lock((*iter)->buf_recv_msgs_m_);
                (*iter)->buf_recv_msgs_.push_back(msg);
            }
            (*iter)->recv_wait_.notify_one();    
            return;
        }

        auto& by_send_not_accepted = not_accepted_connections_.get<Connection::BySend>();
        iter = not_accepted_connections_.find(tmp_ep_);
        if (iter != by_send_not_accepted.end()) {
            if (msg->get_flags() == Message::Flag::ACK) {
                std::unique_lock<std::mutex> lock(accept_con_mutex_);
                accept_con_ = *iter;
                std::unique_lock<std::mutex> set_lock(not_accepted_connections_m_);
                by_send_not_accepted.erase(iter);
                accept_cond_.notify_all();
                return;
            }
        }

        if (msg->get_flags() == Message::Flag::INIT) {
            std::unique_lock<std::mutex> lock(listen_message_mutex_);
            listen_message_ = msg;
            listen_endpoint_ = tmp_ep_;
            lock.unlock();
            listen_notify_ = true;
            listen_cond_.notify_one();
            return;
        
        } if (msg->get_flags() == (Message::Flag::INIT | Message::Flag::ACK)) {
            std::unique_lock<std::mutex> lock(connect_message_mutex_);
            connect_sender_ep_ = tmp_ep_;
            lock.unlock();
            connecting_notify_ = true;
            connect_cond_.notify_all();
            return;
        }
    } catch (std::invalid_argument& e) {
        return;
    }
}

const MRUDPSocket::connections_set_type& MRUDPSocket::get_connections() const {
    return connections_;
}

size_t MRUDPSocket::get_connections_count() const {
    return connections_.size();
}

bool MRUDPSocket::send_to_impl(const shared_ptr<Message>& msg,
                               const udp::endpoint& ep,
                               size_t& bytes_transffered) {
    msg->update_crc();
    shared_ptr<std::string> message = boost::make_shared<std::string>(msg->str());
    bytes_transffered = send_socket_.send_to(boost::asio::buffer(*message), ep);
    if (!bytes_transffered) {
        return false;
    }
    return true;
}

void MRUDPSocket::set_disconnect_handler(std::function<void(shared_ptr<Connection>)> handler) {
    disconnect_handler = handler;
}

void MRUDPSocket::set_accept_handler(std::function<void(shared_ptr<Connection>)> handler) {
    accept_handler = handler;
}

const std::function<void(shared_ptr<Connection>)>& MRUDPSocket::get_disconnect_handler() const {
    return disconnect_handler;
}

const std::function<void(shared_ptr<Connection>)>& MRUDPSocket::get_accept_handler() const {
    return accept_handler;
}