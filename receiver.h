#ifndef RADIO_RECEIVER_H
#define RADIO_RECEIVER_H

#include <iostream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

class receiver {
public:
    virtual ~receiver() {
        close(sock);
    }

    int sock = -1;

    void prepare_to_receive(in_port_t port = 0) {
        int err;
        sockaddr_in server_address;

        do {
            err = 0;
            close(sock);
            sock = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
            if (sock < 0) {
                std::cerr << "Error: rcv socket, errno = " << errno << "\n";
                err = 1;
            }

            int optval = 1;
            if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void *) &optval, sizeof optval) < 0) {
                std::cerr << "Error: setsockopt broadcast\n";
                err = 1;
            }

            server_address.sin_family = AF_INET; // IPv4
            server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
            server_address.sin_port = htons(port); // default port for receiving is PORT_NUM

            // bind the socket to a concrete address
            if (bind(sock, (struct sockaddr *) &server_address,
                     (socklen_t) sizeof(server_address)) < 0) {
                std::cerr << "Error: rcv bind, errno = " << errno << "\n";
                err = 1;
            }
        } while (err);
    }

    int prepare_to_receive_mcast(sockaddr_in addr) {
        /* zmienne i struktury opisujące gniazda */
        int err = 0;
        struct sockaddr_in local_address;
        struct ip_mreq ip_mreq;

        /* otworzenie gniazda */
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cerr << "Error: socket\n";
            err = 1;
        }

        /* podpięcie się do grupy rozsyłania (ang. multicast) */
        ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        ip_mreq.imr_multiaddr = addr.sin_addr;
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void*)&ip_mreq, sizeof(ip_mreq)) < 0) {
            std::cerr << "Error: mcast rcv setsockopt\n";
            err = 1;
        }

        /* podpięcie się pod lokalny adres i port */
        // if (sock == -1) {
            local_address.sin_family = AF_INET;
            local_address.sin_addr.s_addr = htonl(INADDR_ANY);
            local_address.sin_port = addr.sin_port;
            if (bind(sock, (struct sockaddr *) &local_address, sizeof(local_address)) < 0) {
                std::cerr << "Error: bind, errno = " << errno << "\n";
                err = 1;
            }
        // }
        fcntl(sock, F_SETFL, O_NONBLOCK); // TODO del?
        return err;
    }

    int drop_mcast() {
        close(sock);
    }
};


#endif //RADIO_RECEIVER_H
