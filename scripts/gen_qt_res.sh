#!/bin/bash
set -e

echo 'generating resource.qrc...'

script_dir=$(cd "$(dirname "$0")" && pwd)

input_dir="$1"
output_dir="$2"

mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd)

cd "$input_dir"
python "$script_dir/gen_qrc.py" . resource.qrc

echo 'generating qrc_qml.h...'

rcc -name qml resource.qrc -o qrc_qml.cpp

Temp=$(sed -n '$=' qrc_qml.cpp)
sed $(($Temp-58+1)),${Temp}d -i qrc_qml.cpp

mv qrc_qml.cpp "$output_dir/qrc_qml.h"

echo 'done.'
