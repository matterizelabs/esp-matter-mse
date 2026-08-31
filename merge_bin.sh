#!/bin/bash

esptool.py --chip esp32 merge_bin --flash_mode dio --flash_size 4MB --flash_freq 40m -o anime-merged_.bin 0x1000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x15000 build/ota_data_initial.bin 0x18000 tools/certs/out/1618_1-partition.bin 0x20000 build/matter_mse.bin