# btclassic's BTstack backend, for MicroPython ports that build BTstack
# (rp2 boards with a CYW43 radio). Include it from a user C module's
# micropython.cmake; it defines the INTERFACE library usermod_btclassic_btstack.
#
# The port compiles BTstack's core for the `bluetooth` (BLE) module. This adds
# BTstack's Classic sources and SBC codec, and points the port at a BTstack
# config with ENABLE_CLASSIC (the port reads MICROPY_BLUETOOTH_BTSTACK_CONFIG_FILE
# after user modules are included).

if(MICROPY_BLUETOOTH_BTSTACK AND NOT TARGET usermod_btclassic_btstack)
    set(BTCLASSIC_BTSTACK_DIR ${CMAKE_CURRENT_LIST_DIR})
    set(BTCLASSIC_BTSTACK_LIB ${MICROPY_DIR}/lib/btstack)

    set(MICROPY_BLUETOOTH_BTSTACK_CONFIG_FILE "${BTCLASSIC_BTSTACK_DIR}/btclassic_btstack_config.h")

    add_library(usermod_btclassic_btstack INTERFACE)

    target_include_directories(usermod_btclassic_btstack INTERFACE
        ${BTCLASSIC_BTSTACK_DIR}
        ${BTCLASSIC_BTSTACK_DIR}/../esp-idf/btclassic/include
        ${BTCLASSIC_BTSTACK_DIR}/../esp-idf/btclassic
        ${BTCLASSIC_BTSTACK_LIB}/src
        ${BTCLASSIC_BTSTACK_LIB}/platform/embedded
        ${BTCLASSIC_BTSTACK_LIB}/3rd-party/bluedroid/decoder/include
        ${BTCLASSIC_BTSTACK_LIB}/3rd-party/bluedroid/encoder/include
    )

    set(_c ${BTCLASSIC_BTSTACK_LIB}/src/classic)
    set(_dec ${BTCLASSIC_BTSTACK_LIB}/3rd-party/bluedroid/decoder/srce)
    set(_enc ${BTCLASSIC_BTSTACK_LIB}/3rd-party/bluedroid/encoder/srce)
    target_sources(usermod_btclassic_btstack INTERFACE
        ${BTCLASSIC_BTSTACK_DIR}/radio.c
        ${BTCLASSIC_BTSTACK_DIR}/av.c
        ${BTCLASSIC_BTSTACK_DIR}/sink.c
        ${BTCLASSIC_BTSTACK_DIR}/source.c
        ${BTCLASSIC_BTSTACK_DIR}/hf.c
        ${BTCLASSIC_BTSTACK_DIR}/spp.c
        ${BTCLASSIC_BTSTACK_DIR}/hid.c
        ${BTCLASSIC_BTSTACK_DIR}/../esp-idf/btclassic/ring.c
        ${BTCLASSIC_BTSTACK_DIR}/mp_host.c

        ${BTCLASSIC_BTSTACK_LIB}/src/hci_event.c
        ${BTCLASSIC_BTSTACK_LIB}/src/btstack_linked_queue.c
        ${BTCLASSIC_BTSTACK_LIB}/src/btstack_hid.c

        ${_c}/a2dp.c
        ${_c}/a2dp_sink.c
        ${_c}/a2dp_source.c
        ${_c}/avdtp.c
        ${_c}/avdtp_acceptor.c
        ${_c}/avdtp_initiator.c
        ${_c}/avdtp_sink.c
        ${_c}/avdtp_source.c
        ${_c}/avdtp_util.c
        ${_c}/avrcp.c
        ${_c}/avrcp_browsing.c
        ${_c}/avrcp_browsing_controller.c
        ${_c}/avrcp_controller.c
        ${_c}/avrcp_cover_art_client.c
        ${_c}/avrcp_target.c
        ${_c}/btstack_cvsd_plc.c
        ${_c}/btstack_link_key_db_memory.c
        ${_c}/btstack_sbc_decoder_bluedroid.c
        ${_c}/btstack_sbc_encoder_bluedroid.c
        ${_c}/btstack_sbc_plc.c
        ${_c}/device_id_server.c
        ${_c}/goep_client.c
        ${_c}/hfp.c
        ${_c}/hfp_codec.c
        ${_c}/hfp_hf.c
        ${_c}/hfp_msbc.c
        ${_c}/hid_device.c
        ${_c}/hid_host.c
        ${_c}/obex_iterator.c
        ${_c}/obex_message_builder.c
        ${_c}/obex_parser.c
        ${_c}/rfcomm.c
        ${_c}/sdp_client.c
        ${_c}/sdp_client_rfcomm.c
        ${_c}/sdp_server.c
        ${_c}/sdp_util.c
        ${_c}/spp_server.c

        ${_dec}/alloc.c
        ${_dec}/bitalloc.c
        ${_dec}/bitalloc-sbc.c
        ${_dec}/bitstream-decode.c
        ${_dec}/decoder-oina.c
        ${_dec}/decoder-private.c
        ${_dec}/decoder-sbc.c
        ${_dec}/dequant.c
        ${_dec}/framing.c
        ${_dec}/framing-sbc.c
        ${_dec}/oi_codec_version.c
        ${_dec}/synthesis-sbc.c
        ${_dec}/synthesis-dct8.c
        ${_dec}/synthesis-8-generated.c

        ${_enc}/sbc_analysis.c
        ${_enc}/sbc_dct.c
        ${_enc}/sbc_dct_coeffs.c
        ${_enc}/sbc_enc_bit_alloc_mono.c
        ${_enc}/sbc_enc_bit_alloc_ste.c
        ${_enc}/sbc_enc_coeffs.c
        ${_enc}/sbc_encoder.c
        ${_enc}/sbc_packing.c
    )
endif()

