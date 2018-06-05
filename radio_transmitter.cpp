#include <iostream>
#include <string>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <chrono>
#include <thread>
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
    /* first chars of LOOKUP_MSG and REXMIT_MSG shall remain different */
    static const char *LOOKUP_MSG = "ZERO_SEVEN_COME_IN";
    static const char *REXMIT_MSG = "LOUDER_PLEASE ";
    static const char *REPLY_MSG = "BOREWICZ_HERE ";

    boost::circular_buffer<audiogram> data_q;
    std::unique_ptr<std::set<uint64_t>> retransmit_nums_ptr;
    std::queue<sockaddr_in> replies_q;
    std::mutex retransmit_nums_mut;
    std::mutex bcast_mut;
    std::mutex replies_mut;
    int rcv_sock = -1;

public:
    int init(int argc, char *argv[]) override {
        data_q = boost::circular_buffer<audiogram>(fsize / psize);
        retransmit_nums_ptr = std::make_unique<std::set<uint64_t>>();
        audio_transmitter::init(argc, argv);
    }

    void work() {
        std::thread t1(listen_for_incoming());
        std::thread t2(transmit_and_retransmit());
        std::thread t3(send_replies());
        t1.join();
        t2.join();
        t3.join();
    }

private:
    ~radio_transmitter() {
        close(sock);
        close(rcv_sock);
    }

    void prepare_to_receive() {
        sockaddr_in server_address;
        int err = 0;
        rcv_sock = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
        if (rcv_sock < 0) {
            std::cerr << "Error: ctrl_rcv socket, errno = " << errno << "\n";
            err = 1;
        }
        // after socket() call; we should close(sock) on any execution path;
        // since all execution paths exit immediately, sock would be closed when program terminates

        server_address.sin_family = AF_INET; // IPv4
        server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
        server_address.sin_port = htons(ctrl_port); // default port for receiving is PORT_NUM

        // bind the socket to a concrete address
        if (bind(rcv_sock, (struct sockaddr *) &server_address,
                 (socklen_t) sizeof(server_address)) < 0) {
            std::cerr << "Error: ctrl_rcv bind, errno = " << errno << "\n";
            err = 1;
        }
    }

    void listen_for_incoming() {
        prepare_to_receive();
        char buffer[MAX_UDP_MSG_LEN];

        for (;;) {
            struct sockaddr_in rcv_addr;
            socklen_t rcv_addr_len;
            ssize_t rcv_len = recvfrom(sock, (void *) &buffer, sizeof buffer, 0,
                    (struct sockaddr *) &rcv_addr, &rcv_addr_len);

            if (rcv_len < 0) {
                std::cerr << "Error: rcv_ctrl rcvfrom\n";
            } else {
                printf("read %zd bytes: %.*s\n", rcv_len, (int) rcv_len, buffer);
                buffer[rcv_len] = '\0';

                if (buffer[0] == LOOKUP_MSG[0]) {
                    if (!parse_lookup(buffer, (size_t)(rcv_len))) {
                        replies_mut.lock();
                        replies_q.push(rcv_addr);
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

    void transmit_and_retransmit() {
        namespace ch = std::chrono;
        uint64_t packet_id = 0, session_id = (uint64_t)time(nullptr);
        std::vector<char> buf(psize);
        char test[psize];

        while (!std::cin.eof) {
            /* transmit */
            auto start = std::chrono::system_clock::now();
            do {
                std::cin.read(buf.data(), psize);
                if (std::cin.failbit)
                    return;

                audiogram a = audiogram(session_id, packet_id, buf);
                data_q.push_back(a);

                if (send_audiogram(a)) {
                    break; // TODO nie break a handler albo nic
                }

                packet_id += psize;
            } while (ch::duration<ch::milliseconds>(
                    ch::system_clock::now() - start) < rtime && !std::cin.eof());

            /* retransmit */
            retransmit_nums_mut.lock();
            std::unique_ptr<std::set<uint64_t>> nums_ptr =
                    std::move(retransmit_nums_ptr);
            retransmit_nums_ptr = std::make_unique<std::set<uint64_t>>();
            retransmit_nums_mut.unlock();

            int q = 0;
            for (uint64_t num : *nums_ptr) {
                while (q < data_q.size() && num < data_q[q].first_byte_num)
                    ++q;
                if (q == data_q.size())
                    break;

                if (num == data_q[q].first_byte_num) {
                    send_audiogram(data_q[q]);
                }
                ++q;
            }
        }
    }

    void send_replies() {
        for(;;) {
            replies_mut.lock();
            sockaddr_in addr = replies_q.back();
            replies_q.pop();
            replies_mut.unlock();

            bcast_mut.lock();
            send_reply(addr);
            bcast_mut.unlock();
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

            printf ("%s\n",token); // TODO del
            try {
                res = std::stoull(tok);
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