#!/bin/bash
cd /mnt/fast/doc/projects
rm -f botmanager.tar.zst &>/dev/null
tar -cvpf - --exclude botmanager/btdata botmanager | zstd -cz -T16 -19 -o botmanager.tar.zst
