#include <arpa/inet.h>
#include <dlfcn.h>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <vector>

#include <wan_agent/logger.hpp>
#include <wan_agent/wan_agent.hpp>
#include <wan_agent/wan_agent_utils.hpp>

inline uint64_t get_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

namespace wan_agent {

void WanAgent::load_config() noexcept(false) {
    log_enter_func();
    // Check if all mandatory keys are included.
    static const std::vector<std::string> must_have{
            WAN_AGENT_CONF_VERSION,
            // WAN_AGENT_CONF_TRANSPORT, // not so mandatory now.
            WAN_AGENT_CONF_LOCAL_SITE_ID,
            WAN_AGENT_CONF_SERVER_SITES,
            // WAN_AGENT_CONF_SENDER_SITES,
            WAN_AGENT_CONF_NUM_SENDER_SITES,
            // we need to get local ip & port info directly
            WAN_AGENT_CONF_PRIVATE_IP,
            WAN_AGENT_CONF_PRIVATE_PORT};
    for(auto& must_have_key : must_have) {
        if(config.find(must_have_key) == config.end()) {
            throw std::runtime_error(must_have_key + " is not found");
        }
    }
    local_site_id = config[WAN_AGENT_CONF_LOCAL_SITE_ID];
    local_ip = config[WAN_AGENT_CONF_PRIVATE_IP];
    local_port = config[WAN_AGENT_CONF_PRIVATE_PORT];
    num_senders = config[WAN_AGENT_CONF_NUM_SENDER_SITES];
    // Check if sites are valid.
    // if (config[WAN_AGENT_CONF_SENDER_SITES].size() == 0 || config[WAN_AGENT_CONF_SERVER_SITES] == 0)
    if(config[WAN_AGENT_CONF_SERVER_SITES] == 0) {
        throw std::runtime_error("Sites do not have any configuration");
    }
    for(auto& site : config[WAN_AGENT_CONF_SERVER_SITES]) {
        WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_ID);
        WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_IP);
        WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_PORT);
        server_sites_ip_addrs_and_ports.emplace(site[WAN_AGENT_CONF_SITES_ID],
                                                std::make_pair(site[WAN_AGENT_CONF_SITES_IP],
                                                               site[WAN_AGENT_CONF_SITES_PORT]));
    }

    if(config.find(WAN_AGENT_CONF_SENDER_SITES) != config.end()) {
        for(auto& site : config[WAN_AGENT_CONF_SENDER_SITES]) {
            WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_ID);
            WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_IP);
            WAN_AGENT_CHECK_SITE_ENTRY(WAN_AGENT_CONF_SITES_PORT);
            sender_sites_ip_addrs_and_ports.emplace(site[WAN_AGENT_CONF_SITES_ID],
                                                    std::make_pair(site[WAN_AGENT_CONF_SITES_IP],
                                                                   site[WAN_AGENT_CONF_SITES_PORT]));
        }
    }

    log_exit_func();
}  // namespace wan_agent

std::string WanAgent::get_local_ip_and_port() noexcept(false) {
    std::string local_ip;
    unsigned short local_port = 0;
    if(config.find(WAN_AGENT_CONF_PRIVATE_IP) != config.end() && config.find(WAN_AGENT_CONF_PRIVATE_PORT) != config.end()) {
        local_ip = config[WAN_AGENT_CONF_PRIVATE_IP];
        local_port = config[WAN_AGENT_CONF_PRIVATE_PORT];
    } else {
        throw std::runtime_error("Cannot find ip and port configuration for local site.");
    }
    return local_ip + ":" + std::to_string(local_port);
}

WanAgent::WanAgent(const nlohmann::json& wan_group_config, std::string log_level)
        : is_shutdown(false),
          config(wan_group_config) {
    // this->message_counters = std::make_unique<std::map<uint32_t,std::atomic<uint64_t>>>();
    load_config();
    Logger::set_log_level(log_level);
}

