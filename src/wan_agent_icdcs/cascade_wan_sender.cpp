#include <cascade/cascade.hpp>
#include <cascade/object.hpp>
#include <cascade/service.hpp>
#include <cascade/service_types.hpp>
#include <derecho/conf/conf.hpp>
#include <derecho/utils/logger.hpp>
#include <dlfcn.h>
#include <sys/prctl.h>
#include <wan_agent/wan_agent.hpp>

#define PROC_NAME "wan_cascade_test_sender"

using namespace derecho;
using namespace derecho::cascade;

#ifndef NDEBUG
inline void dump_layout(const json& layout) {
    int tid = 0;
    for(const auto& pertype : layout) {
        int sidx = 0;
        for(const auto& persubgroup : pertype) {
            dbg_default_trace("subgroup={}.{},layout={}.", tid, sidx, persubgroup.dump());
            sidx++;
        }
        tid++;
    }
}
#endif  //NDEBUG

int main(int argc, char** argv) {
    // set proc name
    if(prctl(PR_SET_NAME, PROC_NAME, 0, 0, 0) != 0) {
        dbg_default_warn("Cannot set proc name to {}.", PROC_NAME);
    }
    dbg_default_trace("set proc name to {}", PROC_NAME);
    // load configuration
    auto group_layout = json::parse(getConfString(CONF_GROUP_LAYOUT));
#ifndef NDEBUG
    dbg_default_trace("load layout:");
    dump_layout(group_layout);
#endif  //NDEBUG
    auto wpcsu_factory = [](persistent::PersistentRegistry* pr, subgroup_id_t) {
        return std::make_unique<WPCSU>(pr, nullptr);
    };
    auto wpcss_factory = [](persistent::PersistentRegistry* pr, subgroup_id_t) {
        return std::make_unique<WPCSS>(pr, nullptr);
    };
    dbg_default_trace("starting service...");
    Service<WPCSU, WPCSS>::start(group_layout, {}, wpcsu_factory, wpcss_factory);
    dbg_default_trace("started service, waiting till it ends.");
    std::cout << "Press Enter to Shutdown." << std::endl;
    std::cin.get();
    // wait for service to quit.
    Service<WPCSU, WPCSS>::shutdown(false);
    dbg_default_trace("shutdown service gracefully");
    // you can do something here to parallel the destructing process.
    Service<WPCSU, WPCSS>::wait();
    dbg_default_trace("Finish shutdown.");
    return 0;
}
