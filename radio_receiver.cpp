#include <iostream>
#include <string>
#include <cstdint>
#include <cerrno>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <ctime>
#include <chrono>
#include <thread>
#include <mutex>
#include <list>
#include "boost/program_options.hpp"
#include "audiogram.h"
#include "receiver.h"
#include "transmitter.h"
#include "const.h"


class radio_receiver {
private:
    struct station_det {
        struct sockaddr_in addr;
        time_t last_answ;
    };

    static const uint32_t DEFAULT_DISCOVER_ADDR = (uint32_t)-1;
    static const time_t DISCONNECT_INTERVAL = 20; // in seconds
    const int LOOKUP_INTERVAL = 5; // in seconds

    struct sockaddr_in discover_addr;
    struct sockaddr_in mcast_addr = {0};
    in_port_t ctrl_port = (in_port_t)35826;
    in_port_t ui_port = (in_port_t)15826;
    size_t bsize = 8*65536; // TODO change
    std::string station_name;
    size_t psize;
    int rtime = 250; // in milliseconds

    std::map<std::string, std::list<struct station_det>> stations;
    std::vector<audiogram> audio_buf;
    unsigned long out_id = 0;
    unsigned long in_id = 0;
    receiver lookup_tr_reply_rcv; // bound, receives from the same address it sends
    transmitter rexmit_tr;
    receiver reply_rcv;
    receiver mcast_rcv;
    std::mutex mcast_mut;
    std::mutex new_station_mut;
    std::mutex stations_mut;
    std::atomic_flag keep_waiting = ATOMIC_FLAG_INIT;
    std::atomic_flag keep_playing = ATOMIC_FLAG_INIT;
    std::atomic_flag no_refresh_menu = ATOMIC_FLAG_INIT;

public:
    int init(int argc, char *argv[]) {
        namespace po = boost::program_options;
        std::string addr;
        discover_addr.sin_addr.s_addr = htonl(DEFAULT_DISCOVER_ADDR);
        discover_addr.sin_family = AF_INET;

        po::options_description desc("Options");
        desc.add_options()
                (",d", po::value<std::string>(&addr), "discover_addr")
                (",C", po::value<in_port_t>(&ctrl_port), "ctrl_port")
                (",U", po::value<in_port_t>(&ui_port), "ui_port")
                (",b", po::value<size_t>(&bsize), "bsize")
                (",n", po::value<std::string>(&station_name), "name")
                (",r", po::value<int>(&rtime), "rtime");

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
        discover_addr.sin_port = ctrl_port;

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

        lookup_tr_reply_rcv.prepare_to_receive();
        fcntl(lookup_tr_reply_rcv.sock, F_SETFL, O_NONBLOCK);
        rexmit_tr.prepare_to_send();
        keep_waiting.test_and_set();
        keep_playing.test_and_set();
        no_refresh_menu.test_and_set();

        return 0;
    }

    void work() {
        std::ios_base::sync_with_stdio(false);
        std::cin.tie(nullptr);
        std::cerr.tie(nullptr);

        // run other threads
        std::thread t1(&radio_receiver::play, this);
        std::thread t2(&radio_receiver::receive_replies, this);
        // std::thread t3(&send_rexmit, this);
        while (true) {
            delete_inactive_stations();
            send_lookup();
            sleep(LOOKUP_INTERVAL);
        }

        // t1.join();
        // t2.join();
        // t3.join();
    }

private:
    void send_lookup() {
        if (sendto(lookup_tr_reply_rcv.sock, (void*)LOOKUP_MSG, (size_t)LOOKUP_MSG_LEN, 0,
                   (struct sockaddr *)&discover_addr, sizeof(discover_addr)) == -1) {
            std::cerr << "Error: reply sendto, errno = " << errno << "\n";
            std::cerr << "bind: " << inet_ntoa(discover_addr.sin_addr) << " p: " << ntohs(discover_addr.sin_port) << "\n";

        }
        std::cerr << "sent\n";
    }

    void receive_replies() {
        int started_playing = 0;
        time_t start = time(nullptr);

        while (true) {
            do {
                sockaddr_in addr;
                std::string name;

                if (!receive_reply(addr, name)) {
                    std::cerr << "received reply\n";
                    if (!started_playing) {
                        if (station_name.empty()) {
                            started_playing = 1;
                        } else {
                            if (name == station_name) {
                                started_playing = 1;
                            } else {
                                continue;
                            }
                        }
                    }

                    stations_mut.lock();
                    station_det del_station = {0};
                    if (handle_station_change(addr, name, &del_station)) {
                        if (mcast_addr.sin_addr.s_addr == del_station.addr.sin_addr.s_addr &&
                            mcast_addr.sin_port == del_station.addr.sin_port) {
                            if (!stations.empty()) {
                                set_new_station();
                            }
                            no_refresh_menu.clear();
                        }
                    }
                    stations_mut.unlock();
                }
            } while (true);
        }
    }

