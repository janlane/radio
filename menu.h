#ifndef RADIO_MENU_H
#define RADIO_MENU_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <cstring>
#include <cstdlib>
#include <map>
#include "radio_receiver.cpp"
#include "const.h"
#include "err.h"

#define BUFFER_SIZE     65536
#define TINY_BUF_SIZE   4096
#define QUEUE_LENGTH    128
#define MAX_CLIENTS    (_POSIX_OPEN_MAX - 1)
#define HISTORY_SIZE    3
#define MAX_PORT_NUM    65535
#define CONTINUE  0 // must be unequal END and key codes below
#define END       1 // must be unequal key codes below
#define UP        2
#define DOWN      3
#define ENTER     4
#define OTHER     5
#define DEFAULT_MENU    0
#define DEFAULT_OPTION  1


class next_radio_receiver: protected radio_receiver {
private:
    struct read_history {
        char first; // newest character read
        char second; // second to the newest character read
        char third; // third to the newest character read
        int count; // number of characters saved in the structure, initially 0
    };

    const unsigned char ECHO = 1;
    const unsigned char SGA = 3;
    const unsigned char ESC = 27;
    const unsigned char WILL = 251;
    const unsigned char DO = 253;
    const unsigned char IAC = 255;

    const unsigned char UP_CHARS[3] = {27, 91, 65};
    const unsigned char DOWN_CHARS[3] = {27, 91, 66};
    const unsigned char ENTER_CHARS[2] = {13, 0};

    int tcp_sock = -1;
    read_history HISTORY;
    int max_count = -1;
    ssize_t buffer_position = 0; // number of elements read from the buffer
    ssize_t buffer_volume = 0; // number of elements stored in the buffer


    void reset_server_state() {
        buffer_position = 0;
        buffer_volume = 0;
    }

    int stations_string(std::string &s) {
        int i = 0;
        s.clear();

        for (auto &si : stations) {
            ++i;
            if (si.first == station_name)
                s.append(CHOICE);
            else
                s.append(NO_CHOICE);
            s.append(si.first).append("\r\n");
        }

        return i;
    }

    void print_menu(int action_sock) {
        char buffer[TINY_BUF_SIZE];
        std::string options = "";
        int count = stations_string(options);
        if (count > max_count)
            max_count = count;
        // clears menu space only, prints menu again
        ssize_t length = snprintf(buffer, TINY_BUF_SIZE,
                                  "%c[%d;0H%c[1J%c[1;0H%s%s%s",
                                  ESC,
                                  TOP_LEN + max_count + FOOT_LEN + 1,
                                  ESC,
                                  ESC,
                                  TOP,
                                  options.c_str(),
                                  FOOT);
        ssize_t sent_length = write(action_sock, buffer, length);
        if (sent_length != length)
            syserr("writing to client socket");
    }

    void prepare_client_terminal(int action_sock) {
        char buffer[TINY_BUF_SIZE];
        // request suppressing go-ahead, turning off echo, hiding cursor and clear the screen
        ssize_t length = snprintf(buffer, TINY_BUF_SIZE, "%c%c%c%c%c%c%c[?25l%c[2J",
                                  IAC,
                                  WILL,
                                  SGA,
                                  IAC,
                                  WILL,
                                  ECHO,
                                  ESC,
                                  ESC);
        ssize_t sent_length = write(action_sock, buffer, length);
    }

    void refresh_history(char new_char) {
        HISTORY.third = HISTORY.second;
        HISTORY.second = HISTORY.first;
        HISTORY.first = new_char;
        if (HISTORY.count < HISTORY_SIZE)
            HISTORY.count++;
    }

    int get_key_code(int action_sock, char *buffer) {
        reset_server_state();
        if (buffer_position == buffer_volume) { // load to buffer
            buffer_volume = read(action_sock, buffer, BUFFER_SIZE);
            buffer_position = 0;
            if (buffer_volume < 0)
                syserr("reading from client socket");
            else if (buffer_volume == 0)
                return END;
        }

        while (buffer_position < buffer_volume) {
            refresh_history(buffer[buffer_position]);
            buffer_position++;

//            if (HISTORY.count >= 2 && HISTORY.first == ENTER_CHARS[1] &&
//                HISTORY.second == ENTER_CHARS[0])
//                return ENTER;

            if (HISTORY.count == 3 && HISTORY.third == UP_CHARS[0] &&
                HISTORY.second == UP_CHARS[1]) {
                if (HISTORY.first == UP_CHARS[2])
                    return UP;
                if (HISTORY.first == DOWN_CHARS[2])
                    return DOWN;
            }
        }
        return OTHER;
    }