RemoteMessageService::RemoteMessageService(const site_id_t local_site_id,
                                           int num_senders,
                                           unsigned short local_port,
                                           const size_t max_payload_size,
                                           const RemoteMessageCallback& rmc,
                                           WanAgent* hugger)
        : local_site_id(local_site_id),
          num_senders(num_senders),
          max_payload_size(max_payload_size),
          rmc(rmc),
          hugger(hugger) {
    std::cout << "1: " << local_site_id << std::endl;
    std::cout << "2" << std::endl;
    sockaddr_in serv_addr;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
        throw std::runtime_error("RemoteMessageService failed to create socket.");

    int reuse_addr = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse_addr,
                  sizeof(reuse_addr))
       < 0) {
        fprintf(stderr, "ERROR on setsockopt: %s\n", strerror(errno));
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(local_port);
    if(bind(fd, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "ERROR on binding to socket: %s\n", strerror(errno));
        throw std::runtime_error("RemoteMessageService failed to bind socket.");
    }
    listen(fd, 5);
    server_socket = fd;
    std::cout << "RemoteMessageService listening on " << local_port << std::endl;
    // dbg_default_info("RemoteMessageService listening on {} ...", local_port);
};

void RemoteMessageService::establish_connections() {
    // TODO: maybe support dynamic join later, i.e. having a infinite loop always listening for join requests?
    while(worker_threads.size() < num_senders) {
        struct sockaddr_storage client_addr_info;
        socklen_t len = sizeof client_addr_info;

        int connected_sock_fd = ::accept(server_socket, (struct sockaddr*)&client_addr_info, &len);
        worker_threads.emplace_back(std::thread(&RemoteMessageService::epoll_worker, this, connected_sock_fd));
    }
}

void RemoteMessageService::worker(int connected_sock_fd) {
    RequestHeader header;
    bool success;
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(max_payload_size);
    std::cout << "worker start" << std::endl;
    while(1) {
        if(connected_sock_fd < 0)
            throw std::runtime_error("connected_sock_fd closed!");

        success = sock_read(connected_sock_fd, header);
        if(!success)
            throw std::runtime_error("Failed to read request header");

        success = sock_read(connected_sock_fd, buffer.get(), header.payload_size);
        if(!success)
            throw std::runtime_error("Failed to read message");

        // dbg_default_info("received msg {} from site {}", header.seq, header.site_id);

        rmc(header.site_id, buffer.get(), header.payload_size);
        success = sock_write(connected_sock_fd, Response{header.seq, local_site_id});
        if(!success)
            throw std::runtime_error("Failed to send ACK message");
    }
}

void RemoteMessageService::epoll_worker(int connected_sock_fd) {
    RequestHeader header;
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(max_payload_size);
    bool success;
    std::cout << "epoll_worker start\n";

    int epoll_fd_recv_msg = epoll_create1(0);
    if(epoll_fd_recv_msg == -1)
        throw std::runtime_error("failed to create epoll fd");
    add_epoll(epoll_fd_recv_msg, EPOLLIN, connected_sock_fd);

    std::cout << "The connected_sock_fd is " << connected_sock_fd << std::endl;

    struct epoll_event events[EPOLL_MAXEVENTS];
    while(!hugger->get_is_shutdown()) {
        int n = epoll_wait(epoll_fd_recv_msg, events, EPOLL_MAXEVENTS, -1);
        for(int i = 0; i < n; i++) {
            if(events[i].events & EPOLLIN) {
                std::cout << "get event from fd " << events[i].data.fd << std::endl;
                // get msg from sender
                success = sock_read(connected_sock_fd, header);
                if(!success) {
                    std::cout << "Failed to read request header, "
                              << "receive " << n << " messages from sender.\n";
                    throw std::runtime_error("Failed to read request header");
                }
                success = sock_read(connected_sock_fd, buffer.get(), header.payload_size);
                if(!success)
                    throw std::runtime_error("Failed to read message");

                // dbg_default_info("received msg {} from site {}", header.seq, header.site_id);

                rmc(header.site_id, buffer.get(), header.payload_size);
                success = sock_write(connected_sock_fd, Response{header.seq, local_site_id});
                if(!success)
                    throw std::runtime_error("Failed to send ACK message");
            }
        }
    }
}

WanAgentServer::WanAgentServer(const nlohmann::json& wan_group_config,
                               const RemoteMessageCallback& rmc, std::string log_level)
        : WanAgent(wan_group_config, log_level),
          remote_message_callback(rmc),
          remote_message_service(
                  local_site_id,
                  num_senders,
                  local_port,
                  wan_group_config[WAN_AGENT_MAX_PAYLOAD_SIZE],
                  rmc,
                  this) {
    std::thread rms_establish_thread(&RemoteMessageService::establish_connections, &remote_message_service);
    rms_establish_thread.detach();

    // deprecated
    // // TODO: for now, all sites must start in 3 seconds; to be replaced with retry mechanism when establishing sockets
    // sleep(3);

    std::cout << "Press ENTER to kill." << std::endl;
    std::cin.get();
    shutdown_and_wait();
}

