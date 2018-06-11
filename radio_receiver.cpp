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
#include <sys/time.h>
#include <atomic>
#include <unordered_map>
#include "boost/program_options.hpp"
#include "audiogram.h"
#include "receiver.h"
#include "transmitter.h"
#include "const.h"

static uint64_t count = 0;
class radio_receiver {
private:
    struct station_det {
        struct sockaddr_in addr;
        struct sockaddr_in direct;
        std::string name;
        time_t last_answ;
    };

    struct rexmit_data {
        uint64_t id;
        struct sockaddr_in direct;
    };

    static const uint32_t DEFAULT_DISCOVER_ADDR = (uint32_t)-1;
    static const time_t DISCONNECT_INTERVAL = 20; // in seconds
    static const int LOOKUP_INTERVAL = 5; // in seconds

    /* current station data */
    struct sockaddr_in direct_addr;
    std::string station_name;
    struct sockaddr_in mcast_addr = {0};

    struct sockaddr_in discover_addr;
    in_port_t ctrl_port = (in_port_t)35826;
    in_port_t ui_port = (in_port_t)15826;
    size_t bsize = 8*65536; // TODO change
    size_t psize;
    unsigned long rtime = 250; // in milliseconds

    std::map<std::string, std::list<struct station_det>> stations;
    std::vector<audiogram> audio_buf;
    unsigned long out_id = 0;
    receiver lookup_tr_reply_rcv; // bound, receives from the same address it sends
    transmitter rexmit_tr;
    transmitter direct_tr;
    receiver mcast_rcv;
    std::mutex current_mut;
    std::mutex direct_mut;
    std::mutex new_station_mut;
    std::mutex stations_mut;
    std::mutex name_mut;
    std::atomic<uint64_t> last_id_written;
    std::atomic_flag keep_waiting = ATOMIC_FLAG_INIT;
    std::atomic_flag keep_playing = ATOMIC_FLAG_INIT;
    std::atomic_flag no_refresh_menu = ATOMIC_FLAG_INIT;
    std::vector<std::mutex> rexmit_batch_mut;
    std::vector<std::unordered_map<std::string, std::list<rexmit_data>>> rexmit_batch;

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
                (",r", po::value<unsigned long>(&rtime), "rtime");

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

        rexmit_batch_mut = std::vector<std::mutex>(rtime);
        rexmit_batch =
            std::vector<std::unordered_map<std::string, std::list<rexmit_data>>>(rtime);
        lookup_tr_reply_rcv.prepare_to_receive();
        fcntl(lookup_tr_reply_rcv.sock, F_SETFL, O_NONBLOCK);
        rexmit_tr.prepare_to_send();
        direct_tr.prepare_to_send_nonblock();
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
        //std::thread t3(&radio_receiver::send_rexmit, this);

