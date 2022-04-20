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

echo "Starting server1"
tmux new -s server1 -d 
tmux send -t server1 "sudo ./go_to.sh server1" ENTER
tmux send -t server1 "cd $HOME_FOLDER/lab2" ENTER 
tmux send -t server1 "sudo ./ctcp -m -s -w 40 -p $SERVER1_PORT > server1_output.txt" ENTER

echo "Starting server2"
tmux new -s server2 -d 
tmux send -t server2 "sudo ./go_to.sh server2" ENTER
tmux send -t server2 "cd $HOME_FOLDER/lab2" ENTER 
tmux send -t server2 "sudo ./ctcp -m -s -w 40 -p $SERVER2_PORT > server2_output.txt" ENTER

sleep 10

# echo "Starting client1"
# tmux new -s client1 -d 
# tmux send -t client1 "sudo ./go_to.sh client" ENTER
# tmux send -t client1 "cd $HOME_FOLDER/lab2" ENTER
# tmux send -t client1 "sudo ./ctcp -m -c $SERVER1_IP:$SERVER1_PORT -p $CLIENT1_PORT -l < $SRC_FILE1" ENTER
# sleep 15
# echo "Starting client2"
# tmux new -s client2 -d 
# tmux send -t client2 "sudo ./go_to.sh client" ENTER
# tmux send -t client2 "cd $HOME_FOLDER/lab2" ENTER
# tmux send -t client2 "sudo ./ctcp -m -c $SERVER2_IP:$SERVER2_PORT -p $CLIENT2_PORT -l < $SRC_FILE2" ENTER

# START_TIME=$(($(date +%s)))
# FILE_SIZE=$(($(stat --printf="%s" $SRC_FILE1)))
# sleep 30

# echo "Ending all nodes"
# tmux kill-session -t client1
# tmux kill-session -t client2
# tmux kill-session -t server1
# tmux kill-session -t server2