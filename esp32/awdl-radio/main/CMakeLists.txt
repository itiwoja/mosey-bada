set(OWL ${CMAKE_CURRENT_LIST_DIR}/../../../third_party/owl)

idf_component_register(
    SRCS
        main.c
        ${OWL}/src/channel.c
        ${OWL}/src/circular_buffer.c
        ${OWL}/src/crc32.c
        ${OWL}/src/election.c
        ${OWL}/src/frame.c
        ${OWL}/src/hashmap.c
        ${OWL}/src/peers.c
        ${OWL}/src/rx.c
        ${OWL}/src/schedule.c
        ${OWL}/src/siphash24.c
        ${OWL}/src/state.c
        ${OWL}/src/sync.c
        ${OWL}/src/tx.c
        ${OWL}/src/version.c
        ${OWL}/src/wire.c
        ${OWL}/radiotap/radiotap.c
    INCLUDE_DIRS compat . ${OWL}/src ${OWL}/radiotap
    PRIV_REQUIRES esp_wifi esp_timer esp_driver_uart nvs_flash esp_event
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE AWDL_FIXED_RADIO_CHANNEL=6)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-format -Wno-unused-variable)