    void up_action(int action_sock) {
        stations_mut.lock();
        name_mut.lock();
        auto station_id = stations.find(station_name);
        name_mut.unlock();
        if (station_id != stations.begin() && station_id != stations.end())
            set_new_station(std::prev(stations.find(station_name))->second.front());
        print_menu(action_sock);
        stations_mut.unlock();
    }

    void down_action(int action_sock) {
        stations_mut.lock();
        name_mut.lock();
        auto station_id = stations.find(station_name);
        name_mut.unlock();
        if (station_id != stations.end() && std::next(station_id) != stations.end())
            set_new_station(std::next(stations.find(station_name))->second.front());
        print_menu(action_sock);
        stations_mut.unlock();
    }

    void serve_clients() {
        struct pollfd client[MAX_CLIENTS];
        char buffer[BUFFER_SIZE];
        ssize_t buf_len[MAX_CLIENTS], buf_pos[MAX_CLIENTS];
        struct sockaddr_in server;
        ssize_t rval, length;
        int msg_sock, i, ret, key;

        for (i = 1; i < MAX_CLIENTS; ++i) {
            client[i].fd = -1;
            client[i].events = (POLLIN | POLLOUT);
        }
        client[0].fd = tcp_sock;
        client[0].events = POLLIN;

        while (true) {
            key = OTHER;

            for (i = 0; i < _POSIX_OPEN_MAX; ++i)
                client[i].revents = 0;

            ret = poll(client, _POSIX_OPEN_MAX, 5000);
            if (ret > 0) {
                if ((client[0].revents & POLLIN)) {
                    msg_sock = accept(client[0].fd, (struct sockaddr *) nullptr,
                                      (socklen_t *) nullptr);
                    if (msg_sock != -1) {
                        fcntl(msg_sock, F_SETFL, O_NONBLOCK);

                        prepare_client_terminal(msg_sock);
                        stations_mut.lock();
                        print_menu(msg_sock);
                        stations_mut.unlock();
                        for (i = 1; i < _POSIX_OPEN_MAX; ++i) {
                            if (client[i].fd == -1) {
                                client[i].fd = msg_sock;
                                break;
                            }
                        }
                    }
                }

                for (i = 1; i < _POSIX_OPEN_MAX; ++i) {
                    if (client[i].fd != -1 && (client[i].revents & (POLLIN))) {std::cerr << "pregetkey\n";
                        key = get_key_code(client[i].fd, buffer); std::cerr << "getkey\n";
//                        if (rval <= 0) {
//                            close(client[i].fd);
//                            client[i].fd = -1;
//                        } else {
//                            break;
//                        }
                        if (key == UP || key == DOWN) {std::cerr <<"TRUE\n";
                            break;}
                    }
                }
            }

            if (key == UP || key == DOWN || !unchanged_list.test_and_set()) {
                ret = poll(client, _POSIX_OPEN_MAX, 500);
                if (ret > 0) {
                    for (i = 1; i < _POSIX_OPEN_MAX; ++i) {
                        if (client[i].fd != -1 && (client[i].revents & POLLOUT)) {
                            if (key == UP) { std::cerr << "upaction\n";
                                up_action(client[i].fd);
                            } else if (key == DOWN) {// key == DOWN
                                down_action(client[i].fd);std::cerr <<"downadction\n";
                            } else {
                                stations_mut.lock();
                                print_menu(msg_sock);
                                stations_mut.unlock();
                            }
//                            if (rval < 0) {
//                                close(client[i].fd);
//                                client[i].fd = -1;
//                            }
                        }
                    }
                }
            }
        }
    }

public:
    int init(int argc, char *argv[]) {
        struct sockaddr_in server_address;

        tcp_sock = socket(PF_INET, SOCK_STREAM, 0); // creating IPv4 TCP socket
        if (tcp_sock < 0)
            syserr("creating socket");

        server_address.sin_family = AF_INET; // IPv4
        server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
        server_address.sin_port = htons(ui_port); // listening on chosen port

        // bind the socket to a concrete address
        if (bind(tcp_sock, (struct sockaddr *) &server_address, sizeof(server_address)) < 0)
            syserr("binding socket to address");

        // switch to listening (passive open)
        if (listen(tcp_sock, QUEUE_LENGTH) < 0)
            syserr("listening");

        return radio_receiver::init(argc, argv);
    }

    void work() {
        std::thread t(&next_radio_receiver::serve_clients, this);

        radio_receiver::work();

        t.join();
    }
};

#endif //RADIO_MENU_H
