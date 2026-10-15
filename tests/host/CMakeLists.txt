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
    ${CMAKE_SOURCE_DIR}/common/progress.c
    ${CMAKE_SOURCE_DIR}/common/update_agent.c
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
add_host_test(test_progress)

# ---- signed test images ----
#
# A throwaway key is generated into the build directory, with every image the
# tests use, so no key material is involved from outside it. All of it comes
# from one command: test_pubkey is the only target that owns it, so a
# parallel build cannot run it twice.
#
#   images/         imgtool.py gen-bad: one good image and one per way of
#                   being bad. The payload only has to be long enough to
#                   tamper with.
#   update_images/  for the update agent, all for slot B: an old image
#                   already in the slot, the update, and a second, different
#                   update. The update spans two full pages and part of a
#                   third, so a transfer crosses page boundaries and ends
#                   partway through one.
set(TEST_KEY_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_key)
set(TEST_IMAGE_DIR ${CMAKE_CURRENT_BINARY_DIR}/images)
set(TEST_PAYLOAD ${CMAKE_CURRENT_BINARY_DIR}/payload.bin)
set(TEST_PUBKEY_C ${CMAKE_CURRENT_BINARY_DIR}/test_pubkey.c)
set(UPDATE_IMAGE_DIR ${CMAKE_CURRENT_BINARY_DIR}/update_images)
set(UPDATE_PAYLOAD ${CMAKE_CURRENT_BINARY_DIR}/update_payload.bin)
set(OTHER_PAYLOAD ${CMAKE_CURRENT_BINARY_DIR}/other_payload.bin)

string(REPEAT "test payload for image verification\n" 32 payload_text)
file(WRITE ${TEST_PAYLOAD} "${payload_text}")
string(REPEAT "update agent test payload, 32 b\n" 138 update_payload_text)
file(WRITE ${UPDATE_PAYLOAD} "${update_payload_text}")
string(REPEAT "second update payload, 32 bytes\n" 100 other_payload_text)
file(WRITE ${OTHER_PAYLOAD} "${other_payload_text}")

set(SIGN_FOR_B ${IMGTOOL_PYTHON} ${IMGTOOL} sign --key ${TEST_KEY_DIR}/priv.pem
    --load-addr 0x08040000)

add_custom_command(
    OUTPUT ${TEST_IMAGE_DIR}/EXPECTATIONS.txt ${TEST_PUBKEY_C}
        ${UPDATE_IMAGE_DIR}/old.img ${UPDATE_IMAGE_DIR}/new.img
        ${UPDATE_IMAGE_DIR}/other.img
    COMMAND ${IMGTOOL_PYTHON} ${IMGTOOL} keygen --force
        --out-priv ${TEST_KEY_DIR}/priv.pem --out-pub ${TEST_KEY_DIR}/pub.pem
    COMMAND ${IMGTOOL_PYTHON} ${IMGTOOL} pubkey-c
        --key ${TEST_KEY_DIR}/pub.pem --out ${TEST_PUBKEY_C}
    COMMAND ${IMGTOOL_PYTHON} ${IMGTOOL} gen-bad
        --key ${TEST_KEY_DIR}/priv.pem --in ${TEST_PAYLOAD} --outdir ${TEST_IMAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${UPDATE_IMAGE_DIR}
    COMMAND ${SIGN_FOR_B} --in ${UPDATE_PAYLOAD} --out ${UPDATE_IMAGE_DIR}/old.img
        --version 1.0.0 --security-counter 3
    COMMAND ${SIGN_FOR_B} --in ${UPDATE_PAYLOAD} --out ${UPDATE_IMAGE_DIR}/new.img
        --version 1.1.0 --security-counter 5
    COMMAND ${SIGN_FOR_B} --in ${OTHER_PAYLOAD} --out ${UPDATE_IMAGE_DIR}/other.img
        --version 1.2.0 --security-counter 5
    DEPENDS ${IMGTOOL} ${TEST_PAYLOAD} ${UPDATE_PAYLOAD} ${OTHER_PAYLOAD}
    COMMENT "Generating throwaway key and test images"
)
add_library(test_pubkey STATIC ${TEST_PUBKEY_C})

# ---- image verification against imgtool.py ----
add_executable(test_image_verify test_image_verify.c)
target_link_libraries(test_image_verify PRIVATE ota_core test_pubkey)
target_compile_definitions(test_image_verify PRIVATE
    TEST_IMAGE_DIR="${TEST_IMAGE_DIR}"
)
add_test(NAME test_image_verify COMMAND test_image_verify)

# ---- the update agent ----
#
# The agent works on the real flash map, so its simulator covers all 512 KB
add_library(sim_flash_full STATIC sim_flash.c)
target_include_directories(sim_flash_full PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sim_flash_full PUBLIC SIM_PAGES=256u)
target_link_libraries(sim_flash_full PUBLIC ota_core)

add_executable(test_update test_update.c)
target_link_libraries(test_update PRIVATE ota_core sim_flash_full test_pubkey)
target_compile_definitions(test_update PRIVATE
    UPDATE_IMAGE_DIR="${UPDATE_IMAGE_DIR}"
    TEST_IMAGE_DIR="${TEST_IMAGE_DIR}"
)
add_test(NAME test_update COMMAND test_update)
