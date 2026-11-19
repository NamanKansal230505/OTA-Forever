# Replaces ESP-IDF's second-stage bootloader "main" component (same name = override).
# Everything except the boot-partition choice is the stock IDF bootloader.
idf_component_register(SRCS "bootloader_start.c"
                       REQUIRES bootloader bootloader_support esp_rom)

idf_build_get_property(target IDF_TARGET)
set(ld_dir "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}")

if(EXISTS "${ld_dir}/bootloader.ld")
    target_linker_script(${COMPONENT_LIB} INTERFACE "${ld_dir}/bootloader.ld")
else()
    # Newer ESP-IDF releases ship a template that has to be preprocessed
    target_linker_script(${COMPONENT_LIB} INTERFACE "${ld_dir}/bootloader.ld.in"
                         PROCESS "${CMAKE_CURRENT_BINARY_DIR}/ld/bootloader.ld")
endif()
target_linker_script(${COMPONENT_LIB} INTERFACE "${ld_dir}/bootloader.rom.ld")
