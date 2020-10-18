#pragma once
#include <functional>
#include <thread>
#include <list>
#include <memory>
#include <map>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <nlohmann/json.hpp>
#include <exception>
#include <wan_agent/wan_agent_type_definitions.hpp>

namespace wan_agent
{

    // Abandon the division of WanAgent base class and TCP implementation class, and use only TCP's WanAgent class.

// configuration entries
#define WAN_AGENT_CONF_VERSION "version"
#define WAN_AGENT_CONF_TRANSPORT "transport"
#define WAN_AGENT_CONF_PRIVATE_IP "private_id"
#define WAN_AGENT_CONF_PRIVATE_PORT "private_port"
#define WAN_AGENT_CONF_LOCAL_SITE_ID "local_site_id"
#define WAN_AGENT_CONF_SITES "sites"
#define WAN_AGENT_CONF_SITES_ID "id"
#define WAN_AGENT_CONF_SITES_IP "ip"
#define WAN_AGENT_CONF_SITES_PORT "port"
#define WAN_AGENT_MAX_PAYLOAD_SIZE "max_payload_size"
#define WAN_AGENT_WINDOW_SIZE "window_size"
#define EPOLL_MAXEVENTS 64
#define WAN_AGENT_CHECK_SITE_ENTRY(x)                                           \
    if (site.find(x) == site.end())                                             \
    {                                                                           \
        throw std::runtime_error(std::string(x) + " missing in a site entry."); \
    }

    /**
     * predicate lambda on the "WAN SST", which is organized as a map.
     * The map key is a node id, while the value is a counter for the number
     * of messages that being acknowleged by the corresponding site.
     * Plesae note that the parameter is a copy of the working 'message_counters'
     * The implementation should provide a function to return a message_counters.
     */
    using PredicateLambda = std::function<void(const std::map<site_id_t, uint64_t> &)>;

    using ReportACKFunc = std::function<void()>;
    using NotifierFunc = std::function<void()>;

    /**
     * remote message callback function type
     * @param const site_id_t: site id.
     * @param const char*: message in byte array
     * @param const size_t: message size.
     */
    using RemoteMessageCallback = std::function<void(const site_id_t, const char *, const size_t)>;

    /**
     * The Wan Agent abstract class
     */
    // TODO: break down into sender and receiver
    class WanAgent
    {
        // private:
    protected:
        std::atomic<bool> is_shutdown;
        /** local site id */
        site_id_t local_site_id;

        std::map<site_id_t, std::pair<ip_addr_t, uint16_t>> sites_ip_addrs_and_ports;

        /**
         * configuration
         */
        const nlohmann::json config;

        /**
         * load configuration from this->config
         */
        void load_config() noexcept(false);

        /**
         * get local ip and port string
         */
        std::string get_local_ip_and_port() noexcept(false);

    public:
        /**
         * constructor
         * @param wan_group_config - the wan_group_config in json.
         * @param pl    - predicate lambda
         * @param rmc   - remote message callback
         */
        WanAgent(const nlohmann::json &wan_group_config);

        /**
         * destructor
         */
        virtual ~WanAgent() {}

        /**
         * get local id
         */
        const site_id_t get_local_site_id() const
        {
            return this->local_site_id;
        }

        /**
         * shutdown the wan agent service and block until finished.
         */
        virtual void shutdown_and_wait() noexcept(false) = 0;
    };

    // TODO: how to have multiple wan agents on one site?
    // I decided to hand this to applications. For example, an application could
    // create a Derecho subgroup with multiple WAN agent nodes, each of which joins
    // a parallel WAN group doing exactly the same thing. In each of the WAN group,
    // the messages is ordered. But no guarantee across WAN groups. The application
    // should taking care of this when they try to leverage the bandwidth benefits of
    // multiple WAN groups.

    struct RequestHeader
    {
        uint64_t seq;
        uint32_t site_id;
        size_t payload_size;
    };

    struct Response
    {
        uint64_t seq;
        uint32_t site_id;
    };

    // the Server worker
    class RemoteMessageService final
    {
    private:
        const site_id_t local_site_id;
        const std::map<site_id_t, std::pair<ip_addr_t, uint16_t>> &sites_ip_addrs_and_ports;
        const size_t max_payload_size;
        const RemoteMessageCallback rmc;

        const NotifierFunc ready_notifier;
        std::atomic<bool> server_ready;
        std::list<std::thread> worker_threads;

        int server_socket;
        /**
         * configuration
         */
        const nlohmann::json config;

    public:
        RemoteMessageService(const site_id_t local_site_id,
                             const std::map<site_id_t, std::pair<ip_addr_t, uint16_t>> &sites_ip_addrs_and_ports,
                             const size_t max_payload_size,
                             const RemoteMessageCallback &rmc,
                             const NotifierFunc &ready_notifier_lambda);

        void establish_connections();

        void worker(int sock);

        bool is_server_ready();
    };

    class WanAgentServer : public WanAgent
    {
    private:
        /** 
         * remote_message_callback is called when a new message is received.
         */
        const RemoteMessageCallback &remote_message_callback;

        RemoteMessageService remote_message_service;
        // the conditional variable for initialization
        std::mutex ready_mutex;           // TODO: 思考下ready的作用究竟是什么
        std::condition_variable ready_cv; // TODO: 思考下ready的作用究竟是什么

    public:
        WanAgentServer(const nlohmann::json &wan_group_config,
                       const RemoteMessageCallback &rmc);
        ~WanAgentServer() {}

        // bool is_ready()
        // {
        //     if (!remote_message_service.is_server_ready())
        //     {
        //         return false;
        //     }

        //     return true;
        // }

        /**
         * shutdown the wan agent service and block until finished.
         */
        virtual void shutdown_and_wait() noexcept(false) override;
    };