    void delete_inactive_stations() {
        stations_mut.lock();
        time_t now = time(nullptr);
        for (auto mi = stations.begin(); mi != stations.end();) {
            for (auto li = mi->second.begin(); li != mi->second.end();) {
                ++li;
                if (now - std::prev(li)->last_answ > DISCONNECT_INTERVAL) {
                    std::cerr << "deleting (name " << mi->first << " addr " << inet_ntoa(std::prev(li)->addr.sin_addr) << " port " << ntohs(std::prev(li)->addr.sin_port) << ")\n";
                    mi->second.erase(std::prev(li));
                }
            }
            ++mi;
            if (std::prev(mi)->second.empty()) {
                std::cerr << "deleting name " << prev(mi)->first << "\n";
                stations.erase(std::prev(mi));
            }
        }
        stations_mut.unlock();
    }

    void set_new_station(struct station_det &station) {
        new_station_mut.lock();
        std::cerr << "in 1 mutex\n";
        keep_playing.clear();
        keep_waiting.clear();
        mcast_mut.lock();std::cerr << "in 2 mutex\n";
        mcast_rcv.drop_mcast();
        std::cerr << "set new station " << station.addr.sin_addr.s_addr << " " << inet_ntoa(station.addr.sin_addr) << " p " << station.addr.sin_port << "\n";
        mcast_rcv.prepare_to_receive_mcast(station.addr);
        mcast_mut.unlock();std::cerr << "out 1 mutex\n";
        new_station_mut.unlock();std::cerr << "out 2 mutex\n";
    }

    void set_new_station() {
        if (!stations.empty())
            set_new_station(stations.begin()->second.front());
    }

    /* returns 1 if station list changes, 0 otherwise */
    int handle_station_change(sockaddr_in &addr, std::string &name, station_det *del_station) {
        time_t now = time(nullptr);
        if (stations.count(name)) {
            for (auto li = stations[name].begin(); li != stations[name].end(); ++li) {
                struct station_det &sd = *li;
                if (sd.addr.sin_addr.s_addr == addr.sin_addr.s_addr &&
                    sd.addr.sin_port == addr.sin_port) {
                    if (now - sd.last_answ > DISCONNECT_INTERVAL) {
                        *del_station = sd;
                        stations[name].erase(li);
                        std::cerr << "del station " << inet_ntoa(addr.sin_addr) << "\n";
                        if (stations[name].empty())
                            stations.erase(name);
                        return 1;
                    } else {
                        sd.last_answ = now;
                        std::cerr << "upd station " << name << "\n";
                        return 0;
                    }
                }
            }
        } else {
            struct station_det sd = {addr, now};
            stations[name].push_back(sd);
            std::cerr << "add station " << name << "\n";
            return 1;
        }
    }

    int receive_reply(sockaddr_in &addr, std::string &name) {
        char buffer[MAX_CTRL_MSG_LEN];
        sockaddr_in rcv_addr;
        socklen_t rcv_addr_len;
        ssize_t rcv_len = recv(lookup_tr_reply_rcv.sock, (void *)&buffer, sizeof(buffer), 0);

        if (rcv_len >= 0) {
            // printf("read %zd bytes: %.*s\n", rcv_len, (int) rcv_len, buffer);
            int err = 0;
            buffer[rcv_len] = '\0';

            return parse_reply(buffer, addr, name);
        }

        return 1;
    }

    int parse_reply(char *reply_str, sockaddr_in &addr, std::string &name) {
        int err = 0;
        strtok(reply_str, " ");
        char *token = strtok(nullptr, " ");

        //BOREWICZ_HERE [MCAST_ADDR] [DATA_PORT] [nazwa stacji]
        if (!inet_pton(AF_INET, token, &addr.sin_addr)) {
            err = 1;
        } else { std::cerr<<"repl inet_pton: " << token << "\n";
            std::string port_str(strtok(nullptr, " "));
            std::cerr << "port str " << port_str << "\n";
            try {
                uint32_t port = (uint32_t)std::stoi(port_str);
                std::cerr << "port " << port << "\n";
                port = ntohs(port);
                std::cerr << "cor port " << port << "\n";
                if (port <= 0 || port > 65536)
                    err = 1;
                else addr.sin_port = (in_port_t)port;
            } catch (std::exception const &) {
                err = 1;
            }
        }

        if (!err) { std::cerr << "rpl port: " << addr.sin_port << "\n";
            token = strtok(nullptr, "\n");
            if (token == nullptr || strlen(token) > MAX_NAME_LEN)
                err = 1;
            else
                //name = std::string(token);
            {name = std::string(token); std::cerr << "rpl name: " << name << "\n";}
        }

        return err;
    }

