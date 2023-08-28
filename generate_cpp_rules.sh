#!/bin/bash
set -eu

if [ $# -ne 1 ]; then
    echo "Usage: $0 <path to rules dir>"
    exit 1
fi

if [ ! -d $1 ]; then
    echo "Error: $1 is not a directory"
    echo "Usage: $0 <path to rules dir>"
    exit 1
fi

for file in "$1"/*.conf "$1"/*.data; do
    if [[ -f "$file" ]]; then
        sed -i '/^#/d' $file
        sed -i '/^$/N;/^\n$/D' $file
        sed -i '/./,$!d' $file
    fi
done
