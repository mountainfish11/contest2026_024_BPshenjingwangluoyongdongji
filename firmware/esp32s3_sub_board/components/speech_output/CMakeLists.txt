idf_component_register(
    SRCS "speech_output.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition espressif__esp-sr espressif__usb_host_uac usb
)

# Keep the TTS voice corpus outside the application image. idf.py flash
# therefore writes both the firmware and the matching voice_data partition.
idf_component_get_property(esp_sr_dir espressif__esp-sr COMPONENT_DIR)
set(voice_data_file
    "${esp_sr_dir}/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaoxin_small.dat")

if(EXISTS "${voice_data_file}")
    add_dependencies(flash ${COMPONENT_LIB})
    esptool_py_flash_to_partition(flash "voice_data" "${voice_data_file}")
else()
    message(FATAL_ERROR "ESP-TTS voice data not found: ${voice_data_file}")
endif()
