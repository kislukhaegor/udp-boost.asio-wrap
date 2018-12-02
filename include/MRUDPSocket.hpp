#pragma once

#include <array>
#include <list>
#include <set>
#include <cstdint>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <condition_variable>
#include <boost/asio.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/fiber/all.hpp>

#include "Message.hpp"

using std::chrono::time_point;
using std::chrono::milliseconds;
using std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::duration;
using boost::asio::ip::udp;
using boost::shared_ptr;
using boost::multi_index::tag;
using boost::multi_index::const_mem_fun;
using boost::multi_index::member;
using boost::multi_index::multi_index_container;
using boost::multi_index::indexed_by;
using boost::multi_index::ordered_unique;
using boost::multi_index::ordered_non_unique;
using boost::shared_ptr;

namespace mrudp {

    // class MRUDPSocket;

    class MRUDPSocket;

    class Connection : public boost::enable_shared_from_this<Connection> {
      public:
        Connection(const udp::endpoint&, const udp::endpoint&, MRUDPSocket*);
        ~Connection();
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        void send_def(const std::string& data);
        void send_req(const std::string& data);
        void send_seq(const std::string& data);

        bool start_async_recv(const std::function<void(std::string data)>& msg_handler);
        void stop_async_recv();
        bool is_async_recv() const;

        template<class Rep, class Period>
        bool recv_data(std::string& data, const duration<Rep, Period>& timeout);

        bool is_open() const;
        void close();

        const udp::endpoint& get_send_ep() const;
        const udp::endpoint& get_recv_ep() const;
        const time_point<steady_clock>& get_last_send_time() const;
        const time_point<steady_clock>& get_last_recv_time() const;

        struct BySend {};  // multi_index tag
        struct ByRecv {};  // multi_index tag
        struct BySendTime {};  // multi_index tag
        struct ByRecvTime {};  // multi_index tag
        struct ByPtr {};  // multi_index tag

        typedef shared_ptr<Message> message_ptr;

        typedef multi_index_container<message_ptr,
            indexed_by<
                ordered_unique<
                    tag<Message::BySeq>, const_mem_fun<Message, uint32_t, &Message::get_seq>
                >,
                ordered_non_unique<
                    tag<Message::ByTime>,
                    const_mem_fun<Message,
                    const time_point<steady_clock>&,
                    &Message::get_time>
                >,
                ordered_unique<
                    tag<Message::ByPtr>, const_mem_fun<message_ptr, Message*, &message_ptr::get>
                >
            >
        > message_set_type;

      private:
        void start();
        // void wait_recv();

        void send_msg(const message_ptr& msg);
        void send_serv_msg(Message::Flag flags, uint32_t params);

        milliseconds handle_not_accepted_msg(const time_point<steady_clock>&);
        void handle_message(const message_ptr&);
        void handle_serv(const message_ptr&);
        void handle_seq(const message_ptr&);

        bool accept_msg(uint32_t seq);

        bool open_;

        std::recursive_mutex async_recv_m_;
        bool is_async_recv_;
        std::function<void(std::string data)> msg_handler_;

        std::recursive_mutex current_seq_m_;
        time_point<steady_clock> last_send_time_;
        uint32_t current_seq_;

        std::recursive_mutex connection_seq_m_;
        time_point<steady_clock> last_recv_time_;
        uint32_t connection_seq_;

        std::recursive_mutex unreaded_messages_m_;
        std::list<message_ptr> unreaded_messages_;

        std::mutex read_message_m_;
        std::condition_variable read_message_;
        
        std::recursive_mutex defered_messages_m_;
        message_set_type defered_messages_;
        uint32_t last_seq_recv_;
        uint32_t last_seq_send_;

        std::recursive_mutex not_accepted_messages_m_;
        message_set_type not_accepted_messages_;

        udp::endpoint send_endpoint_;
        udp::endpoint recv_endpoint_;

        MRUDPSocket* socket_;

        std::set<uint32_t> accepted_nums;

        friend class MRUDPSocket;
    };

    class MRUDPSocket {
      public:
        typedef shared_ptr<Connection> con_ptr;

        typedef multi_index_container<con_ptr,
            indexed_by<
                ordered_unique<
                    tag<Connection::BySend>, const_mem_fun<Connection, const udp::endpoint&, &Connection::get_send_ep>
                >,
                ordered_unique<
                    tag<Connection::ByRecv>, const_mem_fun<Connection, const udp::endpoint&, &Connection::get_recv_ep>
                >,
                ordered_unique<
                    tag<Connection::BySendTime>,
                    const_mem_fun<Connection,
                                  const time_point<steady_clock>&,
                                  &Connection::get_last_send_time
                                 >
                >,
                 ordered_unique<
                    tag<Connection::ByRecvTime>,
                    const_mem_fun<Connection,
                                  const time_point<steady_clock>&,
                                  &Connection::get_last_recv_time
                                 >
                >,
                ordered_unique<
                    tag<Connection::ByPtr>, const_mem_fun<con_ptr, Connection*, &con_ptr::get>
                >
            >
        > connections_set_type;

        MRUDPSocket(unsigned send_port = 0, unsigned recv_port = 0);

