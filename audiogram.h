#ifndef RADIO_AUDIOGRAM_H
#define RADIO_AUDIOGRAM_H

#include <cstdint>
#include <utility>
#include <vector>


class audiogram {
public:
    static const int HEADER_SIZE = 16;

    uint64_t session_id;
    uint64_t first_byte_num;
    std::vector<uint8_t> audio_data;

    audiogram() = default;

    audiogram(uint64_t session_id, uint64_t first_byte_num) :
            session_id(reverse_bytes(session_id)),
            first_byte_num(reverse_bytes(first_byte_num)) {};

    audiogram(uint64_t session_id, uint64_t first_byte_num,
              std::vector<uint8_t > &v) :
            session_id(reverse_bytes(session_id)),
            first_byte_num(reverse_bytes(first_byte_num)),
            audio_data(v) {};

    size_t size() {
        return audio_data.size() + sizeof(session_id) + sizeof(first_byte_num);
    }

    uint64_t reverse_bytes(const uint64_t in) {
        unsigned char out[8] = {in>>56, in>>48, in>>40, in>>32, in>>24, in>>16, in>>8, in};
        return *(uint64_t *)out;
    }
};

#endif //RADIO_AUDIOGRAM_H
