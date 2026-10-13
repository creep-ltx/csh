#!csh
# ls on the real ROM (output to a file: one name a line, no colours)
MakeDir RAM:lst RAM:lst/dir
echo a > RAM:lst/file10.txt
echo bb > RAM:lst/file9.txt
echo ccc > RAM:lst/Zebra.lha
echo x > RAM:lst/pic.info
echo y > RAM:lst/tool
echo "-- plain"; RAM:ls RAM:lst
echo "-- F"; RAM:ls -F RAM:lst
echo "-- a"; RAM:ls -a RAM:lst
echo "-- S"; RAM:ls -S RAM:lst
echo "-- r"; RAM:ls -r RAM:lst
echo "-- #?"; RAM:ls RAM:lst/#?.txt
echo "-- *"; RAM:ls RAM:lst/*.txt
echo "-- file"; RAM:ls RAM:lst/file9.txt
echo "-- d"; RAM:ls -d RAM:lst/dir
RAM:ls RAM:nothere
echo "rc $?"
RAM:ls RAM:lst/#?.none
echo "rc $?"
echo "-- R"; RAM:ls -R RAM:lst/dir
Delete RAM:lst ALL QUIET
