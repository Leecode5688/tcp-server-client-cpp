#!/bin/bash

NUM_CLIENTS=1000
NORMAL_INTERVAL_MS=2000
SPAM_INTERVAL=100

echo "Starting $NUM_CLIENTS bot clients"

for i in $(seq 1 $NUM_CLIENTS)
do
    if((i%10==0)); then
        echo "Launching SPAM BOT: bot_$i"
        ./load_bot "bot_$i" $SPAM_INTERVAL &
    else
        echo "Launching NORMAL BOT: bot_$i"
        ./load_bot "bot_$i" $NORMAL_INTERVAL_MS &
    fi
done

echo "Bots running. Spammers will likely get disconnected by the server."
echo "Press Ctrl+C to stop all bots."

trap "echo; echo 'Stopping bots...'; killall load_bot; exit 0" SIGINT
wait