#!/bin/bash

cd /mnt/fast/doc/projects/botmanager

wc -l $(find . -type d \( -name "old" -o -name "tests" -o -name ".claude" \) -prune -o \( -name "*.c" -o -name "*.h" \) -print)

exit 0
