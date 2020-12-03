#include <cascade/cascade.hpp>
#include <cascade/object.hpp>
#include <cascade/service.hpp>
#include <cascade/service_types.hpp>
#include <derecho/conf/conf.hpp>
#include <derecho/utils/logger.hpp>
#include <dlfcn.h>
#include <sys/prctl.h>
#include <vector>
#include <wan_agent/wan_agent.hpp>

#define PROC_NAME "wan_cascade_no_svc_sender"

using namespace derecho::cascade;

int main(int argc, char** argv) {
    auto group_layout = json::parse(derecho::getConfString(CONF_GROUP_LAYOUT));
    auto wpcsu_factory = [](persistent::PersistentRegistry* pr, derecho::subgroup_id_t) {
        return std::make_unique<WPCSU>(pr, nullptr);
    };
    auto wpcss_factory = [](persistent::PersistentRegistry* pr, derecho::subgroup_id_t) {
        return std::make_unique<WPCSS>(pr, nullptr);
    };

    derecho::SubgroupInfo si = generate_subgroup_info<WPCSU, WPCSS>(group_layout);

    derecho::Group<WPCSU, WPCSS> group(
            derecho::CallbackSet{},
            si,
            std::vector<derecho::DeserializationContext*>{},
            std::vector<derecho::view_upcall_t>{
                    [&group](const derecho::View&) {
                        dbg_default_info("in view upcall, # members: {}", group.get_members().size());
                    }},
            wpcsu_factory, wpcss_factory);

    std::cout << "Cascade Server finished constructing Derecho group." << std::endl;
    std::cout << "Press ENTER to shutdown..." << std::endl;
    std::cin.get();
    group.barrier_sync();
    group.leave();
    dbg_default_info("Cascade server shutdown.");
}