        ~MRUDPSocket();

        void open(uint32_t handle_threads_count = 1);

        bool is_open() const;

        void close();

        MRUDPSocket(const MRUDPSocket&) = delete;

        MRUDPSocket& operator=(const MRUDPSocket&) = delete;

        const connections_set_type& get_connections() const;

        size_t get_connections_count() const;

        bool send_to_impl(const shared_ptr<Message>& msg, const udp::endpoint& ep, size_t& bytes_transferred);
     
        template<class Rep, class Period>
        bool connect(const udp::endpoint& ep, shared_ptr<Connection>& con,
                     const std::chrono::duration<Rep, Period>& timeout);
        
        template<class Rep, class Period>
        bool listen(shared_ptr<Connection>& con, const std::chrono::duration<Rep, Period>& timeout);  //blocking

        template<class Rep, class Period>
        bool accept(shared_ptr<Connection>& con, const std::chrono::duration<Rep, Period>& timeout);

        template<class Rep, class Period>
        bool listen_and_accept(const std::chrono::duration<Rep, Period>& timeout);

        template<class Rep, class Period>
        size_t async_listen_and_accept(size_t count, const std::chrono::duration<Rep, Period>&);

        void set_disconnect_handler(std::function<void(shared_ptr<Connection>)>);

        void set_accept_handler(std::function<void(shared_ptr<Connection>)>);

        const std::function<void(shared_ptr<Connection>)>& get_disconnect_handler() const;
       
        const std::function<void(shared_ptr<Connection>)>& get_accept_handler() const;
      
        boost::asio::ip::address get_local_ip();

        boost::asio::io_context& get_io_context();

        static const uint32_t service_thread_count = 2;

      private:
        void handle_not_accepted_msg();

        void run(uint32_t);

        void start_receive();

        void handle_receive(size_t bytes_rcvd);

        void handle_cons();

        bool open_;

        /* listen condition_variable logic  */
        std::future<void> io_future_;

        std::mutex listen_message_mutex_;
        shared_ptr<Message> listen_message_;
        udp::endpoint listen_endpoint_;
        
        std::mutex listen_mutex_;
        bool listen_notify_;
        std::condition_variable listen_cond_;
        /*----------------------------------*/

        /* accept condition_variable logic  */        
        std::mutex accept_con_mutex_;
        shared_ptr<Connection> accept_con_;
        
        std::mutex accept_mutex_;
        std::condition_variable accept_cond_;
        /*----------------------------------*/

        /* connect condition_variable logic */
        std::mutex connect_message_mutex_;
        udp::endpoint connect_sender_ep_;
        
        std::mutex connect_mutex_;
        bool connecting_notify_;
        std::condition_variable connect_cond_;
        /*----------------------------------*/

        boost::asio::io_context io_context_;

        udp::socket send_socket_;
        udp::socket recv_socket_;

        udp::endpoint tmp_ep_;
        std::array<char, 2048> tmp_data_;

        std::mutex connections_m_;
        connections_set_type connections_;

        std::mutex not_accepted_connections_m_;
        connections_set_type not_accepted_connections_;

        std::function<void(shared_ptr<Connection>)> disconnect_handler;
        std::function<void(shared_ptr<Connection>)> accept_handler;
    };


    template<class Rep, class Period>
    bool MRUDPSocket::listen_and_accept(const std::chrono::duration<Rep, Period>& timeout) {
        shared_ptr<Connection> con;
        bool complete = listen(con, timeout);
        if (!complete) {
            return false;
        }
        complete = accept(con, timeout);
        return complete;
    }

    template<class Rep, class Period>
    size_t MRUDPSocket::async_listen_and_accept(size_t count, const std::chrono::duration<Rep, Period>& timeout) {
        std::vector<std::future<bool>> futures;
        for (size_t i = 0; i < count; ++i) {
            futures.push_back(std::async(std::launch::async, &MRUDPSocket::listen_and_accept<Rep, Period>, this, std::cref(timeout)));
        }
        for (auto& fut : futures) {
            fut.wait();
        }
        size_t success_count = 0;
        for (size_t i = 0; i < count; ++i) {
            bool complete = futures[i].get();
            if (complete) {
                ++success_count;
            }
        }
        return success_count;
    }

    template<class Rep, class Period>
    bool MRUDPSocket::listen(shared_ptr<Connection>& con,
                             const std::chrono::duration<Rep, Period>& timeout) {
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

    template<class Rep, class Period>
    bool MRUDPSocket::accept(shared_ptr<Connection>& con,
                             const std::chrono::duration<Rep, Period>& timeout) {
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
        std::unique_lock<std::mutex> lock_con(connections_m_);
        connections_.insert(con);
        con->start();
        if (accept_handler) {
            io_context_.post(std::bind(accept_handler, con));
        }
        return true;
    }

    template<class Rep, class Period>
    bool MRUDPSocket::connect(const udp::endpoint& ep,
                              shared_ptr<Connection>& con,
                              const std::chrono::duration<Rep, Period>& timeout) {
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

    template<class Rep, class Period>
    bool Connection::recv_data(std::string& data, const std::chrono::duration<Rep, Period>& timeout) {
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
};
