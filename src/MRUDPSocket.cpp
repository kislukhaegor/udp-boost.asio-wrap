#include <iostream>
#include <thread>
#include "MRUDPSocket.hpp"
#include "Message.hpp"

using mrudp::MRUDPSocket;
using mrudp::Message;
using mrudp::Connection;
using boost::asio::ip::udp;
using boost::asio::io_service;

static const std::chrono::milliseconds recv_timeout(3000);
static const std::chrono::milliseconds conn_timeout(12000);

Connection::Connection(const udp::endpoint& send_endpoint,
                       const udp::endpoint& recv_endpoint,
                       MRUDPSocket* socket)
    : open_(false),
      is_async_recv_(false),
      current_seq_(1),
      connection_seq_(1),
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
    recv_wait_.notify_all();
    not_accepted_cond_.notify_all();
    async_handle_not_accepted_.wait();
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
        while (!buf_recv_msgs_.empty()) {
            std::unique_lock<std::mutex> lock(buf_recv_msgs_m_);
            handle_message(buf_recv_msgs_.front());
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
        while (!unreaded_messages_.empty()) {
            msg_handler_(std::move(unreaded_messages_.front()->get_data()));
            std::unique_lock<std::recursive_mutex> queue_lock(unreaded_messages_m_);
            unreaded_messages_.pop_front();
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

bool Connection::recv_data(std::string& data, const std::chrono::milliseconds& timeout) {
    if (unreaded_messages_.empty()) {
        std::unique_lock<std::mutex> lock(read_message_m_);
        if (!read_message_.wait_for(lock, timeout,
            [this]() {
                return !unreaded_messages_.empty() && !open_;
            }
        )) {
            return false;
        }
    }
    if (!open_) {
        return false;
    }
    std::unique_lock<std::recursive_mutex> lock(unreaded_messages_m_);
    auto msg = unreaded_messages_.front();
    unreaded_messages_.pop_front();
    data = std::move(msg->get_data());
    return true;
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
    {
        std::unique_lock<std::recursive_mutex> lock(not_accepted_messages_m_);
        not_accepted_messages_.insert(msg);
    }
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
        // if (mrudp::calculate_crc32(msg->get_string_data().data(), msg->get_string_data().size()) != msg->get_crc32()) {
        //     return;
        // }
        
        if (msg->get_packet_type() == Message::PacketType::DEF) {
            std::unique_lock<std::recursive_mutex> lock_con(connection_seq_m_, std::defer_lock);
            std::unique_lock<std::recursive_mutex> lock_list(unreaded_messages_m_, std::defer_lock);
            std::lock(lock_con, lock_list);
            if (msg->get_seq() > connection_seq_) {
                connection_seq_ = msg->get_seq();
                std::unique_lock<std::recursive_mutex> lock(unreaded_messages_m_);
                unreaded_messages_.push_back(msg);
                read_message_.notify_one();
            }
            return;
        }
        if (msg->get_packet_type() == Message::PacketType::REQ) {
            send_serv_msg(Message::Flag::ACK, msg->get_seq());
            std::unique_lock<std::recursive_mutex> lock_con(connection_seq_m_, std::defer_lock);
            std::unique_lock<std::recursive_mutex> lock_list(unreaded_messages_m_, std::defer_lock);
            std::lock(lock_con, lock_list);
            if (msg->get_seq() > connection_seq_) {
                connection_seq_ = msg->get_seq();
            }
            std::unique_lock<std::recursive_mutex> lock(unreaded_messages_m_);
            send_serv_msg(Message::Flag::ACK, msg->get_seq());
            unreaded_messages_.push_back(msg);
            read_message_.notify_one();
            return;
        }
        
        if (msg->get_packet_type() == Message::PacketType::SEQ) {
            // TODO: добавить обработку
        }
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
        if (msg->get_params()) {
        }
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
      send_socket_(io_service_, udp::endpoint(udp::v4(), send_port)),
      recv_socket_(io_service_, udp::endpoint(udp::v4(), recv_port)),
      recv_deadline_(io_service_) {
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
    std::size_t (boost::asio::io_service::*run)() = &boost::asio::io_service::run;
    io_future_ = std::async(std::launch::async, std::bind(run, &io_service_));
}

void MRUDPSocket::close() {
    if (!open_) {
        return;
    }
    open_ = false;
    io_service_.stop();
    io_future_.wait();
    for (auto con : connections_) {
        con->close();
    }
}

void MRUDPSocket::start_receive() {
    recv_socket_.async_receive_from(
        boost::asio::buffer(tmp_data_), tmp_ep_,
        [this] (boost::system::error_code ec, size_t bytes_recvd) {
            if (!ec && bytes_recvd >= sizeof(Message::Header)) {
                handle_receive(bytes_recvd);
            }
            start_receive();
        }
    );
}

void MRUDPSocket::handle_receive(size_t bytes_recvd) {
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
            lock.unlock();
            std::unique_lock<std::mutex> set_lock(not_accepted_connections_m_);
            by_send_not_accepted.erase(iter);
            set_lock.unlock();
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
}

bool MRUDPSocket::listen(shared_ptr<Connection>& con,
                         const std::chrono::milliseconds& timeout) {
    std::unique_lock<std::mutex> lock(listen_mutex_);
    if (!listen_cond_.wait_for(lock, timeout,
        [this] {
            return listen_notify_ == true;
        })
    ) {
        return false;
    }
    listen_notify_ = false;
    lock.unlock();

    std::unique_lock<std::mutex> lock_msg(listen_message_mutex_);
    udp::endpoint send_ep = listen_endpoint_;
    shared_ptr<Message> msg = listen_message_;
    listen_message_.reset();
    lock_msg.unlock();

    udp::endpoint recv_ep = send_ep;
    recv_ep.port(msg->get_params());
    con = boost::make_shared<Connection>(send_ep, recv_ep, this);

    std::unique_lock<std::mutex> lock_con(not_accepted_connections_m_);
    not_accepted_connections_.insert(con);
    lock_con.unlock();

    return true;
}

bool MRUDPSocket::accept(shared_ptr<Connection>& con,
                         const std::chrono::milliseconds& timeout) {
    shared_ptr<Message> send_msg = boost::make_shared<Message>();
    send_msg->set_flags(Message::Flag::INIT | Message::Flag::ACK);
    send_msg->set_params(recv_socket_.local_endpoint().port());
    size_t length = 0;
    bool complete = send_to_impl(send_msg, con->get_recv_ep(), length);
    if (!complete) {
        return false;
    }
    std::unique_lock<std::mutex> lock(accept_mutex_);
    if (!accept_cond_.wait_for(lock, timeout,
        [this, con] {
            if (!accept_con_) {
                return false;
            }
            return accept_con_->get_send_ep() == con->get_send_ep();
        })
    ) {
        return false;
    }
    lock.unlock();

    std::unique_lock<std::mutex> lock_accept(accept_con_mutex_);
    shared_ptr<Connection> save_accept_con = accept_con_; // Сохраняем у себя
    lock_accept.unlock();

    std::unique_lock<std::mutex> lock_con(connections_m_);
    connections_.insert(con);
    con->start();
    return true;
}

bool MRUDPSocket::connect(const udp::endpoint& ep,
                          shared_ptr<Connection>& con,
                          const std::chrono::milliseconds& timeout) {
    shared_ptr<Message> send_msg = boost::make_shared<Message>();
    send_msg->set_flags(Message::Flag::INIT);
    send_msg->set_params(recv_socket_.local_endpoint().port());
    size_t length = 0;
    bool complete = send_to_impl(send_msg, ep, length);
    if (!complete) {
        return false;
    }
    std::unique_lock<std::mutex> lock(connect_mutex_);
    if (!connect_cond_.wait_for(lock, timeout,
        [this] {
            return connecting_notify_ == true;
        })
    ) {
        return false;
    }
    lock.unlock();
    std::unique_lock<std::mutex> lock_con(connect_message_mutex_);
    udp::endpoint send_ep = connect_sender_ep_;
    lock_con.unlock();

    send_msg->set_flags(Message::Flag::ACK);
    send_msg->set_params(0);
    length = 0;
    complete = send_to_impl(send_msg, ep, length);
    if (!complete) {
        return false;
    }

    con = boost::make_shared<Connection>(send_ep, ep, this);
    connections_.insert(con);

    con->start();
    return true;
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
