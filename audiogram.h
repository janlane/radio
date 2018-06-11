#ifndef RADIO_AUDIOGRAM_H
#define RADIO_AUDIOGRAM_H

#include <cstdint>
#include <utility>
#include <vector>


class audiogram {
private:
    std::vector<uint8_t> packet;

public:
    static const int HEADER_SIZE = 16;

    void set_size(size_t size) {
        packet = std::vector<uint8_t>(size);
    }

    uint64_t get_session_id() {
        return htonll(*(uint64_t *)packet.data());
    }

    uint64_t set_session_id(uint64_t id) {
        *(uint64_t *)packet.data() = id;
    }

    uint64_t get_packet_id() {
        return ntohll(*(uint64_t *)(packet.data() + sizeof(uint64_t)));
    }

    uint64_t set_packet_id(uint64_t id) {
        *(uint64_t *)(packet.data() + sizeof(uint64_t)) = id;
    }

    uint8_t *get_audio_data() {
        return packet.data() + HEADER_SIZE;
    }

    uint8_t *get_packet_data() {
        return packet.data();
    }

    size_t size() {
        return packet.size();
    }

    bool empty() {
        return packet.empty();
    }

    void clear() {
        packet.clear();
    }

    static inline uint64_t htonll(const uint64_t x) {
        return (1 == htonl(1)) ? x : ((uint64_t)htonl((uint32_t)(x & 0xFFFFFFFF)) << 32u) | htonl((uint32_t)(x >> 32u));
    }

    static inline uint64_t ntohll(const uint64_t x) {
        return (1 == ntohl(1)) ? x : ((uint64_t)ntohl((uint32_t)(x & 0xFFFFFFFF)) << 32u) | ntohl((uint32_t)(x >> 32u));
    }
};

#endif //RADIO_AUDIOGRAM_H