    // the Client worker, deprecated
    class MessageSenderRingBuffer final
    {
    private:
        const site_id_t local_site_id;
        int epoll_fd_send_msg;
        int epoll_fd_recv_ack;

        const size_t n_slots;
        size_t head = 0;
        size_t tail = 0;
        size_t size = 0;
        std::vector<std::unique_ptr<char[]>> buf;

        // mutex and condition variables for producer-consumer problem
        std::mutex mutex;
        std::condition_variable not_empty;
        std::condition_variable not_full;

        uint64_t last_all_sent_seqno;
        std::map<site_id_t, uint64_t> last_sent_seqno;
        std::map<int, site_id_t> sockfd_to_site_id_map;

        std::map<site_id_t, std::atomic<uint64_t>> &message_counters;

        const ReportACKFunc report_new_ack;
        const NotifierFunc ready_notifier;

        std::atomic<bool> client_ready;
        std::atomic<bool> thread_shutdown;

    public:
        MessageSenderRingBuffer(const site_id_t &local_site_id,
                                const std::map<site_id_t, std::pair<ip_addr_t, uint16_t>> &sites_ip_addrs_and_ports,
                                const size_t &n_slots, const size_t &max_payload_size,
                                std::map<site_id_t, std::atomic<uint64_t>> &message_counters,
                                const ReportACKFunc &report_new_ack,
                                const NotifierFunc &ready_notifier_lambda);

        void recv_ack_loop();

        void enqueue(const char *payload, const size_t payload_size);

        void send_msg_loop();

        bool is_client_ready();
    };

    struct LinkedBufferNode
    {
        size_t message_size;
        char *message_body;
        LinkedBufferNode *next;

        LinkedBufferNode() {}
    };

    // the Client worker
    class MessageSender final
    {
    private:
        std::list<LinkedBufferNode> buffer_list;
        const site_id_t local_site_id;
        // std::map<site_id_t, int> sockets;
        int epoll_fd_send_msg;
        int epoll_fd_recv_ack;

        const size_t n_slots;
        // size_t head = 0;
        // size_t tail = 0;
        size_t size = 0;
        // std::vector<std::unique_ptr<char[]>> buf;
        // mutex and condition variables for producer-consumer problem
        std::mutex mutex;
        std::condition_variable not_empty;
        std::mutex size_mutex;
        // std::condition_variable not_full;

        uint64_t last_all_sent_seqno;
        std::map<site_id_t, uint64_t> last_sent_seqno;
        std::map<int, site_id_t> sockfd_to_site_id_map;

        std::map<site_id_t, std::atomic<uint64_t>> &message_counters;
        const ReportACKFunc report_new_ack;

        const NotifierFunc ready_notifier;
        std::atomic<bool> client_ready;

        std::atomic<bool> thread_shutdown;

    public:
        // uint64_t *buffer_size = static_cast<uint64_t *>(malloc(sizeof(uint64_t) * N_MSG));
        // uint64_t *time_keeper = static_cast<uint64_t *>(malloc(sizeof(uint64_t) * 4 * N_MSG));
        // uint64_t *ack_keeper = static_cast<uint64_t *>(malloc(sizeof(uint64_t) * 4 * N_MSG));
        MessageSender(const site_id_t &local_site_id,
                      const std::map<site_id_t, std::pair<ip_addr_t, uint16_t>> &sites_ip_addrs_and_ports,
                      const size_t &n_slots, const size_t &max_payload_size,
                      std::map<site_id_t, std::atomic<uint64_t>> &message_counters,
                      const ReportACKFunc &report_new_ack,
                      const NotifierFunc &ready_notifier_lambda);

        void recv_ack_loop();
        void enqueue(const char *payload, const size_t payload_size);
        void send_msg_loop();
        bool is_client_ready()
        {
            return client_ready.load();
        }
    };

    class WanAgentClient : public WanAgent
    {
    private:
        /** the conditional variable and thread for notification */
        std::mutex new_ack_mutex;
        std::condition_variable new_ack_cv;
        bool has_new_ack;
        std::thread predicate_thread;
        /** predicate_loop */
        void predicate_loop();

        /**
         * predicted_lambda is called when an acknowledgement is received.
         */
        const PredicateLambda &predicate_lambda;

        std::unique_ptr<MessageSender> message_sender;
        // the conditional variable for initialization
        std::mutex ready_mutex;           // TODO: 思考下ready的作用究竟是什么
        std::condition_variable ready_cv; // TODO: 思考下ready的作用究竟是什么
        std::thread recv_ack_thread;
        std::thread send_msg_thread;
        std::map<site_id_t, std::atomic<uint64_t>> message_counters;

    public:
        WanAgentClient(const nlohmann::json &wan_group_config,
                       const PredicateLambda &pl);
        ~WanAgentClient() {}

        // bool is_ready()
        // {
        //     if (!message_sender->is_client_ready())
        //     {
        //         return false;
        //     }

        //     return true;
        // }

        virtual void shutdown_and_wait() noexcept(false) override;

        /**
         * report new ack. Implementation should call this to wake up the predicate thread.
         */
        void report_new_ack();

        /**
         * send the message
         */
        virtual uint64_t send(const char *message, const size_t message_size)
        {
            message_sender->enqueue(message, message_size);
            return 0ull;
        }

        /**
         * return a moveable conter table
         */
        std::map<uint32_t, uint64_t> get_message_counters() noexcept(true)
        {
            std::map<uint32_t, uint64_t> counters;
            for (auto &item : message_counters)
            {
                counters[item.first] = item.second.load();
            }
            return std::move(counters);
        }
    };

} // namespace wan_agent
#include "detail/wan_agent_impl.hpp"