        while (true) {
            //delete_inactive_stations();std::cerr <<" bef sendlookup\n";
            send_lookup();std::cerr << "aft sendlookup\n";
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
                sockaddr_in addr, direct;
                std::string name;

                if (!receive_reply(addr, direct, name)) {
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
                    std::cerr << "bef st mut replies" << "\n";
                    stations_mut.lock(); std::cerr << "in st mut replies" << "\n";
                    station_det del_station = {0};
                    if (handle_stations_update(addr, direct, name, &del_station)) {
                        name_mut.lock();
                        if (del_station.name == station_name) {
                            name_mut.unlock();
                            std::cerr << "LIST upd " << inet_ntoa(mcast_addr.sin_addr) << "\n";
                            if (!stations.empty()) {
                                set_new_station();
                            }
                            no_refresh_menu.clear();
                        } else {
                            name_mut.unlock();
                        }
                    }
                    stations_mut.unlock();std::cerr << "out st mut replies" << "\n";
                }
            } while (true);
        }
    }

    void delete_inactive_stations() { std::cerr << "in del inact" << "\n";
        stations_mut.lock();
        time_t now = time(nullptr);
        station_det del_station = {0};
        for (auto mi = stations.begin(); mi != stations.end();) {
            for (auto li = mi->second.begin(); li != mi->second.end();) {
                ++li;
                if (now - std::prev(li)->last_answ > DISCONNECT_INTERVAL) {
                    std::cerr << "deleting (name " << mi->first << " addr " << inet_ntoa(std::prev(li)->addr.sin_addr) << " port " << ntohs(std::prev(li)->addr.sin_port) << ")\n";
                    del_station = *(std::prev(li));
                    clean_rexmits(del_station.name);
                    mi->second.erase(std::prev(li));
                }
            }
            ++mi;
            if (std::prev(mi)->second.empty()) {
                std::cerr << "deleting name " << prev(mi)->first << "\n";
                stations.erase(std::prev(mi));
            }
        }std::cerr << "maintenance " << inet_ntoa(mcast_addr.sin_addr) << " " << ntohs(mcast_addr.sin_port) << "\n";
        std::cerr << "m. played " << inet_ntoa(del_station.addr.sin_addr) << " " << ntohs(del_station.addr.sin_port) << "\n";
        if (mcast_addr.sin_addr.s_addr == del_station.addr.sin_addr.s_addr &&
            mcast_addr.sin_port == del_station.addr.sin_port) {
            if (!stations.empty()) {
                set_new_station();
            }
            no_refresh_menu.clear();
        }
        stations_mut.unlock();std::cerr << "out del inact" << "\n";
    }

    void clean_rexmits(std::string &name) {
        for (unsigned int i = rtime - 1; i >= 0; --i) {
            rexmit_batch_mut[i].lock();
            rexmit_batch[i].erase(name);
            rexmit_batch_mut[i].unlock();
        }
    }

    void set_new_station(struct station_det &station) {
        new_station_mut.lock();
        std::cerr << "in 1 mutex\n";
        keep_playing.clear();
        keep_waiting.clear();

        current_mut.lock();std::cerr << "in 2 mutex\n";
        mcast_rcv.drop_mcast();
        mcast_addr = station.addr;
        mcast_rcv.prepare_to_receive_mcast(station.addr);

        name_mut.lock();
        station_name = station.name;
        name_mut.unlock();

        direct_mut.lock();
        direct_addr = station.direct;
        direct_mut.unlock();

        current_mut.unlock();std::cerr << "out 1 mutex\n";

        new_station_mut.unlock();std::cerr << "out 2 mutex\n";
    }

    void set_new_station() {
        if (!stations.empty() && !stations.begin()->second.empty())
            set_new_station(stations.begin()->second.front());
    }

    /* returns 1 if station list changes, 0 otherwise */
    int handle_stations_update(sockaddr_in &addr, sockaddr_in &direct,
                               std::string &name, station_det *del_station) {std::cerr << "handle in" << "\n";
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
                        sd.direct = direct;
                        sd.name = name;
                        std::cerr << "upd station " << name << "\n";
                        return 0;
                    }
                }
            }
        } else {
            struct station_det sd = {addr, direct, name, now};
            stations[name].push_back(sd);
            std::cerr << "add station " << name << inet_ntoa(addr.sin_addr) << " " << ntohs(addr.sin_port) << " direct " << inet_ntoa(direct.sin_addr) << " " << ntohs(direct.sin_port) << "\n";
            return 1;
        }
    }

    int receive_reply(sockaddr_in &addr, sockaddr_in &direct, std::string &name) {
        char buffer[MAX_CTRL_MSG_LEN];
        // sockaddr_in rcv_addr;
        socklen_t rcv_addr_len = (socklen_t)sizeof(direct);
        ssize_t rcv_len = recvfrom(lookup_tr_reply_rcv.sock, (void *)&buffer,
                sizeof(buffer), 0, (struct sockaddr *)&direct, &rcv_addr_len);

        if (rcv_len > 0) {
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
                //port = htons(port);
                 std::cerr << "port network ord " << port << "\n";
                if (ntohs(port) <= 0 || ntohs(port) > 65536)
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
        uint64_t max_id_read;
        last_id_written = (uint64_t)-1;
        struct pollfd polled[2];
        polled[0].fd = STDOUT_FILENO;
        polled[0].events = POLLOUT;
        polled[1].events = POLLIN;

        while (true) {
            initialized = 0;
            play = 0;
            end = 0;
            last_id_written = (uint64_t)-1;
            audiogram a;

            new_station_mut.lock();std::cerr<<"in newstmut\n";
            new_station_mut.unlock();std::cerr<<"after newstmut\n";
            current_mut.lock();std::cerr<<"in mcastmut\n";

            polled[1].fd = mcast_rcv.sock;

            while (!end) {
                if (!keep_playing.test_and_set()) {
                    break;
                }

                if (!initialized) {
                    if (!uninitialized_recv(buffer, a)) {
                        session_id = a.get_session_id();
                        byte_zero = a.get_packet_id();
                        max_id_read = byte_zero;
                        audio_buf[0] = a;
                        out_id = 0;
                        initialized = 1;
                    }
                    continue;
                }

                if (!play) {
                    a.set_size(psize);
                    ssize_t rcv_len = read(mcast_rcv.sock, (void *)a.get_packet_data(), psize);
                    if (rcv_len < 0) {
                        continue;
                    }//std::cerr<<"rcv ok";
                    if (handle_new_audiogram(session_id, byte_zero, max_id_read, a)) {
                        break;
                    }//std::cerr << "not play aft handle";
                    if (a.get_packet_id() >=
                        byte_zero + psize * audio_buf.capacity() * 3 / 4) {
                        play = 1;
                    }//std::cerr << "bytezero" << byte_zero << " capacity " << audio_buf.capacity() << " pcktid " << a.get_packet_id() << " < " << byte_zero + psize * audio_buf.capacity() * 3 / 4<< "\n";
                } else {
                    polled[0].revents = 0;
                    polled[1].revents = 0;

                    int poll_num = poll(polled, 2, 0);
                    switch (poll_num) {
                    case 0:
                        continue;
                    case 1:
                    case 2:
                        if (polled[0].revents & POLLOUT) {std::cerr<<"FF " << out_id << "/" << audio_buf.capacity() << "JJ";
                            if (audio_buf[out_id].empty() ||
                                audio_buf[out_id].get_packet_id() != last_id_written + psize &&
                                last_id_written + 1 != 0) {std::cerr<<"HH";
                                end = true;
                                break;
                            }std::cerr<<"GG";
                            std::cout.write((char *)audio_buf[out_id].get_audio_data(),
                                            psize - audiogram::HEADER_SIZE);
                            //std::cout.flush(); // keep deleted
                            last_id_written = audio_buf[out_id].get_packet_id();
                            out_id = (out_id + 1) % audio_buf.capacity();
                        }
                        if (polled[1].revents & POLLIN) {
                            a.set_size(psize);
                            ssize_t rcv_len = read(mcast_rcv.sock, (void *)a.get_packet_data(), psize);
                            if (rcv_len < 0) {
                                std::cerr << "Error: receiver read, errno = " << errno << "\n";
                                continue;
                            }

                            if (handle_new_audiogram(session_id, byte_zero, max_id_read, a)) {
                                end = true;
                                break;
                            }
                        }
                    //default: // < 0
                        //std::cerr << "Error: rcv poll " << errno << "\n";
                    }
                }
            }
            current_mut.unlock();
        }
    }

    /* returns 1 if playing needs to be started again, 0 otherwise */
    int handle_new_audiogram(uint64_t session_id, uint64_t byte_zero,
                             uint64_t &max_id_read, audiogram &a) {//std::cerr<<"innewaudg ";
        if (session_id > a.get_session_id())
            return 1;

        uint64_t packet_id = a.get_packet_id();
        if (packet_id <= byte_zero)
            return 0;
        if (((packet_id - byte_zero) % psize) != 0)
            return 0;
        unsigned long buf_id = (packet_id - byte_zero) / psize;
        if (buf_id >= audio_buf.size() + out_id) {
            std::cerr << "in_id = " << buf_id << " out = " << out_id << "\n";
            return 1;
        }
        buf_id = ((packet_id - byte_zero) / psize) % audio_buf.capacity();

        if (packet_id > max_id_read + psize) {
            for (unsigned long i = (max_id_read / psize) % audio_buf.capacity() + 1; // max index
                    i < buf_id; ++i)
                audio_buf[i].clear();
            add_rexmit(max_id_read + psize, packet_id - psize);
        }
//        if (packet_id >= max_id_read + psize)
//            max_id_read = packet_id;

        audio_buf[buf_id] = a;

        std::cerr << "count " << count++ << " ";
        return 0;
    }

    void add_rexmit(uint64_t min, uint64_t max) {
        if (min <= max) {std::cerr << "min " << min << " max " << max << "\n";
            struct timeval moment;
            gettimeofday(&moment, nullptr);
            int batch = (int)((moment.tv_sec * 1000 + moment.tv_usec / 1000) % rtime);
            rexmit_batch_mut[batch].lock();
            for (uint64_t i = min; i <= max; i+=psize) {//rexmit_batch[batch][station_name].
                rexmit_batch[batch][station_name].push_back({i, direct_addr});
                //std::cerr << "rexmit add to list " << i << " -> " << inet_ntoa(direct_addr.sin_addr) << "\n";
            }
            rexmit_batch_mut[batch].unlock();
        }
    }

    void send_rexmit() {
        while (true) {
            for (int i = 0; i < rtime; ++i) {
                rexmit_batch_mut[i].lock();
                name_mut.lock();
                std::string name = station_name;
                name_mut.unlock();
                std::list<rexmit_data> &rdl = rexmit_batch[i][name];

                for (auto mi = rexmit_batch[i].begin();
                        mi != rexmit_batch[i].end(); ++mi) {
                    std::string msg(REXMIT_MSG);
                    for (auto li = mi->second.begin();
                            li != mi->second.end();) {
                        ++li;
                        if (mi->first == name &&
                            std::prev(li)->id <= last_id_written) {
                            rdl.erase(std::prev(li));
                        } else {
                            msg.append(std::to_string(std::prev(li)->id));
                            if (li != mi->second.end())
                                msg.append(",");
                        }
                    }
                    if (!rdl.empty()) {
                        msg.append("\n");
                        std::cerr << msg;
                        sendto(direct_tr.sock, (void *) msg.c_str(), msg.size(),
                               0, (struct sockaddr *) &rdl.front().direct,
                               sizeof(rdl.front().direct));
                    } else {
                        rexmit_batch[i].erase(name);
                    }
                }
                rexmit_batch_mut[i].unlock();
            }
        }
    }

    int uninitialized_recv(char *buffer, audiogram &a) {
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