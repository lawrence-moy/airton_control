#!/bin/sh
# Tests unitaires sur PC (sans ESP-IDF) : parseurs Tuya et 7A 7A.
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -Wall -Wextra -Imain test/test_parser.cpp main/tuya_mcu.cpp -o /tmp/airton_test_tuya
g++ -std=c++17 -Wall -Wextra -Imain test/test_acw02.cpp main/acw02.cpp -o /tmp/airton_test_acw02
/tmp/airton_test_tuya
/tmp/airton_test_acw02
