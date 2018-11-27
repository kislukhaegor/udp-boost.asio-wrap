#include <arpa/inet.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <boost/crc.hpp>
#include <stdexcept>

#include "Message.hpp"

using mrudp::Message;

#define GET_FROM_STRING(get_type, data, read_pos) (*(reinterpret_cast<std::add_pointer<std::add_const<get_type>::type>::type>(reinterpret_cast<const void *>(&((data)[(read_pos)])))))

template<class IntT>
static std::string number_to_string(IntT number) {
    char* data = reinterpret_cast<char*>(&number);
    return std::string(data, sizeof(IntT));
}


Message::Header::Header(uint32_t seq_number, Message::PacketType type)
    : crc_32_sum(0),
      seq_number(seq_number),
      type(type),
      flags(0),
      params(0),
      data_size(0) {
}

Message::Header::Header(const char* data) {
    uint32_t parse_id = ntohl(GET_FROM_STRING(decltype(id), data, 0));
    if (parse_id != id) {
        throw std::invalid_argument("Invalid id");
    }
    size_t read_pos = sizeof(id);
    crc_32_sum = ntohl(GET_FROM_STRING(decltype(crc_32_sum), data, read_pos));
    read_pos += sizeof(crc_32_sum);
    seq_number = ntohl(GET_FROM_STRING(decltype(seq_number), data, read_pos));
    read_pos += sizeof(seq_number);
    type = Message::PacketType(ntohs(GET_FROM_STRING(decltype(type), data, read_pos)));
    read_pos += sizeof(type);
    flags = ntohs(GET_FROM_STRING(decltype(flags), data, read_pos));
    read_pos += sizeof(flags);
    params = ntohl(GET_FROM_STRING(decltype(params), data, read_pos));
    read_pos += sizeof(params);
    data_size = ntohl(GET_FROM_STRING(decltype(data_size), data, read_pos));
    read_pos += sizeof(data_size);
}

std::string Message::Header::str() const {
    std::string data;
    data += number_to_string(htonl(id)) +
            number_to_string(htonl(crc_32_sum)) +
            number_to_string(htonl(seq_number)) +
            number_to_string(htons(type)) +
            number_to_string(htons(flags)) +
            number_to_string(htonl(params)) +
            number_to_string(htonl(data_size));
    return data;
}

Message::Message(const char* net_data, size_t size)
    : data_(net_data + (sizeof(Header) + sizeof(Header::id)), size - (sizeof(Header) + sizeof(Header::id))),
      header_(net_data) {
}

Message::Message(const std::string& data, Message::PacketType type, uint32_t seq_number)
    : data_(data.begin(), data.end()),
      header_(seq_number, type) {
}

void Message::clear() {
    data_.clear();
}

void Message::update_crc() {
    header_.crc_32_sum = calculate_crc32(data_.data(), data_.size());
}

void Message::append_data(const std::string& data) {
    std::copy(data.begin(), data.end(), std::back_inserter(data_));
    header_.data_size = data_.size();
    update_crc();
}

void Message::set_packet_type(PacketType type) {
    header_.type = type;
}

void Message::set_seq(uint32_t seq_number) {
    header_.seq_number = seq_number;
}

void Message::set_params(uint32_t params) {
    header_.params = params;
}

void Message::set_flags(uint32_t flags) {
    header_.flags = flags;
}

void Message::set_time(const std::chrono::time_point<std::chrono::steady_clock>& time) {
    time_ = time;
}

uint32_t Message::get_crc32() const {
    return header_.crc_32_sum;
}

const std::string& Message::get_data() const {
    return data_;
}

uint32_t Message::get_seq() const {
    return header_.seq_number;
}

uint32_t Message::get_data_size() const {
    return header_.data_size;
}

uint8_t Message::get_flags() const {
    return header_.flags;
}

Message::PacketType Message::get_packet_type() const {
    return header_.type;
}

uint32_t Message::get_params() const {
    return header_.params;
}

const std::chrono::time_point<std::chrono::steady_clock>& Message::get_time() const {
    return time_;
}

std::string Message::str() {
    return header_.str() + std::string(data_.begin(), data_.end());
}

bool Message::operator<(const Message& other) {
    return header_.seq_number < other.header_.seq_number;
}

bool Message::operator>(const Message& other) {
    return header_.seq_number > other.header_.seq_number;
}

bool Message::operator>=(const Message& other) {
    return header_.seq_number >= other.header_.seq_number;
}

bool Message::operator<=(const Message& other) {
    return header_.seq_number <= other.header_.seq_number;
}

bool Message::operator!=(const Message& other) {
    return header_.seq_number != other.header_.seq_number;
}

bool Message::operator==(const Message& other) {
    return header_.seq_number == other.header_.seq_number;
}

std::ostream& operator<<(std::ostream& os, const Message::Header& h) {
    os << h.id << " " << h.crc_32_sum << " " << h.seq_number << " " << h.type << " " << h.flags << " " << h.params << " " << h.data_size;
    return os;
}

uint32_t mrudp::calculate_crc32(const char* data, size_t size) {
    boost::crc_32_type result;
    result.process_bytes(data, size);
    return result.checksum();
}