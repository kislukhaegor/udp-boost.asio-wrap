#pragma once

#include <chrono>
#include <cstdint>
#include <boost/asio.hpp>
#include <boost/shared_ptr.hpp>

namespace mrudp {
    using boost::asio::ip::udp;

    static const uint32_t UUID = 3537606816;

    class Message {
      public:
        enum PacketType : uint16_t {
            DEF,
            REQ,
            SEQ
        };

        enum Flag : uint16_t{
            INIT = 1,
            ACK = 2,
            FIN = 4
        };

        struct Header {
            Header(const char*);
            Header(uint32_t seq_number = 0, PacketType type = PacketType::DEF);

            std::string str() const;

            static const uint32_t id = UUID;
            uint32_t crc_32_sum;
            uint32_t seq_number;
            PacketType type;
            uint16_t flags;
            uint32_t params;
            uint32_t data_size;
        };

        struct BySeq {};  // multi_index tag
        struct ByTime {};  // multi_index tag
        struct ByPtr {};  // multi_index tag

        struct TimeUpdate {
            TimeUpdate(const std::chrono::time_point<std::chrono::steady_clock>& time)
                : time(time) {
            }
            void operator()(Message& m) {
                m.set_time(time);
            }
            void operator()(boost::shared_ptr<Message>& m) {
                m->set_time(time);
            }

            std::chrono::time_point<std::chrono::steady_clock> time;
        };  // functor for updating time

        Message() = default;
        Message(Header&);
        Message(const char* net_data, size_t size);
        Message(const std::string& data, PacketType type = Message::PacketType::DEF, uint32_t seq_number = 0);

        Message& operator=(const Message&) = default;


        bool operator<(const Message& other);
        bool operator>(const Message& other);
        bool operator>=(const Message& other);
        bool operator<=(const Message& other);
        bool operator!=(const Message& other);
        bool operator==(const Message& other);

        const Header& get_header() { return header_; }
        void clear();
        void update_crc();
        void append_data(const std::string& data);
        void set_packet_type(PacketType type);
        void set_seq(uint32_t seq);
        void set_params(uint32_t params);
        void set_flags(uint32_t flags);
        void set_time(const std::chrono::time_point<std::chrono::steady_clock>& time);
        const std::chrono::time_point<std::chrono::steady_clock>& get_time() const;
        uint32_t get_crc32() const;
        const std::string& get_data() const;
        // const std::vector<char>& get_data() const;
        uint32_t get_data_size() const;
        uint32_t get_seq() const;
        uint8_t get_flags() const;
        PacketType get_packet_type() const;
        uint32_t get_params() const;
        /* return string which may be send by net */
        std::string str();

      private:
        std::chrono::time_point<std::chrono::steady_clock> time_;  // send or recv
        // udp::endpoint endpoint_;  // sender or reciver;
        std::string data_;
        Header header_;
    };

    uint32_t calculate_crc32(const char* data, size_t size);
};

std::ostream& operator<<(std::ostream& os, const mrudp::Message::Header& h);