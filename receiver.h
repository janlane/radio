#ifndef RADIO_RECEIVER_H
#define RADIO_RECEIVER_H

#include <iostream>
#include <netinet/in.h>
#include <arpa/inet.h>

class receiver {
public:
    virtual ~receiver() {
        close(sock);
    }

protected:
    int sock = -1;

    int prepare_to_receive_from(char* dotted_addr, in_port_t port) {
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
        // zakładając, że otrzymany adres został sprawdzony i jest poprawny
        if (inet_pton(AF_INET, dotted_addr, &ip_mreq.imr_multiaddr) == -1) {
            std::cerr << "Error: inet_pton\n";
            err = 1;
        }

        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void*)&ip_mreq, sizeof ip_mreq) < 0) {
            std::cerr << "Error: setsockopt\n";
            err = 1;
        }

        /* podpięcie się pod lokalny adres i port */
        local_address.sin_family = AF_INET;
        local_address.sin_addr.s_addr = htonl(INADDR_ANY);
        local_address.sin_port = htons(port);
        if (bind(sock, (struct sockaddr *)&local_address, sizeof local_address) < 0) {
            std::cerr << "Error: bind, errno = " << errno << "\n";
            err = 1;
        }

        return err;
    }
};


#endif //RADIO_RECEIVER_H
