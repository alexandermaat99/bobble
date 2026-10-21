#!/bin/zsh
# Links the firmware's own source (main/, and the libraries it uses) into the emulator's C target, so the emulator
# always builds whatever's in main/. wifi_setup.c is the one file swapped out (port/sim_wifi.c does its job).
set -e
cd "${0:A:h}"
fw=Sources/Firmware/fw
rm -rf $fw; mkdir -p $fw
for f in ../main/*.c ../main/*.h; do
    [[ ${f:t} == wifi_setup.c ]] && continue
    name=${f:t}
    [[ $name == main.c ]] && name=firmware_main.c   # (SwiftPM takes a target with a main.c for a program)
    ln -s ../../../$f $fw/$name
done
mc=../managed_components
for f in $mc/espressif__esp_jpeg/jpeg_decoder.c $mc/espressif__esp_jpeg/jpeg_default_huffman_table.c \
         $mc/espressif__esp_jpeg/tjpgd/tjpgd.c $mc/espressif__esp_jpeg/tjpgd/tjpgd.h $mc/espressif__esp_jpeg/tjpgd/tjpgdcnf.h \
         $mc/espressif__esp_jpeg/include/jpeg_decoder.h $mc/espressif__esp_jpeg/jpeg_default_huffman_table.h \
         $mc/espressif__qrcode/qrcodegen.c $mc/espressif__qrcode/qrcodegen.h $mc/espressif__qrcode/esp_qrcode_main.c \
         $mc/espressif__qrcode/esp_qrcode_wrapper.c $mc/espressif__qrcode/include/qrcode.h; do
    [[ -e $f ]] && ln -s ../../../$f $fw/${f:t}
done
idf=${IDF_PATH:-$HOME/esp/esp-idf}
ln -s $idf/components/json/cJSON/cJSON.c $fw/cJSON.c
ln -s $idf/components/json/cJSON/cJSON.h $fw/cJSON.h
