#!/bin/bash

########################################################
# Name: bbr_mininet.sh
# Function: 
#   Starts four tmux sessions for clients and servers, and send files over mininet 
#   dumbbell topology then calculates the average throughput. 
# How to use:
#   1. Make sure you have tmux. otherwise run "sudo apt-get install tmux"
#   2. In one terminal, go to lab3 folder and run "sudo python lab3_topology.py"
#   3. In another terminal, go to lab3 folder and copy "file.txt" here 
#   4. In the same folder, run the script with "sudo ./bbr_mininet.sh [551_HOME_PATH]",
#      [551_HOME_PATH] is your 551 repo folder's path. 
########################################################


HOME_FOLDER=${1%/}

SERVER1_PORT=9096

SERVER1_IP="12.0.1.1"

CLIENT1_PORT=9098

SRC_FILE1="file.txt"
RAND_SUFFIX=$RANDOM
DST_FILE1="dst1_$RAND_SUFFIX.txt" 

echo "551 home folder: $HOME_FOLDER"
cd $HOME_FOLDER/lab3

echo "Starting server1"
tmux new -s server1 -d 
tmux send -t server1 "sudo ./go_to.sh server1" ENTER
tmux send -t server1 "cd $HOME_FOLDER/lab3" ENTER 
tmux send -t server1 "sudo ./ctcp -m -s -w 40 -p $SERVER1_PORT > $DST_FILE1" ENTER

sleep 5 

echo "Starting client1"
tmux new -s client1 -d 
tmux send -t client1 "sudo ./go_to.sh client1" ENTER
tmux send -t client1 "cd $HOME_FOLDER/lab3" ENTER
tmux send -t client1 "sudo ./ctcp -m -c $SERVER1_IP:$SERVER1_PORT -p $CLIENT1_PORT < $SRC_FILE1" ENTER

START_TIME=$(($(date +%s)))
FILE_SIZE=$(($(stat --printf="%s" $SRC_FILE1)))
sleep 30

echo "Ending all nodes"
tmux kill-session -t client1
tmux kill-session -t server1

# Calculate throughput 
SEND_TIME1=$(($(stat -c %Y $DST_FILE1)-START_TIME)) 

echo "Dumbbell Throughput: $(($FILE_SIZE * 8 / ($SEND_TIME1))) bps"
