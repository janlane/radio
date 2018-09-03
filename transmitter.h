#ifndef RADIO_TRANSMITTER_H
#define RADIO_TRANSMITTER_H

#include <unistd.h>
#include <sys/socket.h>
#include <iostream>
#include <netinet/in.h>
#include <fcntl.h>

class transmitter {
private:
    static const int TTL = 4;

    void prepare_to_send_helper() {
        int optval, err = 0;

        do {
            sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock < 0) {
                std::cerr << "Error: socket\n";
                err = 1;
            }

            /* uaktywnienie rozgłaszania (ang. broadcast) */
            optval = 1;
            if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void *) &optval, sizeof optval) < 0) {
                std::cerr << "Error: setsockopt broadcast\n";
                err = 1;
            }

            /* ustawienie TTL dla datagramów rozsyłanych do grupy */
            optval = TTL;
            if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (void *) &optval, sizeof optval) < 0) {
                std::cerr << "Error: setsockopt multicast ttl\n";
                err = 1;
            }
        } while (err);
    }
public:
    int sock = -1;

    virtual ~transmitter() {
        close(sock);
    }

    virtual int prepare_to_send() {
        prepare_to_send_helper();
    }

    virtual int prepare_to_send_nonblock() {
        prepare_to_send_helper();
        fcntl(sock, O_NONBLOCK);
    }

};


#endif //RADIO_TRANSMITTER_H
