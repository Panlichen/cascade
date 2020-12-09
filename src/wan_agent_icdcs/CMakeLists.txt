cmake_minimum_required(VERSION 3.10.1)
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)


add_executable(perf_with_wan_icdcs perf_with_wan_icdcs.cpp)
target_include_directories(perf_with_wan_icdcs PRIVATE
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
# cascade has linked lib wan_agent already, so maybe we do not link again here.
target_link_libraries(perf_with_wan_icdcs cascade predicate_generator)
add_custom_command(TARGET perf_with_wan_icdcs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/icdcs_cfg
    ${CMAKE_CURRENT_BINARY_DIR}/icdcs_cfg
)
add_executable(cascade_wan_sender_icdcs cascade_wan_sender.cpp)
target_include_directories(cascade_wan_sender PRIVATE
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(cascade_wan_sender_icdcs cascade dl predicate_generator)