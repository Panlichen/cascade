for path in `find /root/lcpan/cascade/build/src/wan_agent_icdcs/icdcs_cfg_local_test -regex ".*\.plog"`
do
    rm -rf $path
done

for path in `find /root/lcpan/cascade/build/src/wan_agent_icdcs/icdcs_cfg_local_test -regex ".*\.log"`
do
    rm -rf $path
done