void WanAgentServer::shutdown_and_wait() {
    log_enter_func();
    is_shutdown.store(true);
    log_exit_func();
}

MessageSender::MessageSender(const site_id_t& local_site_id,
                             const std::map<site_id_t, std::pair<ip_addr_t, uint16_t>>& server_sites_ip_addrs_and_ports,
                             const size_t& n_slots, const size_t& max_payload_size,
                             std::map<site_id_t, std::atomic<uint64_t>>& message_counters,
                             const ReportACKFunc& report_new_ack)
        : local_site_id(local_site_id),
          n_slots(n_slots),  // TODO: useless after using linked list
          last_all_sent_seqno(static_cast<uint64_t>(-1)),
          message_counters(message_counters),
          report_new_ack(report_new_ack),
          thread_shutdown(false) {
    log_enter_func();
    // for(unsigned int i = 0; i < n_slots; i++) {
    //     buf.push_back(std::make_unique<char[]>(sizeof(size_t) + max_payload_size));
    // }

    epoll_fd_send_msg = epoll_create1(0);
    if(epoll_fd_send_msg == -1)
        throw std::runtime_error("failed to create epoll fd");

    epoll_fd_recv_ack = epoll_create1(0);
    if(epoll_fd_recv_ack == -1)
        throw std::runtime_error("failed to create epoll fd");

    for(const auto& [site_id, ip_port] : server_sites_ip_addrs_and_ports) {
        if(site_id != local_site_id) {
            sockaddr_in serv_addr;
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if(fd < 0)
                throw std::runtime_error("MessageSender failed to create socket.");
            int flag = 1;
            int ret = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

            if(ret == -1) {
                fprintf(stderr, "ERROR on setsockopt: %s\n", strerror(errno));
                exit(-1);
            }
            memset(&serv_addr, 0, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons(ip_port.second);

            inet_pton(AF_INET, ip_port.first.c_str(), &serv_addr.sin_addr);
            if(connect(fd, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
                // log_debug("ERROR on connecting to socket: {}", strerror(errno));
                throw std::runtime_error("MessageSender failed to connect socket");
            }
            add_epoll(epoll_fd_send_msg, EPOLLOUT, fd);
            add_epoll(epoll_fd_recv_ack, EPOLLIN, fd);
            sockfd_to_server_site_id_map[fd] = site_id;
            last_sent_seqno.emplace(site_id, static_cast<uint64_t>(-1));
        }
        // sockets.emplace(node_id, fd);
    }
    log_exit_func();
}

void MessageSender::recv_ack_loop() {
    log_enter_func();
    struct epoll_event events[EPOLL_MAXEVENTS];
    while(!thread_shutdown.load()) {
        // std::cout << "in recv_ack_loop, thread_shutdown.load() is " << thread_shutdown.load() << std::endl;
        int n = epoll_wait(epoll_fd_recv_ack, events, EPOLL_MAXEVENTS, -1);
        for(int i = 0; i < n; i++) {
            if(events[i].events & EPOLLIN) {
                // received ACK
                Response res;
                sock_read(events[i].data.fd, res);
                log_info("received ACK from {} for msg {}", res.site_id, res.seq);
                if(message_counters[res.site_id] != res.seq) {
                    throw std::runtime_error("sequence number is out of order for site-" + std::to_string(res.site_id) + ", counter = " + std::to_string(message_counters[res.site_id].load()) + ", seqno = " + std::to_string(res.seq));
                }
                message_counters[res.site_id]++;
                uint64_t pre_cal_st_time = get_time_us();
                predicate_calculation();
                transfer_data_cost += (get_time_us() - pre_cal_st_time) / 1000000.0;
                // if(res.seq == wait_target_sf) {
                //     ack_keeper[res.site_id - 1000] = get_time_us();
                // }
            }
        }
    }
    // std::cout << "in recv_ack_loop, thread_shutdown.load() is " << thread_shutdown.load() << std::endl;
    log_exit_func();
}
void MessageSender::predicate_calculation() {
    log_enter_func();
    std::vector<int> value_ve;
    std::vector<std::pair<site_id_t, uint64_t>> pair_ve;
    value_ve.reserve(message_counters.size());
    pair_ve.reserve(message_counters.size());
    value_ve.push_back(0);
    for(std::map<site_id_t, std::atomic<uint64_t>>::iterator it = message_counters.begin(); it != message_counters.end(); it++) {
        value_ve.push_back(it->second.load());
        pair_ve.push_back(std::make_pair(it->first, it->second.load()));
    }
    int* arr = &value_ve[0];

    int val = predicate(5, arr);
    stability_frontier = pair_ve[val - 1].second;

    /**general recording all sf at every 5000 message**/
    if((stability_frontier + 1) % 5000 == 0) {
        std::cout << stability_frontier << std::endl;
    //     sf_arrive_time_map[stability_frontier] = get_time_us();
    //     for(int i = 1; i < (int)value_ve.size(); i++) {
    //         std::cout << arr[i] << " ";
    //     }
    //     std::cout << std::endl;
    //     all_sf_situation[all_sf_tics * 7] = get_time_us();
    //     int tmp_idx = 1;
    //     for(std::map<std::string, predicate_fn_type>::iterator it = predicate_map.begin(); it != predicate_map.end(); it++) {
    //         int tmp_val = it->second(5, arr);
    //         all_sf_situation[all_sf_tics * 7 + tmp_idx] = pair_ve[tmp_val - 1].second;
    //         tmp_idx++;
    //     }
    //     all_sf_tics++;
    }

    /**wait for certain file size, and record the first time it arrives**/
    // for(std::map<std::string, predicate_fn_type>::iterator it = predicate_map.begin(); it != predicate_map.end(); it++) {
    //     int tmp_val = it->second(5, arr);
    //     int tmp_sf = pair_ve[tmp_val - 1].second;
    //     if(tmp_sf == wait_target_sf) {
    //         if(predicate_arrive_map.count(it->first) == 0) {
    //             predicate_arrive_map[it->first] = get_time_us();
    //         }
    //     }
    // }

    /**record every message arrive to see each file's performance**/
    int predicate_idx = 6;
    for(std::map<std::string, predicate_fn_type>::iterator it = predicate_map.begin(); it != predicate_map.end(); it++) {
        int tmp_val = it->second(5, arr);
        int tmp_sf = pair_ve[tmp_val - 1].second;
        if(tmp_sf > 0 && sf_arrive_time_keeper[tmp_sf * 6 - predicate_idx] == 0) {
            sf_arrive_time_keeper[tmp_sf * 6 - predicate_idx] = get_time_us();
        }
        // if(tmp_sf > 0 && it->first == "MAX_NODE" && who_is_max[tmp_sf] == 0) {
        //     who_is_max[tmp_sf] = pair_ve[tmp_val - 1].first;
        // }

        predicate_idx--;
    }
    stability_frontier_arrive_cv.notify_one();

    /**comparation with gccjit and none gccjit**/
    // uint64_t sf_cal_st_time = get_time_us();
    // complicate_predicate(5, test_arr);
    // predicate_map["Complicated"](5,test_arr);
    // non_gccjit_calculation(test_arr);
    // sf_calculation_cost += (get_time_us() - sf_cal_st_time) / 1000000.0;
    // log_exit_func();
}  // namespace wan_agent

int MessageSender::non_gccjit_calculation(int* seq_vec) {
    int predicate_size = (int)operations.size();
    int operation_num = (int)operations.size();
    // blocks initialization based on # of operator
    for(int i = 0; i < predicate_size; i++) {
        int maxx = -1, minn = 0x7f7f7f7f, kth = 0;
        int maxx_idx = -1, minn_idx = -1, kth_idx = -1;
        int operation_index = operation_num - 1 - i;
        int op = operations[operation_index].op_code;

        switch(op) {
            case 1:  // MAX
                maxx = -1;
                for(int j = 0; j < (int)operations[operation_index].site_range.size(); j++) {
                    if(maxx < seq_vec[operations[operation_index].site_range[j]]) {
                        maxx = seq_vec[operations[operation_index].site_range[j]];
                        maxx_idx = operations[operation_index].site_range[j];
                    }
                }
                if(operations[operation_index].pass_result_to != -1) {
                    // int pass_to = operation_num - 1 - operations[operation_index].pass_result_to;
                    // cout << operations[operation_index].pass_result_to << endl;
                    // operations[pass_to].site_range.push_back(maxx_idx);
                    operations[operations[operation_index].pass_result_to].site_range.push_back(maxx_idx);
                } else {
                    return maxx_idx;
                }
                break;
            case 2:  //MIN
                minn = 0x7f7f7f7f;
                for(int j = 0; j < (int)operations[operation_index].site_range.size(); j++) {
                    if(minn > seq_vec[operations[operation_index].site_range[j]]) {
                        minn = seq_vec[operations[operation_index].site_range[j]];
                        minn_idx = operations[operation_index].site_range[j];
                    }
                }
                if(operations[operation_index].pass_result_to != -1) {
                    // int pass_to = operation_num - 1 - operations[operation_index].pass_result_to;
                    // operations[pass_to].site_range.push_back(minn_idx);
                    operations[operations[operation_index].pass_result_to].site_range.push_back(minn_idx);
                } else {
                    return minn_idx;
                }
                break;
            case 3:  //KTH
                kth = operations[operation_index].kth_k;
                for(int j = 0; j < (int)operations[operation_index].site_range.size() - 1; j++) {
                    for(int k = 0; k < (int)operations[operation_index].site_range.size() - j - 1; k++) {
                        if(seq_vec[operations[operation_index].site_range[k]] > seq_vec[operations[operation_index].site_range[k + 1]]) {
                            int tmp = seq_vec[operations[operation_index].site_range[k]];
                            seq_vec[operations[operation_index].site_range[k]] = seq_vec[operations[operation_index].site_range[k + 1]];
                            seq_vec[operations[operation_index].site_range[k + 1]] = tmp;
                        }
                    }
                }
                if(operations[operation_index].pass_result_to != -1) {
                    int pass_to = operation_num - 1 - operations[operation_index].pass_result_to;
                    operations[pass_to].site_range.push_back(operations[operation_index].site_range[kth - 1]);
                } else {
                    return operations[operation_index].site_range[kth - 1];
                }
                break;
            default:
                break;
        }
    }
}

void MessageSender::wait_stability_frontier_loop(int sf) {
    std::unique_lock<std::mutex> lock(stability_frontier_arrive_mutex);
    stability_frontier_arrive_cv.wait(lock, [this, sf]() { return stability_frontier >= sf; });
    sf_arrive_time = get_time_us();
    stability_frontier_set_cv.notify_one();
}

void MessageSender::enqueue(const char* payload, const size_t payload_size) {
    // std::unique_lock<std::mutex> lock(mutex);
    size_mutex.lock();
    LinkedBufferNode* tmp = new LinkedBufferNode();
    tmp->message_size = payload_size;
    tmp->message_body = (char*)malloc(payload_size);
    memcpy(tmp->message_body, payload, payload_size);
    buffer_list.push_back(*tmp);
    enter_queue_time_keeper[msg_idx++] = get_time_us();
    size_mutex.unlock();
    not_empty.notify_one();
}

void MessageSender::send_msg_loop() {
    log_enter_func();
    struct epoll_event events[EPOLL_MAXEVENTS];
    while(!thread_shutdown.load()) {
        // std::cout << "in send_msg_loop, thread_shutdown.load() is " << thread_shutdown.load() << std::endl;
        std::unique_lock<std::mutex> lock(mutex);
        not_empty.wait(lock, [this]() { return buffer_list.size() > 0; });
        // has item on the queue to send
        int n = epoll_wait(epoll_fd_send_msg, events, EPOLL_MAXEVENTS, -1);
        // log_trace("epoll returned {} sockets ready for write", n);
        for(int i = 0; i < n; i++) {
            if(events[i].events & EPOLLOUT) {
                // socket send buffer is available to send message
                site_id_t site_id = sockfd_to_server_site_id_map[events[i].data.fd];
                // log_trace("send buffer is available for site {}.", site_id);
                auto offset = last_sent_seqno[site_id] - last_all_sent_seqno;
                if(offset == buffer_list.size()) {
                    // all messages on the buffer have been sent for this site_id
                    continue;
                }
                // auto pos = (offset + head) % n_slots;

                size_t payload_size = buffer_list.front().message_size;
                // decode paylaod_size in the beginning
                // memcpy(&payload_size, buf[pos].get(), sizeof(size_t));
                auto curr_seqno = last_sent_seqno[site_id] + 1;
                // log_info("sending msg {} to site {}.", curr_seqno, site_id);
                // send over socket
                // time_keeper[curr_seqno*4+site_id-1] = now_us();
                sock_write(events[i].data.fd, RequestHeader{curr_seqno, local_site_id, payload_size});
                sock_write(events[i].data.fd, buffer_list.front().message_body, payload_size);
                leave_queue_time_keeper[curr_seqno * 7 + site_id - 1000] = get_time_us();
                // buffer_size[curr_seqno] = size;

                last_sent_seqno[site_id] = curr_seqno;
            }
        }

        // static_cast<uint64_t>(-1) will simpliy the logic in the above loop
        // but we need to be careful when computing min_element, since it's actually 0xFFFFFFF
        // but we still want -1 to be the min element.
        auto it = std::min_element(last_sent_seqno.begin(), last_sent_seqno.end(),
                                   [](const auto& p1, const auto& p2) { 
                                           if (p1.second == static_cast<uint64_t>(-1)) {return true;} 
                                           else {return p1.second < p2.second;} });

        // log_debug("smallest seqno in last_sent_seqno is {}", it->second);
        // dequeue from ring buffer
        // || min_element == 0 will skip the comparison with static_cast<uint64_t>(-1)
        if(it->second > last_all_sent_seqno || (last_all_sent_seqno == static_cast<uint64_t>(-1) && it->second == 0)) {
            // log_info("{} has been sent to all remote sites, ", it->second);
            assert(it->second - last_all_sent_seqno == 1);
            // std::unique_lock<std::mutex> list_lock(list_mutex);
            size_mutex.lock();
            buffer_list.pop_front();
            // list_lock.lock();
            size_mutex.unlock();
            // list_lock.unlock();
            last_all_sent_seqno++;
        }
        lock.unlock();
    }

    log_exit_func();
}

WanAgentSender::WanAgentSender(const nlohmann::json& wan_group_config,
                               const PredicateLambda& pl, std::string log_level)
        : WanAgent(wan_group_config, log_level),
          has_new_ack(false),
          predicate_lambda(pl) {
    // std::string pss = "MIN($1,MAX($2,$3))";
    predicate_experssion = wan_group_config[WAN_AGENT_PREDICATE];
    std::istringstream iss(predicate_experssion);
    predicate_generator = new Predicate_Generator(iss);
    predicate = predicate_generator->get_predicate_function();
    std::cout << predicate_experssion << std::endl;
    // start predicate thread.
    // predicate_thread = std::thread(&WanAgentSender::predicate_loop, this);
    for(const auto& pair : server_sites_ip_addrs_and_ports) {
        if(local_site_id != pair.first) {
            message_counters[pair.first] = 0;
        }
    }

    message_sender = std::make_unique<MessageSender>(
            local_site_id,
            server_sites_ip_addrs_and_ports,
            wan_group_config[WAN_AGENT_WINDOW_SIZE],  // TODO: useless after using linked list
            wan_group_config[WAN_AGENT_MAX_PAYLOAD_SIZE],
            message_counters,
            [this]() {});
    // [this]() { this->report_new_ack(); });
    generate_predicate();
    recv_ack_thread = std::thread(&MessageSender::recv_ack_loop, message_sender.get());
    send_msg_thread = std::thread(&MessageSender::send_msg_loop, message_sender.get());

    message_sender->predicate = predicate;
}

// void WanAgentSender::report_new_ack()
// {
//     log_enter_func();
//     std::unique_lock lck(new_ack_mutex);
//     has_new_ack = true;
//     lck.unlock();
//     new_ack_cv.notify_all();
//     log_exit_func();
// }

void WanAgentSender::submit_predicate(std::string key, std::string predicate_str, bool inplace) {
    std::istringstream iss(predicate_str);
    predicate_generator = new Predicate_Generator(iss);
    predicate_fn_type prl = predicate_generator->get_predicate_function();
    if(inplace) {
        predicate = prl;
        message_sender->predicate = predicate;
    }
    predicate_map[key] = prl;
    message_sender->predicate_map[key] = prl;

    if(key == "Complicated") {
        std::vector<pre_operation> pre_vec(std::begin(predicate_generator->driver.operations), std::end(predicate_generator->driver.operations));
        message_sender->operations = pre_vec;
        message_sender->complicate_predicate = prl;
        std::cout << "sender's operation: " << message_sender->operations.size() << std::endl;
    }
    // test_predicate();
}

void WanAgentSender::generate_predicate() {
    // origin
    // std::string predicates[6] = {
    //         "MAX($1,$2,$3,$4,$5,$6,$7)",
    //         "MAX(MAX($2,$3,$4),MAX($5,$6),$7)",
    //         "KTH_MIN($2,MAX($2,$3,$4),MAX($5,$6),$7)",
    //         "MIN(MAX($2,$3,$4),MAX($5,$6),$7)",
    //         "MIN($1,$2,$3,$4,$5,$6,$7)",
    //         "KTH_MIN($4,$1,$2,$3,$4,$5,$6,$7)"};
    // "KTH_MIN($3,$1, $4,$2,$3,$5,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,KTH_MIN($3,$1, $4,$2,$3,$5,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10),KTH_MIN($3,$1, $4,$2,$3,$5,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10),KTH_MIN($3,$1, $4,$2,$3,$5,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10),KTH_MIN($3,$1, $4,$2,$3,$5,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10),KTH_MIN($3,$1, $4,$2,$3,$5,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10),KTH_MIN($3,$1, $4,$2,$3,$5,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10),KTH_MIN($3,$1, $4,$2,$3,$5,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10),KTH_MIN($3,$1, $4,$2,$3,$5,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10),KTH_MIN($3,$1, $4,$2,$3,$5,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10,$6, $7,$8,$9,$10))"};
    
    // new
    std::string predicates[6] = {
            "MAX($1,$2,$3,$4,$5,$6,$7)",
            "MAX(MAX($2,$3,$4,$5),$6,$7)",
            "KTH_MIN($2,MAX($2,$3,$4,$5),$6,$7)",
            "MIN(MAX($2,$3,$4,$5),$6,$7)",
            "MIN($1,$2,$3,$4,$5,$6,$7)",
            "KTH_MIN($4,$1,$2,$3,$4,$5,$6,$7)"};
    std::string keys[6] = {
            "MAX_NODE",
            "MAX_REGION",
            "MAJ_REGION",
            "MIN_REGION",
            "MIN_NODE",
            "MAJ_NODE"};
    // "Complicated"};
    for(int i = 0; i < 6; i++) {
        submit_predicate(keys[i], predicates[i], false);
    }
}

void WanAgentSender::set_stability_frontier(int sf) {
    message_sender->wait_target_sf = sf;
    std::cout << "msg senderwaiting for " << message_sender->wait_target_sf << std::endl;
    wait_sf_thread = std::thread(&MessageSender::wait_stability_frontier_loop, message_sender.get(), sf);
}

uint64_t WanAgentSender::get_stability_frontier_arrive_time() {
    std::unique_lock<std::mutex> lock(message_sender->stability_frontier_set_mutex);
    message_sender->stability_frontier_set_cv.wait(lock, [this]() { return message_sender->sf_arrive_time != 0; });
    return message_sender->sf_arrive_time;
}
int WanAgentSender::get_stability_frontier() {
    return message_sender->stability_frontier;
}

void WanAgentSender::change_predicate(std::string key) {
    log_debug("changing predicate to {}", key);
    if(predicate_map.find(key) != predicate_map.end()) {  // 0-success
        predicate = predicate_map[key];
        message_sender->predicate = predicate;
        log_debug("change success");
    } else {  //1-error
        log_debug("change failed");
        throw std::runtime_error(key + "predicate is not found");
    }

    // test_predicate();
}

void WanAgentSender::test_predicate() {
    int arr[6] = {0, 3, 7, 1, 5, 9};
    for(auto it = predicate_map.begin(); it != predicate_map.end(); it++) {
        int val = it->second(5, arr);
        std::cout << "test_predicate " << it->first << " returned: " << val << std::endl;
    }
    int cur = predicate(5, arr);
    log_debug("current test_predicate returned: {}", cur);
}
void WanAgentSender::out_out_file() {
    std::ofstream file("./enter_leave.csv");
    if(file) {
        file << "enter_time,s1,s2,s3,s4,s5,s6,s7\n";
        for(int i = 0; i < message_sender->msg_idx; i++) {
            file << message_sender->enter_queue_time_keeper[i] << "," << message_sender->leave_queue_time_keeper[i * 7] << "," << message_sender->leave_queue_time_keeper[i * 7 + 1] << "," << message_sender->leave_queue_time_keeper[i * 7 + 2] << "," << message_sender->leave_queue_time_keeper[i * 7 + 3] << "," << message_sender->leave_queue_time_keeper[i * 7 + 4] << "," << message_sender->leave_queue_time_keeper[i * 7 + 5] << "," << message_sender->leave_queue_time_keeper[i * 7 + 6] << "\n";
        }
    }
    file.close();

    // std::ofstream file1("./all_sf.csv");
    // if(file1) {
    //     file1 << "timestamp,";
    //     for(std::map<std::string, predicate_fn_type>::iterator it = predicate_map.begin(); it != predicate_map.end(); it++) {
    //         file1 << it->first << ",";
    //     }
    //     file1 << "\n";
    //     for(int i = 0; i < message_sender->all_sf_tics; i++) {
    //         file1 << message_sender->all_sf_situation[i * 7] << "," << message_sender->all_sf_situation[i * 7 + 1] << "," << message_sender->all_sf_situation[i * 7 + 2] << "," << message_sender->all_sf_situation[i * 7 + 3] << "," << message_sender->all_sf_situation[i * 7 + 4] << "," << message_sender->all_sf_situation[i * 7 + 5] << "," << message_sender->all_sf_situation[i * 7 + 6] << "\n";
    //     }
    // }
    // file1.close();

    /**wait for certain file size, and record the first time it arrives**/
    // std::string file2name = "./" + std::to_string(message_sender->wait_target_sf + 1) + "_files_arrive_time.csv";
    // std::ofstream file2(file2name);
    // if(file2) {
    //     file2 << "predicate,timestamp\n";
    //     file2 << "start," << message_sender->enter_queue_time_keeper[0] << "\n";
    //     for(std::map<std::string, uint64_t>::iterator it = message_sender->predicate_arrive_map.begin(); it != message_sender->predicate_arrive_map.end(); it++) {
    //         file2 << it->first << "," << (it->second - message_sender->enter_queue_time_keeper[0]) / 1000000.0 << "\n";
    //     }
    // }
    // file2.close();

    /**record every message arrive to see each file's performance**/
    std::ofstream file3("./all_sf_for_each_msg.csv");
    if(file3) {
        file3 << "enter_time,";
        for(std::map<std::string, predicate_fn_type>::iterator it = predicate_map.begin(); it != predicate_map.end(); it++) {
            file3 << it->first << ",";
        }
        file3 << "who_is_max";
        file3 << "\n";
        for(int i = 0; i < message_sender->msg_idx; i++) {
            file3 << message_sender->enter_queue_time_keeper[i] << "," << message_sender->sf_arrive_time_keeper[i * 6] << "," << message_sender->sf_arrive_time_keeper[i * 6 + 1] << "," << message_sender->sf_arrive_time_keeper[i * 6 + 2] << "," << message_sender->sf_arrive_time_keeper[i * 6 + 3] << "," << message_sender->sf_arrive_time_keeper[i * 6 + 4] << "," << message_sender->sf_arrive_time_keeper[i * 6 + 5] << "," << message_sender->who_is_max[i] << "\n";
        }
    }
    file3.close();

    /**record ack to calculate the utility of the bandwidth**/
    // std::ofstream file4("./ack_keeper_bandwidth.csv");
    // if(file4) {
    //     file4 << "time,s1,s2,s3,s4,s5,s6,s7\n";
    //     file4 << message_sender->enter_queue_time_keeper[0] << ",";
    //     for(int i = 0; i < 7; i++) {
    //         file4 << message_sender->ack_keeper[i] << ",";
    //     }
    //     file4 << "\n";
    // }
    // file4.close();
}

void WanAgentSender::shutdown_and_wait() {
    std::cout << "all done! " << get_time_us() << std::endl;
    std::cout << "all done used " << (message_sender->sf_arrive_time - message_sender->enter_queue_time_keeper[0]) / 1000000.0 << std::endl;
    // std::cout << "sf cal cost " << message_sender->sf_calculation_cost / 100000.0 << std::endl;
    std::cout << "total sf cal cost " << message_sender->transfer_data_cost / 100000.0 << std::endl;
    // std::cout << "per latency " << ((message_sender->sf_arrive_time - message_sender->enter_queue_time_keeper[0]) / 1000000.0) / 100000 << std::endl;
    log_enter_func();
    is_shutdown.store(true);
    // report_new_ack(); // to wake up all predicate_loop threads with a pusedo "new ack"
    // predicate_thread.join();
    out_out_file();

    message_sender->shutdown();
    // send_msg_thread.join();
    // recv_ack_thread.join();
    std::cout << "send_msg_thread.joinable(): " << send_msg_thread.joinable() << ", recv_ack_thread.joinable(): " << recv_ack_thread.joinable() << std::endl;
    send_msg_thread.detach();
    recv_ack_thread.detach();
    wait_sf_thread.detach();
    log_exit_func();
}

}  // namespace wan_agent