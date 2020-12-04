#include <cascade/cascade.hpp>
#include <cascade/object.hpp>
#include <cascade/service.hpp>
#include <cascade/service_types.hpp>
#include <derecho/conf/conf.hpp>
#include <derecho/utils/logger.hpp>
#include <dlfcn.h>
#include <iostream>
#include <map>
#include <sys/prctl.h>
#include <typeindex>
#include <typeinfo>
#include <vector>
#include <wan_agent/wan_agent.hpp>

#define PROC_NAME "wan_cascade_no_svc_sender"

using namespace derecho::cascade;
using namespace derecho;

int main(int argc, char** argv) {
    auto group_layout = json::parse(getConfString(CONF_GROUP_LAYOUT));
    auto wpcsu_factory = [](persistent::PersistentRegistry* pr, subgroup_id_t) {
        return std::make_unique<WPCSU>(pr, nullptr);
    };
    auto wpcss_factory = [](persistent::PersistentRegistry* pr, subgroup_id_t) {
        return std::make_unique<WPCSS>(pr, nullptr);
    };

    SubgroupInfo si = generate_subgroup_info<WPCSU, WPCSS>(group_layout);

    Group<WPCSU, WPCSS> group(
            CallbackSet{},
            si,
            std::vector<DeserializationContext*>{},
            std::vector<view_upcall_t>{
                    [&group](const View& view) {
                        node_id_t my_id = getConfUInt32(CONF_DERECHO_LOCAL_ID);
                        const std::map<subgroup_id_t, SubgroupSettings>& subgroup_settings_map = view.multicast_group->get_subgroup_settings();

                        // Iterate every shard I am a member of.
                        auto m_iter = view.my_subgroups.begin();
                        while(m_iter != view.my_subgroups.end()) {
                            subgroup_id_t my_subgroup_id = m_iter->first;
                            uint32_t shard_num = m_iter->second;
                            auto subgroup_settings = subgroup_settings_map.at(my_subgroup_id);

                            subgroup_type_id_t my_type_id = static_cast<subgroup_type_id_t>(-1);
                            uint32_t my_subgroup_index = static_cast<uint32_t>(-1);

                            auto mm_iter = view.subgroup_ids_by_type_id.begin();
                            while(mm_iter != view.subgroup_ids_by_type_id.end()) {
                                for(uint32_t i = 0; i < mm_iter->second.size(); i++) {
                                    subgroup_type_id_t temp_subgroup_id = mm_iter->second.at(i);
                                    if(temp_subgroup_id == my_subgroup_id) {
                                        my_type_id = mm_iter->first;
                                        my_subgroup_index = i;
                                        break;
                                    }
                                }
                                if(my_type_id != static_cast<subgroup_type_id_t>(-1)) {
                                    break;
                                }
                                mm_iter++;
                            }

                            // TODO: use the node with lowest shard_rank or sender_rank to call Replicated<T>.ordered_send to avoid redundant broadcast.
                            dbg_default_info("I am in subgroup({}), shard({}), subgroup_settings says my shard_num is {}, shard_rank is {}, sender_rank is {}", my_subgroup_id, shard_num, subgroup_settings.shard_num, subgroup_settings.shard_rank, subgroup_settings.sender_rank);

                            /* NOTE: parameter for get_subgroup is "The index of the subgroup within the set of subgroups that replicate the same type of object", not the global subgroup_id */
                            /* CANNOT use auto, must point out SubgroupType */
                            if(my_type_id == 0) {
                                Replicated<WPCSU>& subgroup_handle = group.get_subgroup<WPCSU>(my_subgroup_index);
                                dbg_default_info("my_type_id is {}, my_subgroup_index is {}, subgroup_handle says subgroup_id is {}, shard_num is {}", my_type_id, my_subgroup_index, subgroup_handle.get_subgroup_id(), subgroup_handle.get_shard_num());

                                if(subgroup_settings.shard_rank == 0) {
                                    subgroup_handle.ordered_send<RPC_NAME(set_wan_sender_info)>(my_id);
                                    dbg_default_info("I inform other nodes in my shard the wan_sender is me({})", my_id);
                                }
                            } else if(my_type_id == 1) {
                                Replicated<WPCSS>& subgroup_handle = group.get_subgroup<WPCSS>(my_subgroup_index);
                                dbg_default_info("my_type_id is {}, my_subgroup_index is {}, subgroup_handle says subgroup_id is {}, shard_num is {}", my_type_id, my_subgroup_index, subgroup_handle.get_subgroup_id(), subgroup_handle.get_shard_num());

                                if(subgroup_settings.shard_rank == 0) {
                                    subgroup_handle.ordered_send<RPC_NAME(set_wan_sender_info)>(my_id);
                                    dbg_default_info("I inform other nodes in my shard the wan_sender is me({})", my_id);
                                }
                            }
                            m_iter++;
                        }
                    }},
            wpcsu_factory, wpcss_factory);

    std::cout << "Cascade Server finished constructing Derecho group." << std::endl;
    std::cout << "Press ENTER to shutdown..." << std::endl;
    std::cin.get();
    group.barrier_sync();
    group.leave();
    dbg_default_info("Cascade server shutdown.");
}