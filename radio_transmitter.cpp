#include <iostream>
#include <string>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <limits>
#include <queue>
#include <set>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "boost/circular_buffer.hpp"
#include "boost/program_options.hpp"
#include "audiogram.h"
#include "audio_transmitter.h"
#include "receiver.h"
#include "const.h"


class radio_transmitter : protected audio_transmitter {
private:
    boost::circular_buffer<audiogram> data_q;
    std::unique_ptr<std::set<uint64_t>> retransmit_nums_ptr;
    std::queue<sockaddr_in> replies_q;
    std::mutex retransmit_nums_mut;
    std::mutex replies_mut;
    std::atomic_flag keep_listening = ATOMIC_FLAG_INIT;
    std::atomic_flag stop_replying = ATOMIC_FLAG_INIT;
    int rcv_sock = -1;

public:
    ~radio_transmitter() {
        close(rcv_sock);
    }

    int init(int argc, char *argv[]) override {
        data_q = boost::circular_buffer<audiogram>(fsize / psize);
        retransmit_nums_ptr = std::make_unique<std::set<uint64_t>>();
        audio_transmitter::init(argc, argv);
//        std::ios_base::sync_with_stdio(false);
//        std::cin.tie(nullptr);
//        std::cerr.tie(nullptr);
    }

    void work() {
        keep_listening.test_and_set();
        std::thread t1(&radio_transmitter::listen_for_incoming, this);
        stop_replying.test_and_set();
        std::thread t2(&radio_transmitter::send_replies, this);

        transmit_and_retransmit();
        keep_listening.clear();
        stop_replying.clear();

        t1.join();
        t2.join();
    }

private:
    void prepare_to_receive() {
        int err;
        sockaddr_in server_address;

        do {
            err = 0;
            close(rcv_sock);
            rcv_sock = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
            if (rcv_sock < 0) {
                std::cerr << "Error: ctrl_rcv socket, errno = " << errno << "\n";
                err = 1;
            }

            int optval = 1;
            if (setsockopt(rcv_sock, SOL_SOCKET, SO_BROADCAST, (void *) &optval, sizeof optval) < 0) {
                std::cerr << "Error: setsockopt broadcast\n";
                err = 1;
            }
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 300000;
            if (setsockopt(rcv_sock, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
                std::cerr << "Error: setsockopt rcvtimeo\n";
                err = 1;
            }

            server_address.sin_family = AF_INET; // IPv4
            server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
            server_address.sin_port = ctrl_port; // default port for receiving is PORT_NUM

            // bind the socket to a concrete address
            if (bind(rcv_sock, (struct sockaddr *) &server_address,
                     (socklen_t) sizeof(server_address)) < 0) {
                std::cerr << "Error: ctrl_rcv bind, errno = " << errno << "\n";
                err = 1;
            }
        } while (err);
    }

    void transmit_and_retransmit() {
        namespace ch = std::chrono;
        uint64_t packet_id = 0, session_id = (uint64_t)time(nullptr);

        std::cerr << "session " << session_id << " sent\n";
        while (!std::cin.eof()) {
            /* transmit */
            auto start = std::chrono::system_clock::now();
            do {
                audiogram a(psize, 1);
                a.set_size(psize);
                a.set_session_id(audiogram::htonll(session_id));
                a.set_packet_id(audiogram::htonll(packet_id));
                std::cin.read((char *)a.get_audio_data(), psize - audiogram::HEADER_SIZE);
                if (std::cin.fail())
                    return;

                // TODO temp
                //if (a.get_packet_id() % 1000 != 0 && a.get_packet_id() % 1000 != 1 && a.get_packet_id() % 1000 != 2)
                send_audiogram(a);

                data_q.push_back(std::move(a));
                packet_id += psize;
            } while (ch::system_clock::now() - start < rtime && !std::cin.eof());

            /* retransmit */
            retransmit_nums_mut.lock();
            std::unique_ptr<std::set<uint64_t>> nums_ptr =
                    std::move(retransmit_nums_ptr);
            retransmit_nums_ptr = std::make_unique<std::set<uint64_t>>();
            retransmit_nums_mut.unlock();

            int q = 0;
            for (uint64_t num : *nums_ptr) {
                while (q < data_q.size() && num < data_q[q].get_packet_id())
                    ++q;
                if (q == data_q.size())
                    break;

                if (num == data_q[q].get_packet_id()) {
                    send_audiogram(data_q[q]);
                }
                ++q;
            }
        }
    }

    void listen_for_incoming() {
        prepare_to_receive();
        char buffer[MAX_UDP_MSG_LEN];

        while (keep_listening.test_and_set()) {
            struct sockaddr_in rcv_addr;
            socklen_t rcv_addr_len = (socklen_t)sizeof(rcv_addr);
            ssize_t rcv_len = recvfrom(rcv_sock, (void *)&buffer, sizeof(buffer),
                    0, (struct sockaddr *)&rcv_addr, &rcv_addr_len);

            if (rcv_len < 0) {
                std::cerr << "Error: rcv_ctrl rcvfrom\n";
            } else {
                //printf("read %zd bytes: %.*s\n", rcv_len, (int) rcv_len, buffer);
                buffer[rcv_len] = '\0';

                if (buffer[0] == LOOKUP_MSG[0]) {
                    if (!parse_lookup(buffer, (size_t)rcv_len)) {
                        replies_mut.lock();
                        replies_q.push(rcv_addr); // TODO zdecydować czy od razu send?
                        std::cerr << "reply pushed with " << inet_ntoa(rcv_addr.sin_addr) << "\n";
                        replies_mut.unlock();
                    }
                } else if (buffer[0] == REXMIT_MSG[0]) {
                    std::vector<uint64_t> results;
                    if (!parse_rexmit(buffer, (size_t)rcv_len, results)) {
                        retransmit_nums_mut.lock();
                        for(uint64_t res : results)
                            retransmit_nums_ptr->insert(res);
                        retransmit_nums_mut.unlock();
                    }
                }
            }
        }
    }

    void send_replies() {
        while (stop_replying.test_and_set()) {
            int not_empty = 0;
            sockaddr_in addr;
            replies_mut.lock();
            if (!replies_q.empty()) {
                addr = replies_q.back();
                replies_q.pop();
                not_empty = 1;
            }
            replies_mut.unlock();

            if (not_empty) {
                send_reply(addr);
            }
        }
    }

    int parse_lookup(const char *msg, size_t len) {
        if (strstr(msg, LOOKUP_MSG) != msg)
            return 1;
        return len != strlen(LOOKUP_MSG);
    }

    int parse_rexmit(char *msg, const size_t len,  std::vector<uint64_t> &results) {
        if (strstr(msg, REXMIT_MSG) != msg || msg[len] == ',')
            return 1;

        uint64_t res = 0;
        int err = 0;
        char *token = strtok(msg, ",");

        while (token != nullptr) {
            std::string tok = std::string(token);

            std::cerr << "token " << token << "\n"; // TODO del
            try {
                res = audiogram::ntohll(std::stoull(tok));
            } catch (const std::exception &e) {
                err = 1;
            }

            if (!err)
                results.push_back(res);
            token = strtok(nullptr, ",");
        }

        return 0;
    }

};

int main(int argc, char *argv[]) {
    radio_transmitter t;
    if (t.init(argc, argv)) return 1;
    t.work();

    return 0;
}