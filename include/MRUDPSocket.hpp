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

#include "Message.hpp"

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

        bool recv_data(std::string& data, const std::chrono::milliseconds& timeout);

        bool is_open() const;
        void close();

        const udp::endpoint& get_send_ep() const;
        const udp::endpoint& get_recv_ep() const;

        struct BySend {};  // multi_index tag
        struct ByRecv {};  // multi_index tag
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
                    const std::chrono::time_point<std::chrono::steady_clock>&,
                    &Message::get_time>
                >,
                ordered_unique<
                    tag<Message::ByPtr>, const_mem_fun<message_ptr, Message*, &message_ptr::get>
                >
            >
        > message_set_type;

      private:
        void start();
        void handle_con();
        void wait_recv();

        void send_msg(const message_ptr& msg);
        void send_serv_msg(Message::Flag flags, uint32_t params);

        void async_recv_data();

        void handle_not_accepted_msg();
        void handle_message(const message_ptr&);
        void handle_serv(const message_ptr&);
        void handle_seq(const message_ptr&);

        bool accept_msg(uint32_t seq);

        bool open_;

        std::recursive_mutex async_recv_m_;
        bool is_async_recv_;
        std::function<void(std::string data)> msg_handler_;

        std::recursive_mutex current_seq_m_;
        std::chrono::time_point<std::chrono::steady_clock> last_send_time_;
        uint32_t current_seq_;

        std::recursive_mutex connection_seq_m_;
        std::chrono::time_point<std::chrono::steady_clock> last_recv_time_;
        uint32_t connection_seq_;

        std::recursive_mutex unreaded_messages_m_;
        std::list<message_ptr> unreaded_messages_;

        std::mutex read_message_m_;
        std::condition_variable read_message_;
        
        std::recursive_mutex defered_messages_m_;
        message_set_type defered_messages_;
        uint32_t last_seq_recv_;
        uint32_t last_seq_send_;

        std::mutex not_accepted_cond_m_;
        std::condition_variable not_accepted_cond_;

        std::recursive_mutex not_accepted_messages_m_;
        message_set_type not_accepted_messages_;

        udp::endpoint send_endpoint_;
        udp::endpoint recv_endpoint_;

        MRUDPSocket* socket_;
        std::future<void> async_handle_conn_;
        std::future<void> async_wait_recv_;
        std::future<void> async_handle_not_accepted_;
        std::future<void> async_recv_;

        std::mutex recv_wait_m_;
        std::condition_variable recv_wait_;

        std::mutex buf_recv_msgs_m_;
        std::list<message_ptr> buf_recv_msgs_;

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
                    tag<Connection::ByPtr>, const_mem_fun<con_ptr, Connection*, &con_ptr::get>
                >
            >
        > connections_set_type;

        MRUDPSocket(unsigned send_port, unsigned recv_port);

        ~MRUDPSocket();

        void open();

        bool is_open() const;

        void close();

        MRUDPSocket(const MRUDPSocket&) = delete;

        MRUDPSocket& operator=(const MRUDPSocket&) = delete;

        bool connect(const udp::endpoint& ep, shared_ptr<Connection>& con,
                     const std::chrono::milliseconds& timeout);
        bool listen(shared_ptr<Connection>& con, const std::chrono::milliseconds& timeout);  //blocking

        bool accept(shared_ptr<Connection>& con, const std::chrono::milliseconds& timeout);
        
        const connections_set_type& get_connections() const;

        size_t get_connections_count() const;

        bool send_to_impl(const shared_ptr<Message>& msg, const udp::endpoint& ep, size_t& bytes_transferred);
     
        bool listen_and_accept(const std::chrono::milliseconds& timeout);

        size_t async_listen_and_accept(size_t count, const std::chrono::milliseconds&);

        void set_disconnect_handler(std::function<void(shared_ptr<Connection>)>);

        void set_accept_handler(std::function<void(shared_ptr<Connection>)>);

        const std::function<void(shared_ptr<Connection>)>& get_disconnect_handler() const;
       
        const std::function<void(shared_ptr<Connection>)>& get_accept_handler() const;
      
      private:

        void start_receive();

        void handle_receive(size_t bytes_rcvd);

        bool open_;

        /* listen condition_variable logic  */
        std::future<size_t> io_future_;

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
};
