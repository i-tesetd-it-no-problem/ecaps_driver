#!/bin/sh

depmod

for module in ./*.ko; do
    if [ -f "$module" ]; then
        modprobe "$(basename "$module" .ko)"
        if [ $? -ne 0 ]; then
            echo "Failed to load module: $module" >&2
        fi
    else
        echo "No .ko files found in the current directory"
    fi
done

echo "All modules processed."