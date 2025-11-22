#!/bin/bash

NUM_CLIENTS=100

echo "Starting $NUM_CLIENTS bot clients"

for i in $(seq 1 $NUM_CLIENTS)
do
    ./load_bot "bot_$i" & 
done

echo "$NUM_CLIENTS bots are now spamming. Press Ctrl+C to stop them."
echo "Use 'killall load_bot' to clean up."

trap "echo; echo 'Stopping bots...'; killall load_bot; exit 0" SIGINT
wait