# Each test is a separate executable, so a crash or sanitizer abort in one
# cannot mask the results of another.

# header-only for now
add_library(ota_core INTERFACE)
target_include_directories(ota_core INTERFACE ${CMAKE_SOURCE_DIR}/common/include)

function(add_host_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE ota_core)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# add_host_test(...) entries land here as tests are written