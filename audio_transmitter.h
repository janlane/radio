#ifndef RADIO_UDP_TRANSMITTER_H
#define RADIO_UDP_TRANSMITTER_H

#include <iostream>
#include <chrono>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include "transmitter.h"
#include "const.h"

class audio_transmitter : public transmitter {
protected:
    static const in_addr_t MIN_MCAST_ADDR_VAL = 0xE0000001; // 224.0.0.1
    static const in_addr_t MAX_MCAST_ADDR_VAL = 0xEFFFFFFF; // 239.255.255.255
    static const size_t MAX_NAME_LEN = 64;

    struct sockaddr_in mcast_addr = {0};
    std::string mcast_addr_dotted = "";
    in_port_t data_port = (in_port_t)25826;
    in_port_t ctrl_port = (in_port_t)35826;
    size_t psize = 512;
    size_t fsize = 128 * 1000;
    std::chrono::milliseconds rtime = std::chrono::milliseconds(250);
    std::string name = "Nienazwany Nadajnik";
    transmitter audio_tr;
    transmitter replies_tr;

    virtual int init(int argc, char *argv[]) {
        namespace po = boost::program_options;
        int time = 250;

        po::options_description desc("Options");
        desc.add_options()
                (",a", po::value<std::string>(&mcast_addr_dotted)->required(),
                 "mcast_addr")
                (",P", po::value<in_port_t>(), "data_port")
                (",C", po::value<in_port_t>(), "ctrl_port")
                (",p", po::value<size_t>(&psize), "psize")
                (",f", po::value<size_t>(&fsize), "fsize")
                (",r", po::value<int>(&time), "rtime")
                (",n", po::value<std::string>(&name), "name");

        po::variables_map vm;
        try {
            po::store(po::parse_command_line(argc, argv, desc), vm);
            po::notify(vm);
        } catch (po::error &e) {
            std::cerr << e.what() << "\n";
            return 1;
        }

        if (!inet_pton(AF_INET, mcast_addr_dotted.c_str(), &mcast_addr.sin_addr)/* ||
            mcast_addr.sin_addr.s_addr < htonl(MIN_MCAST_ADDR_VAL) ||
            mcast_addr.sin_addr.s_addr > htonl(MAX_MCAST_ADDR_VAL) */) {
            std::cerr << "the argument ('" << inet_ntoa(mcast_addr.sin_addr) <<
                      "') for option '-a' is invalid\n";
            return 1;
        }
        if (data_port == 0) {
            std::cerr << "the argument ('0') for option '--P' is invalid\n";
            return 1;
        }
        if (ctrl_port == 0) {
            std::cerr << "the argument ('0') for option '--C' is invalid\n";
            return 1;
        }
        if (psize == 0) {
            std::cerr << "the argument ('0') for option '--p' is invalid\n";
            return 1;
        }
        if (fsize == 0) {
            std::cerr << "the argument ('0') for option '--f' is invalid\n";
            return 1;
        }
        if (time <= 0) {
            std::cerr << "the argument ('" << time << "') for option '--r' is invalid\n";
            return 1;
        } else {
            rtime = std::chrono::milliseconds(time);
        }
        if (name.size() > MAX_NAME_LEN) {
            std::cerr << "the argument ('" << name << "') for option '--n' is invalid\n";
            return 1;
        }

        data_port = htons(data_port);
        ctrl_port = htons(ctrl_port);

        return prepare_to_send();
    }

    int send_audiogram(audiogram &a) {
//        for (int i = 0; i < psize + 16; ++i) {
//            printf("%x", a[i] & 0xff);
//        } printf("\n");
        if (sendto(audio_tr.sock, (void *)a.get_packet_data(), psize, 0,
                   (struct sockaddr *)&mcast_addr, sizeof(mcast_addr)) == -1) {
            std::cerr << "Error: audiogram sendto, errno = " << errno << "\n";
            return 1;
        }

        return 0;
    }

    void send_reply(sockaddr_in &addr) {
        // BOREWICZ_HERE [MCAST_ADDR] [DATA_PORT] [nazwa stacji]
        char msg[MAX_CTRL_MSG_LEN];
        int msg_size = sprintf(msg, "%s %s %d %s\n", REPLY_MSG,
                mcast_addr_dotted.data(), data_port, name.data());
        std::cerr << "Reply " << msg << " to " << inet_ntoa(addr.sin_addr) << "\n";
        if (msg_size < 0)
            return;

        if (sendto(replies_tr.sock, (void*)&msg, (size_t)msg_size, 0,
                   (struct sockaddr *)&addr, sizeof(addr)) == -1) {
            std::cerr << "Error: reply sendto, errno = " << errno << "\n";
        }
    }

private:
    int prepare_to_send() override {
        audio_tr.prepare_to_send();
        replies_tr.prepare_to_send();

        mcast_addr.sin_family = AF_INET;
        mcast_addr.sin_port = data_port;

        return transmitter::prepare_to_send();
    }
};


#endif //RADIO_UDP_TRANSMITTER_H
