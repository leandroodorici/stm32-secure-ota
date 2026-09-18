# Each test is a separate executable, so a crash or sanitizer abort in one
# cannot mask the results of another.

# header-only for now
add_library(ota_core STATIC ${CMAKE_SOURCE_DIR}/common/boot_record.c)
target_include_directories(ota_core PUBLIC ${CMAKE_SOURCE_DIR}/common/include)

# Flash simulator
add_library(sim_flash STATIC sim_flash.c)
target_include_directories(sim_flash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sim_flash PUBLIC ota_core)

function(add_host_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE ota_core sim_flash)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# add_host_test(...) entries land here as tests are written
add_host_test(test_sim_flash)
add_host_test(test_boot_record)
add_host_test(test_powerfail)
