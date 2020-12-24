#include <cascade/cascade.hpp>
#include <cascade/object.hpp>
#include <chrono>
#include <derecho/core/derecho.hpp>
#include <derecho/utils/logger.hpp>
#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <sstream>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace derecho::cascade;
using derecho::ExternalClientCaller;

using WPCSU = WANPersistentCascadeStore<uint64_t, ObjectWithUInt64Key, &ObjectWithUInt64Key::IK, &ObjectWithUInt64Key::IV, ST_FILE>;
using WPCSS = WANPersistentCascadeStore<std::string, ObjectWithStringKey, &ObjectWithStringKey::IK, &ObjectWithStringKey::IV, ST_FILE>;

#define SHUTDOWN_SERVER_PORT (2300)
#define SLEEP_GRANULARITY_US (50)
// timing unit.
inline uint64_t get_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* telnet server for server remote shutdown */
void wait_for_shutdown(int port) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[1024] = {0};
    const char* response = "shutdown";

    // Creating socket file descriptor
    if((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Forcefully attaching socket to the port 8080
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                  &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if(bind(server_fd, (struct sockaddr*)&address,
            sizeof(address))
       < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if(listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    std::cout << "Press ENTER or send \"shutdown\" to TCP port " << port << " to gracefully shutdown." << std::endl;

    while(true) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        FD_SET(server_fd, &read_set);

        int nfds = ((server_fd > STDIN_FILENO) ? server_fd : STDIN_FILENO) + 1;
        if(select(nfds, &read_set, nullptr, nullptr, nullptr) < 0) {
            dbg_default_warn("failed to wait from remote or local shutdown command.");
            continue;
        }

        if(FD_ISSET(STDIN_FILENO, &read_set)) {
            dbg_default_trace("shutdown server from console.");
            break;
        }

        new_socket = accept(server_fd, (struct sockaddr*)&address,
                            (socklen_t*)&addrlen);
        if(new_socket < -1) {
            dbg_default_warn("failed to receive shutdown with error code:{}.", errno);
        }

        int valread = read(new_socket, buffer, 1024);
        if(valread > 0 && strncmp("shutdown", buffer, strlen("shutdown")) == 0) {
            send(new_socket, response, strlen(response), 0);
            shutdown(new_socket, SHUT_RDWR);
            close(new_socket);
            break;
        }
        shutdown(new_socket, SHUT_RDWR);
        close(new_socket);
    }

    close(server_fd);
}

class PerfCascadeWatcher : public CascadeWatcher<uint64_t, ObjectWithUInt64Key, &ObjectWithUInt64Key::IK, &ObjectWithUInt64Key::IV> {
public:
    // @override
    virtual void operator()(derecho::subgroup_id_t sid,
                            const uint32_t shard_id,
                            const uint64_t& key,
                            const ObjectWithUInt64Key& value,
                            void* cascade_context) {
        dbg_default_info("Watcher is called with\n\tsubgroup id = {},\n\tshard number = {},\n\tkey = {},\n\tvalue = [hidden].", sid, shard_id, key);
    }
};

int do_server() {
    dbg_default_info("Starting cascade sender.");

    /** 1 - group building blocks*/
    derecho::CallbackSet callback_set{
            nullptr,  // delivery callback
            nullptr,  // local persistence callback
            nullptr   // global persistence callback
    };
    derecho::SubgroupInfo si{
            derecho::DefaultSubgroupAllocator({{std::type_index(typeid(WPCSU)),
                                                derecho::one_subgroup_policy(derecho::flexible_even_shards("WPCSU"))},
                                               {std::type_index(typeid(WPCSS)),
                                                derecho::one_subgroup_policy(derecho::flexible_even_shards("WPCSS"))}})};
    PerfCascadeWatcher pcw;
    auto wpcsu_factory = [&pcw](persistent::PersistentRegistry* pr, derecho::subgroup_id_t) {
        return std::make_unique<WPCSU>(pr, &pcw);
    };
    auto wpcss_factory = [&pcw](persistent::PersistentRegistry* pr, derecho::subgroup_id_t) {
        return std::make_unique<WPCSS>(pr);  // TODO: pcw is for uint key, ignore it for now.
    };
    /** 2 - create group */
    derecho::Group<WPCSU, WPCSS> group(callback_set, si, {&pcw} /*deserialization manager*/,
                                       std::vector<derecho::view_upcall_t>{},
                                       wpcsu_factory, wpcss_factory);

    /** 3 - telnet server for shutdown */
    int sport = SHUTDOWN_SERVER_PORT;
    if(derecho::hasCustomizedConfKey("CASCADE_PERF/shutdown_port")) {
        sport = derecho::getConfUInt16("CASCADE_PERF/shutdown_port");
    }
    wait_for_shutdown(sport);
    group.barrier_sync();
    group.leave();
    return 0;
}

struct client_states {
    // 1. transmittion depth for throttling the sender
    //    0 for unlimited.
    const uint64_t max_pending_ops;
    // 2. message traffic
    const uint64_t num_messages;
    const uint64_t message_size;
    // 3. tx semaphore
    std::atomic<uint64_t> idle_tx_slot_cnt;
    std::condition_variable idle_tx_slot_cv;
    std::mutex idle_tx_slot_mutex;
    // 4. future queue semaphore
    std::list<derecho::rpc::QueryResults<std::tuple<persistent::version_t, uint64_t>>> future_queue;
    std::condition_variable future_queue_cv;
    std::mutex future_queue_mutex;
    // 5. timestamps log for statistics
    uint64_t *send_tss, *recv_tss;
    // 7. Thread
    std::thread poll_thread;
    std::thread wait_stability_frontier_thread;
    // 8. future for stability frontier
    const uint64_t target_sf;
    std::condition_variable wait_sf_cv;
    std::mutex wait_sf_mutex;
    std::list<derecho::rpc::QueryResults<int>> wait_sf_queue;
    // derecho::rpc::QueryResults<int> sf_future = NULL;
    // constructor:
    client_states(uint64_t _max_pending_ops, uint64_t _num_messages, uint64_t _message_size, uint64_t _target_sf) : max_pending_ops(_max_pending_ops),
                                                                                                                    num_messages(_num_messages),
                                                                                                                    message_size(_message_size),
                                                                                                                    target_sf(_target_sf) {
        idle_tx_slot_cnt = _max_pending_ops;
        // allocated timestamp space and zero them out
        this->send_tss = new uint64_t[_num_messages];
        this->recv_tss = new uint64_t[_num_messages];
        bzero(this->send_tss, sizeof(uint64_t) * _num_messages);
        bzero(this->recv_tss, sizeof(uint64_t) * _num_messages);
        // start polling thread
        this->poll_thread = std::thread(&client_states::poll_results, this);
        // this->wait_stability_frontier_thread = std::thread(&client_states::waiting_stability_forntier, this);
    }

    // destructor:
    virtual ~client_states() {
        // deallocated timestamp space
        delete this->send_tss;
        delete this->recv_tss;
    }

    // thread
    // waiting stability frontier
    // void waiting_stability_forntier() {
    //     pthread_setname_np(pthread_self(), "waiting_stability_forntier");
    //     dbg_default_trace("waiting stability forntier thread started.");
    //     cout << "waiting stability forntier thread started.\n";
    //     std::list<derecho::rpc::QueryResults<int>> my_wait_sf_queue;
    //     std::unique_lock<std::mutex> lck(this->wait_sf_mutex);
    //     this->wait_sf_cv.wait(lck, [this]() { return !this->wait_sf_queue.empty(); });
    //     lck.unlock();
    //      // wait for sf future

    //     for(auto& f : wait_sf_queue) {
    //         cout << "getting a future.\n";
    //         derecho::rpc::QueryResults<int>::ReplyMap& replies = f.get();
    //         for(auto& reply_pair : replies) {
    //             auto r = reply_pair.second.get();
    //             cout << "reply got :" << r << endl;
    //         }
    //         dbg_default_trace("stability arrived");
    //         uint64_t sf_arrive_time = get_time_us();
    //         cout << "stability frontier arrived using (us) : " << sf_arrive_time - send_tss[0] << endl;
    //     }
    //     dbg_default_trace("wait sf thread shutdown.");
    // }
    // thread
    // polling thread
    void poll_results() {
        pthread_setname_np(pthread_self(), "poll_results");
        dbg_default_trace("poll results thread started.");
        size_t future_counter = 0;
        while(future_counter != this->num_messages) {
            std::list<derecho::rpc::QueryResults<std::tuple<persistent::version_t, uint64_t>>> my_future_queue;
            // wait for a future
            std::unique_lock<std::mutex> lck(this->future_queue_mutex);
            this->future_queue_cv.wait(lck, [this]() { return !this->future_queue.empty(); });
            // get all futures
            this->future_queue.swap(my_future_queue);
            // release lock
            lck.unlock();

            // wait for all futures
            for(auto& f : my_future_queue) {
                derecho::rpc::QueryResults<std::tuple<persistent::version_t, uint64_t>>::ReplyMap& replies = f.get();
                for(auto& reply_pair : replies) {
                    auto r = reply_pair.second.get();
                    dbg_default_trace("polled <{},{}> from <>.", std::get<0>(r), std::get<1>(r), reply_pair.first);
                }
                // log time
                this->recv_tss[future_counter++] = get_time_us();
                // post tx slot semaphore
                if(this->max_pending_ops > 0) {
                    this->idle_tx_slot_cnt.fetch_add(1);
                    this->idle_tx_slot_cv.notify_all();
                }
            }

            // shutdown polling thread.
            if(future_counter == this->num_messages) {
                break;
            }
        }
        dbg_default_trace("poll results thread shutdown.");
    }

    // wait for polling thread
    void wait_poll_all() {
        if(this->poll_thread.joinable()) {
            this->poll_thread.join();
        }
        if(this->wait_stability_frontier_thread.joinable()) {
            this->wait_stability_frontier_thread.join();
        }
    }

    // do_wait_sf
    // void do_wait_sf(const std::function<derecho::rpc::QueryResults<int>()>& func){
    //     auto f = func();
    //     std::unique_lock<std::mutex> wait_sf_lck(this->wait_sf_mutex);
    //     this->wait_sf_queue.emplace_back(std::move(f));
    //     wait_sf_lck.unlock();
    //     this->wait_sf_cv.notify_all();
    // }

    // do_send
    void do_send(uint64_t msg_cnt, const std::function<derecho::rpc::QueryResults<std::tuple<persistent::version_t, uint64_t>>()>& func) {
        // wait for tx slot semaphore
        if(this->max_pending_ops > 0) {
            std::unique_lock<std::mutex> idle_tx_slot_lck(this->idle_tx_slot_mutex);
            this->idle_tx_slot_cv.wait(idle_tx_slot_lck, [this]() { return this->idle_tx_slot_cnt > 0; });
            this->idle_tx_slot_cnt.fetch_sub(1);
            idle_tx_slot_lck.unlock();
        }
        // send
        this->send_tss[msg_cnt] = get_time_us();
        auto f = func();
        // append to future queue
        std::unique_lock<std::mutex> future_queue_lck(this->future_queue_mutex);
        this->future_queue.emplace_back(std::move(f));
        future_queue_lck.unlock();
        this->future_queue_cv.notify_all();
    }

    // print statistics
    void print_statistics() {
        /** print per-message latency
        for (size_t i=0; i<num_messages;i++) {
            std::cout << this->send_tss[i] << "," << this->recv_tss[i] << "\t" << (this->recv_tss[i]-this->send_tss[i]) << std::endl;
        }
        */

        // uint64_t total_bytes = this->num_messages * this->message_size;
        uint64_t total_bytes = 4159899129;
        uint64_t timespan_us = this->recv_tss[this->num_messages - 1] - this->send_tss[0];
        double thp_MiBps, thp_ops, avg_latency_us, std_latency_us;
        {
            thp_MiBps = static_cast<double>(total_bytes) * 1000000 / 1048576 / timespan_us;
            thp_ops = static_cast<double>(this->num_messages) * 1000000 / timespan_us;
        }
        // calculate latency statistics
        {
            double sum = 0.0;
            for(size_t i = 0; i < num_messages; i++) {
                sum += static_cast<double>(this->recv_tss[i] - this->send_tss[i]);
            }
            avg_latency_us = sum / this->num_messages;
            double ssum = 0.0;
            for(size_t i = 0; i < num_messages; i++) {
                ssum += ((this->recv_tss[i] - this->send_tss[i] - avg_latency_us) * (this->recv_tss[i] - this->send_tss[i] - avg_latency_us));
            }
            std_latency_us = sqrt(ssum / (this->num_messages + 1));
        }

        std::cout << "Message Size (KiB): " << static_cast<double>(this->message_size) / 1024 << std::endl;
        std::cout << "Throughput (MiB/s): " << thp_MiBps << std::endl;
        std::cout << "Throughput (Ops/s): " << thp_ops << std::endl;
        std::cout << "Average-Latency (us): " << avg_latency_us << std::endl;
        std::cout << "Latency-std (us): " << std_latency_us << std::endl;
    }
};

inline uint64_t randomize_key(uint64_t& in) {
    static uint64_t random_seed = get_time_us();
    uint64_t x = (in ^ random_seed);
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

int do_client(int argc, char** args) {
    const uint64_t max_distinct_objects = 517295;
    const char* test_type = args[0];
    const uint64_t num_messages = std::stoi(args[1]);
    const int is_wpcss = std::stoi(args[2]);
    const uint64_t max_pending_ops = (argc >= 5) ? std::stoi(args[4]) : 0;
    int target_sf = num_messages - 1;
    int expected_mps = 592;
    // target_sf = 10000;

    if(strcmp(test_type, "put") != 0) {
        std::cout << "TODO:" << test_type << " not supported yet." << std::endl;
        return 0;
    }

    /** 1 - create external client group*/
    derecho::ExternalGroup<WPCSU, WPCSS> group;

    uint64_t msg_size = derecho::getConfUInt64(CONF_SUBGROUP_DEFAULT_MAX_PAYLOAD_SIZE);
    uint32_t my_node_id = derecho::getConfUInt32(CONF_DERECHO_LOCAL_ID);

    /** 2 - test both latency and bandwidth */
    if(is_wpcss) {
        if(derecho::hasCustomizedConfKey("SUBGROUP/WPCSS/max_payload_size")) {
            msg_size = derecho::getConfUInt64("SUBGROUP/WPCSS/max_payload_size") - 128;
        }
        // msg_size = 10240;
        cout << "msg_size: " << msg_size << endl;
        struct client_states cs(max_pending_ops, num_messages, msg_size, target_sf);
        char* bbuf = (char*)malloc(msg_size);
        bzero(bbuf, msg_size);

        ExternalClientCaller<WPCSS, std::remove_reference<decltype(group)>::type>& wpcss_ec = group.get_subgroup_caller<WPCSS>();
        auto members = group.template get_shard_members<WPCSS>(0, 0);
        node_id_t server_id = members[my_node_id % members.size()];
        std::ifstream inFile("/root/lpz/icdcs_cascade/cascade/trace_09_20.csv", std::ios::in);
        std::string lineStr;
        getline(inFile, lineStr);
        uint64_t message_index = 0;
        // cs.do_wait_sf([&target_sf, &wpcss_ec, &server_id]() { return std::move(wpcss_ec.p2p_send<RPC_NAME(wait_for_stability_frontier)>(server_id, target_sf)); });
        wpcss_ec.p2p_send<RPC_NAME(start_wanagent)>(server_id);

        wpcss_ec.p2p_send<RPC_NAME(set_stability_frontier)>(server_id, target_sf);
        uint64_t start_time = get_time_us();
        cout << "start time" << start_time << endl;
        int idx = 0;
        /** the fixed message size with flow control**/
        while(idx != num_messages){
            uint64_t now_time = get_time_us();
            while ((now_time-start_time)/1000000.0*expected_mps > idx && idx != num_messages) {
                ObjectWithStringKey o(std::to_string(randomize_key(message_index) % max_distinct_objects), Blob(bbuf, msg_size));
                cs.do_send(message_index, [&o, &wpcss_ec, &server_id]() { return std::move(wpcss_ec.p2p_send<RPC_NAME(put)>(server_id, o)); });
                idx++;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(SLEEP_GRANULARITY_US));
        }

        /** send with trace **/
        /*
        while(getline(inFile, lineStr)) {
            std::vector<std::string> fields;
            std::stringstream ss(lineStr);
            std::string str;
            while(getline(ss, str, ',')) {
                fields.push_back(str);
            }
            int timestamp = atoi(fields[1].c_str());
            // cout << "time: " << timestamp << endl;
            sleep(timestamp);
            int file_size = atoi(fields[0].c_str());
            // cout << "file size: " << file_size << endl;
            // file_size = msg_size;

            if(file_size < msg_size) {
                // ObjectWithStringKey o(std::to_string(randomize_key(message_index) % max_distinct_objects), Blob(bbuf, file_size));
                ObjectWithStringKey o(std::to_string(message_index), Blob(bbuf, file_size));
                cs.do_send(message_index, [&o, &wpcss_ec, &server_id]() { return std::move(wpcss_ec.p2p_send<RPC_NAME(put)>(server_id, o)); });
                message_index++;
                if(message_index % 10000 == 0) {
                    cout << " message index: " << message_index << endl;
                }
                // cout << " 1message index: " << message_index << endl;
            } else {
                while(file_size > msg_size) {
                    // ObjectWithStringKey o(std::to_string(randomize_key(message_index) % max_distinct_objects), Blob(bbuf, msg_size));
                    ObjectWithStringKey o(std::to_string(message_index), Blob(bbuf, msg_size));
                    cs.do_send(message_index, [&o, &wpcss_ec, &server_id]() { return std::move(wpcss_ec.p2p_send<RPC_NAME(put)>(server_id, o)); });
                    file_size -= msg_size;
                    message_index++;
                    if(message_index % 10000 == 0) {
                        cout << " message index: " << message_index << endl;
                    }
                    // cout << " 2message index: " << message_index << endl;
                }
                if(file_size > 0) {
                    // ObjectWithStringKey o(std::to_string(randomize_key(message_index) % max_distinct_objects), Blob(bbuf, file_size));
                    ObjectWithStringKey o(std::to_string(message_index), Blob(bbuf, file_size));
                    cs.do_send(message_index, [&o, &wpcss_ec, &server_id]() { return std::move(wpcss_ec.p2p_send<RPC_NAME(put)>(server_id, o)); });
                    message_index++;
                    if(message_index % 10000 == 0) {
                        cout << " message index: " << message_index << endl;
                    }
                    // cout << " 3message index: " << message_index << endl;
                }
            }
        }
        */
        free(bbuf);
        cs.wait_poll_all();
        cs.print_statistics();
        auto sf_results = wpcss_ec.p2p_send<RPC_NAME(get_stability_frontier_arrive_time)>(server_id);
        auto& sf_replies = sf_results.get();
        uint64_t sf_arrive_time;
        for(auto& reply_pair : sf_replies) {
            sf_arrive_time = reply_pair.second.get();
        }
        // cout << "stability frontier arrived using (us) : " << sf_arrive_time - cs.send_tss[0] << endl;
        cout << "start time: " << start_time << endl;
        // cout << "sf_arrive_time: " << sf_arrive_time << endl;
        // cout << "stability frontier arrived using (us) : " << sf_arrive_time - start_time << endl;
        // cout << "now from stability frontier arrived using (us) : " << get_time_us() - sf_arrive_time << endl;

    } else {
        // if(derecho::hasCustomizedConfKey("SUBGROUP/WPCSU/max_payload_size")) {
        //     msg_size = derecho::getConfUInt64("SUBGROUP/WPCSU/max_payload_size") - 128;
        // }
        // struct client_states cs(max_pending_ops, num_messages, msg_size);
        // char* bbuf = (char*)malloc(msg_size);
        // bzero(bbuf, msg_size);

        // ExternalClientCaller<WPCSU, std::remove_reference<decltype(group)>::type>& wpcsu_ec = group.get_subgroup_caller<WPCSU>();
        // auto members = group.template get_shard_members<WPCSU>(0, 0);
        // node_id_t server_id = members[my_node_id % members.size()];

        // for(uint64_t i = 0; i < num_messages; i++) {
        //     ObjectWithUInt64Key o(randomize_key(i) % max_distinct_objects, Blob(bbuf, msg_size));
        //     cs.do_send(i, [&o, &wpcsu_ec, &server_id]() { return std::move(wpcsu_ec.p2p_send<RPC_NAME(put)>(server_id, o)); });
        // }
        // free(bbuf);

        // cs.wait_sf_arrive();
        // cs.wait_poll_all();
        // cs.print_statistics();
    }

    return 0;
}

void print_help(std::ostream& os, const char* bin) {
    os << "USAGE:" << bin << " [derecho-config-list --] <client|sender> args..." << std::endl;
    os << "    client args: <test_type> <num_messages> <is_wpcss> [max_pending_ops]" << std::endl;
    os << "        test_type := [put|get]" << std::endl;
    os << "        max_pending_ops is the maximum number of pending operations allowed. Default is unlimited." << std::endl;
    os << "    sender args: N/A" << std::endl;
}

int index_of_first_arg(int argc, char** argv) {
    int idx = 1;
    int i = 2;

    while(i < argc) {
        if(strcmp("--", argv[i]) == 0) {
            idx = i + 1;
            break;
        }
        i++;
    }
    return idx;
}

int main(int argc, char** argv) {
    /** initialize the parameters */
    derecho::Conf::initialize(argc, argv);

    /** check parameters */
    int first_arg_idx = index_of_first_arg(argc, argv);
    if(first_arg_idx >= argc) {
        print_help(std::cout, argv[0]);
        return 0;
    }

    if(strcmp(argv[first_arg_idx], "sender") == 0) {
        return do_server();
    } else if(strcmp(argv[first_arg_idx], "client") == 0) {
        if((argc - first_arg_idx) < 4) {
            std::cerr << "Invalid client args." << std::endl;
            print_help(std::cerr, argv[0]);
            return -1;
        }
        // passing <test_type> <num_messages> <is_wpcss> [tx_deptn]
        return do_client(argc - (first_arg_idx + 1), &argv[first_arg_idx + 1]);
    } else {
        std::cerr << "Error: unknown arg: " << argv[first_arg_idx] << std::endl;
        print_help(std::cerr, argv[0]);
        return -1;
    }
}
