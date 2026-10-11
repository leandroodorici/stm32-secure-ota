# Each test is a separate executable, so a crash or sanitizer abort in one
# cannot mask the results of another.

include(${CMAKE_SOURCE_DIR}/cmake/boot_crypto.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/imgtool.cmake)

# Everything under common/, built exactly as the bootloader builds it
add_library(ota_core STATIC
    ${CMAKE_SOURCE_DIR}/common/boot_record.c
    ${CMAKE_SOURCE_DIR}/common/image_verify.c
    ${CMAKE_SOURCE_DIR}/common/frame.c
    ${CMAKE_SOURCE_DIR}/common/protocol.c
)
target_include_directories(ota_core PUBLIC ${CMAKE_SOURCE_DIR}/common/include)
target_link_libraries(ota_core PUBLIC boot_crypto)
# Pointers and allocator headers are twice the size on a 64-bit host, so the
# same verification needs a larger buffer than on the G474
target_compile_definitions(ota_core PRIVATE IMAGE_VERIFY_HEAP_SIZE=12288u)

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
add_host_test(test_frame)
add_host_test(test_protocol)

# ---- image verification against imgtool.py ----
#
# A throwaway key and the full gen-bad image set are generated into the build
# directory, so the test needs no key material from outside it. The payload
# content is irrelevant; it only has to be long enough for gen-bad to tamper
# with.
set(TEST_KEY_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_key)
set(TEST_IMAGE_DIR ${CMAKE_CURRENT_BINARY_DIR}/images)
set(TEST_PAYLOAD ${CMAKE_CURRENT_BINARY_DIR}/payload.bin)
set(TEST_PUBKEY_C ${CMAKE_CURRENT_BINARY_DIR}/test_pubkey.c)

string(REPEAT "test payload for image verification\n" 32 payload_text)
file(WRITE ${TEST_PAYLOAD} "${payload_text}")

add_custom_command(
    OUTPUT ${TEST_IMAGE_DIR}/EXPECTATIONS.txt ${TEST_PUBKEY_C}
    COMMAND ${IMGTOOL_PYTHON} ${IMGTOOL} keygen --force
        --out-priv ${TEST_KEY_DIR}/priv.pem --out-pub ${TEST_KEY_DIR}/pub.pem
    COMMAND ${IMGTOOL_PYTHON} ${IMGTOOL} pubkey-c
        --key ${TEST_KEY_DIR}/pub.pem --out ${TEST_PUBKEY_C}
    COMMAND ${IMGTOOL_PYTHON} ${IMGTOOL} gen-bad
        --key ${TEST_KEY_DIR}/priv.pem --in ${TEST_PAYLOAD} --outdir ${TEST_IMAGE_DIR}
    DEPENDS ${IMGTOOL} ${TEST_PAYLOAD}
    COMMENT "Generating throwaway key and test image set"
)

add_executable(test_image_verify test_image_verify.c ${TEST_PUBKEY_C})
target_link_libraries(test_image_verify PRIVATE ota_core)
target_compile_definitions(test_image_verify PRIVATE
    TEST_IMAGE_DIR="${TEST_IMAGE_DIR}"
)
add_test(NAME test_image_verify COMMAND test_image_verify)
