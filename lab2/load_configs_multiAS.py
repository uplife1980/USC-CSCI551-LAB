#!/usr/bin/python

import subprocess
import sys

hostList = ["client", "server1", "server2"]
hostInterface = ["client-eth0", "east", "east"]
ipAddr = ["5.1.1.2", "6.0.2.2", "6.0.3.2"]
routerList = ["SEAT","LOSA", "SALT", "HOUS", "KANS", "ATLA", "WASH", "CHIC", "NEWY", "east", "west"]

if __name__ == '__main__':
    
    #config end host
    for index,host in enumerate(hostList):
        peerIP = ipAddr[index][:-1] + '1'

        configIP = "sudo ifconfig %s %s/24 up" %(hostInterface[index], ipAddr[index])
        configDefaultIP = "sudo route add default gw %s %s" %(peerIP, hostInterface[index])
        getPid = "pid=`eval ps ax | grep \"mininet:%s\" | grep bash | grep -v mxexec | awk '{print $1};'`" %(host)
        enterShellPrefix = "sudo mxexec -a $pid -b $pid -k $pid " 
    
        #print (configDefaultIP)
        subprocess.call("%s && %s " %(getPid, enterShellPrefix+configIP) , shell=True)
        subprocess.call("%s && %s " %(getPid, enterShellPrefix+configDefaultIP) , shell=True)

    #config routers
    folder = sys.argv[1]
    for router in routerList:
        appendStr = ""
        getPid = "pid=`eval ps ax | grep \"mininet:%s\" | grep bash | grep -v mxexec |grep -v host| awk '{print $1};'`" %(router)
        enterShellPrefix = "sudo mxexec -a $pid -b $pid -k $pid vtysh -c \'conf ter\' "

        ipConfigFile = open("./"+folder+"/"+router+"/zebra.conf.sav")
        ospfConfigFile = open("./"+folder+"/"+router+"/ospfd.conf.sav")
        bgpConfigFile = open("./"+folder+"/"+router+"/bgpd.conf.sav")

        thisLine = "0"

        while(len(thisLine) != 0):
            while( len(thisLine) != 0  and (("interface" not in thisLine) and ("ip route" not in thisLine))):
                thisLine = ipConfigFile.readline() 
            if("ip route" in thisLine):
                while("!" not in thisLine):
                    appendStr += "-c \'"+thisLine[:-1]+"\' "
                    thisLine = ipConfigFile.readline() 
                thisLine = ipConfigFile.readline() 
                continue
            if(len(thisLine) == 0):
                break
            appendStr += "-c \'"+thisLine[:-1]+"\' "
            thisLine = ipConfigFile.readline() 
            while("!" not in thisLine):
                appendStr += "-c \'"+thisLine[:-1]+"\' "
                thisLine = ipConfigFile.readline() 
            appendStr += "-c \'exit\' "
            thisLine = ipConfigFile.readline() 
        

        thisLine = "0"
        while(len(thisLine) != 0):
            while( len(thisLine) != 0  and (("interface" not in thisLine )and ("router ospf" not in thisLine)) ):
                thisLine = ospfConfigFile.readline() 
            
            if(len(thisLine) == 0):
                break
            appendStr += "-c \'"+thisLine[:-1]+"\' "
            thisLine = ospfConfigFile.readline() 
            while("!" not in thisLine):
                appendStr += "-c \'"+thisLine[:-1]+"\' "
                thisLine = ospfConfigFile.readline() 
            appendStr += "-c \'exit\' "
            thisLine = ospfConfigFile.readline() 

        thisLine = "0"
        while(len(thisLine) != 0):
            while( len(thisLine) != 0  and ("router bgp" not in thisLine) ):
                thisLine = bgpConfigFile.readline() 
            if(len(thisLine) == 0):
                break
            appendStr += "-c \'"+thisLine[:-1]+"\' "
            thisLine = bgpConfigFile.readline() 
            while("!" not in thisLine):
                appendStr += "-c \'"+thisLine[:-1]+"\' "
                thisLine = bgpConfigFile.readline() 
            appendStr += "-c \'exit\' "
            thisLine = bgpConfigFile.readline() 

        ipConfigFile.close()
        ospfConfigFile.close()
        bgpConfigFile.close()

        #print("%s && %s " %(getPid, enterShellPrefix+appendStr))
        if(len(appendStr) != 0):
            subprocess.call("%s && %s " %(getPid, enterShellPrefix+appendStr) , shell=True)
    
    


