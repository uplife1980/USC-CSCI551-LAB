#!/usr/bin/python

import subprocess
import sys

routerList = ["SEAT","LOSA", "SALT", "HOUS", "KANS", "ATLA", "WASH", "CHIC", "NEWY"]


if __name__ == '__main__':
    folder = sys.argv[1]
    
    for router in routerList:
        appendStr = ""
        getPid = "pid=`eval ps ax | grep \"mininet:%s\" | grep bash | grep -v mxexec |grep -v host| awk '{print $1};'`" %(router)
        enterShellPrefix = "sudo mxexec -a $pid -b $pid -k $pid vtysh -c \'conf ter\' "

        ipConfigFile = open("./"+folder+"/"+router+"/zebra.conf.sav")
        ospfConfigFile = open("./"+folder+"/"+router+"/ospfd.conf.sav")

        thisLine = "0"

        while(len(thisLine) != 0):
            while( len(thisLine) != 0  and "interface" not in thisLine):
                thisLine = ipConfigFile.readline() 
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

        ipConfigFile.close()
        ospfConfigFile.close()
    
        #print("%s && %s " %(getPid, enterShellPrefix+appendStr))
        if(len(appendStr) != 0):
            subprocess.call("%s && %s " %(getPid, enterShellPrefix+appendStr) , shell=True)
    