    int play() {
        while (keep_waiting.test_and_set()) {
        }

        int initialized = 0, play = 0, end = 0;
        char buffer[MAX_UDP_MSG_LEN];
        uint64_t session_id, byte_zero;
        struct pollfd polled[2];
        polled[0].fd = STDOUT_FILENO;
        polled[0].events = POLLOUT;
        polled[1].events = POLLIN;

        while (true) {
            initialized = 0;
            play = 0;
            end = 0;
            audiogram a;

            new_station_mut.lock();std::cerr<<"in newstmut\n";
            new_station_mut.unlock();std::cerr<<"after newstmut\n";
            mcast_mut.lock();std::cerr<<"in mcastmut\n";

            polled[1].fd = mcast_rcv.sock;

            while (!end) {
                if (!keep_playing.test_and_set()) {
                    break;
                }

                if (!initialized) {
                    if (!uninitialized_recv(buffer, a)) {
                        session_id = a.get_session_id();
                        byte_zero = a.get_packet_id();
                        audio_buf[0] = a;
                        out_id = 1 % audio_buf.capacity();
                        initialized = 1;
                    }
                    continue;
                }

                if (!play) {
                    a.set_size(psize);
                    ssize_t rcv_len = read(mcast_rcv.sock, (void *)a.get_packet_data(), psize);
                    if (rcv_len < 0) {
                        continue;
                    }
                    if (handle_new_audiogram(session_id, byte_zero, a)) {
                        break;
                    }
                    if (a.get_packet_id() >=
                        byte_zero + psize * audio_buf.capacity() * 3 / 4) {
                        play = 1;
                    }//std::cerr << "bytezero" << byte_zero << " capacity " << audio_buf.capacity() << " pcktid " << a.get_packet_id() << " < " << byte_zero + psize * audio_buf.capacity() * 3 / 4<< "\n";
                } else {//std::cerr<<"now playing\n";
                    polled[0].revents = 0;
                    polled[1].revents = 0;

                    int poll_num = poll(polled, 2, 0);
                    switch (poll_num) {
                    case 0:
                        continue;
                    case 1:
                    case 2:
                        if (polled[1].revents & POLLIN) {
                            a.set_size(psize);
                            ssize_t rcv_len = read(mcast_rcv.sock, (void *)a.get_packet_data(), psize);
                            if (rcv_len < 0) {
                                std::cerr << "Error: receiver read, errno = " << errno << "\n";
                                continue;
                            }

                            if (handle_new_audiogram(session_id, byte_zero, a)) {
                                end = true;
                                break;
                            }
                        }
                        if (polled[0].revents & POLLOUT) {
                            if (audio_buf[out_id].empty()) {
//                                end = true;
//                                break;
                                continue; // TODO del when rexmits
                            }
                            std::cout.write((char *)audio_buf[out_id].get_audio_data(),
                                            psize - audiogram::HEADER_SIZE);
                            std::cout.flush();
                            audio_buf[out_id] = audiogram();
                            out_id = (out_id + 1) % audio_buf.capacity();
                        }
                        break;
                    default: // < 0
                        std::cerr << "Error: rcv poll\n";
                    }
                }
            }
            mcast_mut.unlock();
        }
    }

    /* returns 1 if playing needs to be started again, 0 otherwise */
    int handle_new_audiogram(uint64_t session_id, uint64_t byte_zero, audiogram &a) {
        if (session_id > a.get_session_id())
            return 1;

        uint64_t packet_id = a.get_packet_id();
        if (packet_id <= byte_zero)
            return 0;
        if (((packet_id - byte_zero) % psize) != 0)
            return 0;
        unsigned long buf_id = (packet_id - byte_zero) / psize;
        if (buf_id >= audio_buf.size() + out_id) {std::cerr << "in_id = "<<buf_id<<  " out = "<<out_id<<"\n";
            return 1;}
        buf_id = ((packet_id - byte_zero) / psize) % audio_buf.capacity();

        audio_buf[buf_id] = a;

        return 0;
    }

    int uninitialized_recv(char *buffer, audiogram &a) {std::cerr << "uninrecv in\n";
        ssize_t rcv_len = read(mcast_rcv.sock, (void *)buffer, MAX_UDP_MSG_LEN);
        if (rcv_len < 0) {
            return 1;
        } else {
            psize = (size_t) rcv_len;
            audio_buf = std::vector<audiogram>(bsize / psize);
            a.set_size(psize);
            a.set_session_id(*(uint64_t *)buffer);
            a.set_packet_id(*(uint64_t *)(buffer + sizeof(uint64_t)));
            memcpy(a.get_audio_data(), buffer, psize - audiogram::HEADER_SIZE);
            return 0;
        }
    }
};

int main(int argc, char *argv[]) {
    radio_receiver r;
    if (r.init(argc, argv)) return 1;
    r.work();

    return 0;
}