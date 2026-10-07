#!/bin/bash
cd /mnt/d/Coding_Project/CS374/Project || exit 1
pkill -f './room_server' 2>/dev/null || true
sleep 0.3
gcc -Wall -g -Iinclude -pthread -o client \
  src/ui/client_main.c src/ui/ui_state.c src/ui/ui_term.c \
  src/punch/udp_sock.c src/common/log.c src/common/fa_stream.c || exit 1
gcc -Wall -g -Iinclude -o room_server src/tracker/room_server.c src/common/log.c || exit 1
./room_server >logs/room_run.log 2>&1 &
RP=$!
sleep 1
( sleep 0.2; echo 'join detA localhost'; sleep 1.2; echo probe; sleep 1.2; echo quit ) | ./client >logs/client_run.log 2>&1
kill $RP 2>/dev/null
wait $RP 2>/dev/null
echo DONE
tail -20 logs/client.log
