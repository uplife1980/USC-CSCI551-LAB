#!/bin/bash

########################################################
# Name: bbr_i2.sh
# Function: 
#   Starts four tmux sessions for clients and servers, and send files over I2 
#   then calculates the average throughput. 
# How to use:
#   1. Make sure you have tmux. otherwise run "sudo apt-get install tmux"
#   2. Go to lab2 folder and copy "file.txt" here 
#   3. In the same folder, run the script with "sudo ./bbr_i2.sh [551_HOME_PATH]",
#      [551_HOME_PATH] is your 551 repo folder's path, should be FULL path 
########################################################


HOME_FOLDER=${1%/}
echo "551 home folder: $HOME_FOLDER"

SERVER1_PORT=19997
SERVER2_PORT=19996

SERVER1_IP="6.0.2.2"
SERVER2_IP="6.0.3.2"

CLIENT1_PORT=8887
CLIENT2_PORT=8886

SRC_FILE1="file.txt"
SRC_FILE2="file.txt"
RAND_SUFFIX=$RANDOM
DST_FILE1="dst1_$RAND_SUFFIX.txt" 
DST_FILE2="dst2_$RAND_SUFFIX.txt"

echo "Starting I2 topology"
killall ovs-controller
killall screen
killall ctcp
mn -c
sh config.sh
tmux new -s i2 -d 
tmux send -t i2 "sudo python multiAS.py" ENTER
sleep 20

echo "Copy ctcp and sr to lab2 folder"
cp ../lab3/ctcp .
cp ../lab1/sr .

echo "Configuring I2..."
python load_configs_multiAS.py configs_multiAS
python config_i2_hosts.py
python ../tester/converged.py multiAS

echo "Starting pox and sr"
screen -S pox -d -m ~/pox/pox.py cs144.ofhandler cs144.srhandler
expect pox_expect
sleep 5
screen -S sr -d -m ./sr

