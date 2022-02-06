#!/usr/bin/python
import sys
import subprocess


routerList = ["SEAT","LOSA", "SALT", "HOUS", "KANS", "ATLA", "WASH", "CHIC", "NEWY"]
ipAddr = ["4.109.0.1","4.108.0.1","4.107.0.1","4.106.0.1","4.105.0.1","4.104.0.1","4.103.0.1","4.102.0.1","4.101.0.1"]

if __name__ == '__main__':
    for index,router in enumerate(routerList):
        host = router + "-host"
        lowercase_router = router.lower()
        peerIP = ipAddr[index][:-1] + '2'

        configIP = "sudo ifconfig %s %s/24 up" %(lowercase_router, ipAddr[index])
        configDefaultIP = "sudo route add default gw %s %s" %(peerIP, lowercase_router)
        getPid = "pid=`eval ps ax | grep \"mininet:%s\" | grep bash | grep -v mxexec | awk '{print $1};'`" %(host)
        enterShellPrefix = "sudo mxexec -a $pid -b $pid -k $pid " 
    
        #print (configDefaultIP)
        subprocess.call("%s && %s " %(getPid, enterShellPrefix+configIP) , shell=True)
        subprocess.call("%s && %s " %(getPid, enterShellPrefix+configDefaultIP) , shell=True)
