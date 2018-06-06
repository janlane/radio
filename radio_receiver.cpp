#include <iostream>
#include <string>
#include <cstdint>
#include <cerrno>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "boost/circular_buffer.hpp"
#include "boost/program_options.hpp"
#include "audiogram.h"
#include "receiver.h"


class radio_receiver : private receiver {
private:
    struct sockaddr_in discover_addr = {0, 0, (uint32_t)-1, 0};
    struct sockaddr_in mcast_addr;
    in_port_t ctrl_port = (in_port_t)35826;
    in_port_t ui_port = (in_port_t)15826;
    size_t bsize = 512;
    unsigned int rtime = 250;

    boost::circular_buffer<audiogram> buffer;

public:
    int init(int argc, char *argv[]) {
        namespace po = boost::program_options;
        std::string addr;
        discover_addr.sin_addr.s_addr = htonl(discover_addr.sin_addr.s_addr);

        po::options_description desc("Options");
        desc.add_options()
                (",d", po::value<std::string>(&addr), "discover_addr")
                (",C", po::value<in_port_t>(&ctrl_port), "ctrl_port")
                (",U", po::value<in_port_t>(&ui_port), "ui_port")
                (",b", po::value<size_t>(&bsize), "bsize")
                (",r", po::value<unsigned int>(&rtime), "rtime");

        po::variables_map vm;
        try {
            po::store(po::parse_command_line(argc, argv, desc), vm);
            po::notify(vm);
        } catch (po::error &e) {
            std::cerr << e.what() << "\n";
            return 1;
        }

        ctrl_port = htons(ctrl_port);
        ui_port = htons(ui_port);

        if (!addr.empty() && !inet_pton(AF_INET, addr.c_str(), &discover_addr)) {
            std::cerr << "the argument ('" << addr <<
                      "') for option '-a' is invalid\n";
            return 1;
        }
        if (ctrl_port == 0) {
            std::cerr << "the argument ('0') for option '--C' is invalid\n";
            return 1;
        }
        if (ui_port == 0) {
            std::cerr << "the argument ('0') for option '--U' is invalid\n";
            return 1;
        }
        if (bsize == 0) {
            std::cerr << "the argument ('0') for option '--b' is invalid\n";
            return 1;
        }

        buffer = boost::circular_buffer<audiogram>(bsize / 512);
        // temp
        prepare_to_receive_from((char *)"224.0.0.1", 25826);

        return 0;
    }

    int listen() {
        std::ios_base::sync_with_stdio(false);
        for(int i = 0; i < 8000000; ++i) {
            uint8_t buffer[65536];
            // zmienić na rcvfrom?
            ssize_t rcv_len = read(sock, (void *)buffer, sizeof(buffer));
            if (rcv_len < 0) {
                std::cerr << "Error: receiver read, errno = " << errno << "\n";
                break;
            } else {
                uint64_t session_id = (uint64_t) buffer;
                //printf("read %zd bytes: %.*s\n", rcv_len, (int) rcv_len, buffer + audiogram::HEADER_SIZE);
                uint64_t id = (uint64_t)buffer + audiogram::HEADER_SIZE;
                std::cerr << "read " << rcv_len << " bytes\n";
                std::cout.write((char *)buffer + 16, rcv_len - 16);
//                for (int j = 0; j < 528; ++j) {
//                    printf("%x", buffer[j] & 0xff);
//                } printf("\n");
            }
        }
        return 0;
    }
};

int main(int argc, char *argv[]) {
    radio_receiver r;
    if (r.init(argc, argv)) return 1;
    r.listen();

    return 0;
}