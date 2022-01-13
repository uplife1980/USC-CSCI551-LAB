/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>


#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"


/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    pthread_detach(thread);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/
int isARP(uint8_t *data, unsigned int len)
{
  /*len < 42, cannot package to ARP packet.*/
  if(len < 42) return 0;

  if((data[12] == 0x08) && (data[13] == 0x06)) return 1;

  return 0;
}

int isARPRequest(uint8_t *data, unsigned int len)
{
  if((data[20] == 0) && (data[21] == 1)) return 1;
  return 0;
}

int isValidIPPacket(uint8_t *data, unsigned int len)
{
  if(len < sizeof(sr_ethernet_hdr_t))
    return 0;
  
  if(ethertype(data) == 2048)
  {
    data += sizeof(sr_ethernet_hdr_t);
    len -= sizeof(sr_ethernet_hdr_t);

    /*checksum*/
    if(cksum(data, len) == 0xFFFF)
    {
      return 1;
    }
    else{
      /*printf("checksum invalid, will drop the packet!\n");*/
      return 1;
    }
  }

  return 0;
}

uint8_t* extractSenderMAC(uint8_t *data, unsigned int len)
{
  uint8_t* MACAddr = (uint8_t*)malloc(sizeof(uint8_t)* ETHER_ADDR_LEN);
  memcpy(MACAddr, data+22, ETHER_ADDR_LEN);
  return MACAddr;

}

uint8_t* extractTargetMAC(uint8_t *data, unsigned int len)
{
  uint8_t* MACAddr = (uint8_t*)malloc(sizeof(uint8_t) * ETHER_ADDR_LEN);
  memcpy(MACAddr, data+32, ETHER_ADDR_LEN);
  return MACAddr;
}

uint32_t extractSenderIP(uint8_t *data, unsigned int len)
{
  uint32_t ip = 0;
  uint8_t *i = data + 28;
  for(; i < data + 32; i++)
  {
    ip <<= 8;
    ip += *i;
  }
  return htonl(ip);
}

uint32_t extractTargetIP(uint8_t *data, unsigned int len)
{
  uint32_t ip = 0;
  uint8_t *i = data + 38;
  for(; i < data + 42; i++)
  {
    ip <<= 8;
    ip += *i;
  }
  
  return htonl(ip);
}

uint8_t* findMyMAC(struct sr_instance* sr, uint32_t ipaddr)
{
  struct sr_if *interface = sr->if_list;
  uint8_t* MACAddr = NULL;
  while(interface)
  {
    if(interface->ip == ipaddr)
    {
      MACAddr = interface->addr;
      break;
    }
    else
    {
      interface = interface->next;
    }
  }

  return MACAddr;
}

int generateARPReply(uint8_t* data, uint8_t* dst, uint32_t dstIP, uint8_t* src, uint32_t srcIP)
{
  
  memcpy(data, dst, ETHER_ADDR_LEN);
  data += ETHER_ADDR_LEN;
  memcpy(data, src, ETHER_ADDR_LEN);
  data += ETHER_ADDR_LEN;
  
  *data = 0x08; data++; /*Type*/
  *data = 0x06; data++; 
  *data = 0x00; data++; /*hardware type*/
  *data = 0x01; data++;
  *data = 0x08; data++; /*protocol type*/
  *data = 0x00; data++;
  *data = 0x06; data++; /*hardware size*/
  *data = 0x04; data++; /*protocol size*/

  *data = 0x00; data++; /*op code*/
  *data = 0x02; data++;

  memcpy(data, src, ETHER_ADDR_LEN);
  data += ETHER_ADDR_LEN;

  memcpy(data, &srcIP, sizeof(srcIP));
  data += sizeof(srcIP);

  memcpy(data, dst, ETHER_ADDR_LEN);
  data += ETHER_ADDR_LEN;

  memcpy(data, &dstIP, sizeof(dstIP));

  return sizeof(sr_ethernet_hdr_t)+sizeof(sr_arp_hdr_t);
}

int isForMe(struct sr_instance* sr, uint8_t * data, int len)
{
  data += sizeof(sr_ethernet_hdr_t);
  len -= sizeof(sr_ethernet_hdr_t);
  sr_ip_hdr_t *iphdr = (sr_ip_hdr_t *)(data);
  struct sr_if *interface = sr->if_list;
  while(interface)
  {
    if(interface->ip == iphdr->ip_dst)
    {
      return 1;
    }

    interface = interface->next;
  }

  return 0;
}


void handleIncomingICMP(struct sr_instance* sr, uint8_t * packet/* lent */, unsigned int len, char* interface/* lent */)
{
  uint8_t *data = packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t);
  
  printf("get ICMP PACKET\n");
  switch (*data)
  {
    case 0x08:
    {
      uint8_t *resData = (uint8_t*)calloc(sizeof(uint8_t), len);
      memcpy(resData, packet, len);
      
      sr_ethernet_hdr_t *ethData = (sr_ethernet_hdr_t *)resData;
      
      memcpy(ethData->ether_dhost, ((sr_ethernet_hdr_t *)packet)->ether_shost, ETHER_ADDR_LEN);
      memcpy(ethData->ether_shost, ((sr_ethernet_hdr_t *)packet)->ether_dhost, ETHER_ADDR_LEN);
      ethData->ether_type = ((sr_ethernet_hdr_t *)packet)->ether_type;


      sr_ip_hdr_t *ipData = (sr_ip_hdr_t*)((uint8_t*)resData + sizeof(sr_ethernet_hdr_t));

      /*memcpy(ipData, (packet + sizeof(sr_ethernet_hdr_t)), sizeof(sr_ip_hdr_t));*/
      ipData->ip_dst = ((sr_ip_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t)))->ip_src;
      ipData->ip_src = ((sr_ip_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t)))->ip_dst;
      ipData->ip_ttl = 255;
      ipData->ip_sum = 0;
      ipData->ip_sum = cksum(ipData, sizeof(sr_ip_hdr_t));
      
      sr_icmp_hdr_t *icmpData = (sr_icmp_hdr_t*)((uint8_t*)ipData + sizeof(sr_ip_hdr_t));

      /*memcpy(icmpData, packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t), 8);*/
      icmpData->icmp_type = 0;
      icmpData->icmp_code = 0;
      icmpData->icmp_sum = 0;
      icmpData->icmp_sum = cksum(icmpData, ntohs(ipData->ip_len)-sizeof(sr_ip_hdr_t));
      

      sr_send_packet(sr, resData, len, interface);

      free(resData);
      break;
    }
    default:
    {
    }
  }
}

void generateICMP(struct sr_instance* sr, uint8_t * packet/* lent */, unsigned int len, char* interface/* lent */, uint8_t icmp_type, uint8_t icmp_code)
{
  struct sr_if* interfaceStruct = sr_get_interface(sr, interface);

  unsigned int resDataLen;
  if(icmp_type == TYPE_DST_UNREACHABLE)
    resDataLen= sizeof(sr_ethernet_hdr_t)+ sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
  else
    resDataLen = sizeof(sr_ethernet_hdr_t)+ sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_hdr_t);

  uint8_t *resData = (uint8_t*)malloc(sizeof(uint8_t) * resDataLen);

  sr_ethernet_hdr_t *ethData = (sr_ethernet_hdr_t *)resData;
      
  memcpy(ethData->ether_dhost, ((sr_ethernet_hdr_t *)packet)->ether_shost, ETHER_ADDR_LEN);
  memcpy(ethData->ether_shost, interfaceStruct->addr, ETHER_ADDR_LEN);
  ethData->ether_type = ((sr_ethernet_hdr_t *)packet)->ether_type;

  sr_ip_hdr_t *ipData = (sr_ip_hdr_t*)((uint8_t*)resData + sizeof(sr_ethernet_hdr_t));
  memcpy(ipData, (packet + sizeof(sr_ethernet_hdr_t)), sizeof(sr_ip_hdr_t));
  ipData->ip_p = 1 ;
  ipData->ip_dst = ((sr_ip_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t)))->ip_src;
  ipData->ip_src = interfaceStruct->ip;
  ipData->ip_len = htons(sizeof(sr_ip_hdr_t)+sizeof(sr_icmp_t3_hdr_t));
  ipData->ip_ttl = 255;
  ipData->ip_sum = 0;
  ipData->ip_sum = cksum(ipData, sizeof(sr_ip_hdr_t));

  if(icmp_type == TYPE_DST_UNREACHABLE)
  {
    sr_icmp_t3_hdr_t *icmpData = (sr_icmp_t3_hdr_t*)((uint8_t*)ipData + sizeof(sr_ip_hdr_t));
    icmpData->icmp_type = icmp_type;
    icmpData->icmp_code = icmp_code;
    icmpData->icmp_sum = 0;
    icmpData->icmp_sum = cksum(icmpData, ntohs(sizeof(sr_icmp_t3_hdr_t)));
  }
  else
  {
    sr_icmp_hdr_t *icmpData = (sr_icmp_hdr_t*)((uint8_t*)ipData + sizeof(sr_ip_hdr_t));
    icmpData->icmp_type = icmp_type;
    icmpData->icmp_code = icmp_code;
    icmpData->icmp_sum = 0;
    icmpData->icmp_sum = cksum(icmpData, ntohs(sizeof(sr_icmp_hdr_t)));
  }
  

  sr_send_packet(sr, resData, len, interface);

  free(resData);
}

void handleTCP(struct sr_instance* sr, uint8_t * packet/* lent */, unsigned int len, char* interface/* lent */)
{
  generateICMP(sr, packet, len, interface, TYPE_DST_UNREACHABLE, PORT_UNREACHABLE);
}

void handleUDP(struct sr_instance* sr, uint8_t * packet/* lent */, unsigned int len, char* interface/* lent */)
{
  generateICMP(sr, packet, len, interface, TYPE_DST_UNREACHABLE, PORT_UNREACHABLE);
}

int calcMatchLevel( uint32_t addr1,  struct sr_rt* rt)
{
  if(!rt) return 0;  

  int matchLevel = 0;

  uint32_t rtNetAddr = (rt->mask.s_addr)&(rt->dest.s_addr);

  while(matchLevel < 32)
  {
    if((addr1&(uint8_t)(0x01)) != (rtNetAddr&(uint8_t)(0x01))) 
      break;

    addr1 >>= 1;
    rtNetAddr >>= 1;
    matchLevel++;
  }

  return matchLevel;

}

struct sr_rt* checkRoutingTable(struct sr_instance* sr, uint8_t * packet, unsigned int len)
{
  sr_ip_hdr_t *ipData = (sr_ip_hdr_t*)((uint8_t*)packet + sizeof(sr_ethernet_hdr_t));
  /*print_addr_ip_int(ntohl(ipData->ip_dst));
  print_addr_ip_int(ntohl(sr->routing_table->next->dest.s_addr));*/

  struct sr_rt* defaultRoute = NULL;

  struct sr_rt* currRt = sr->routing_table;

  struct sr_rt* longgestMatch = NULL;
  int maxMatchLevel = 0;
  

  while(currRt)
  {
    if(currRt->mask.s_addr == 0) defaultRoute = currRt;

    int tempMatchLevel = calcMatchLevel(ipData->ip_dst, currRt);
    if(tempMatchLevel > maxMatchLevel)
    {
      maxMatchLevel = tempMatchLevel;
      longgestMatch = currRt;
    }

    currRt = currRt->next;
  }

  if(maxMatchLevel != 0)
  {
    return longgestMatch;
  }
  return defaultRoute;
}

void sendARPReuqest(struct sr_instance* sr, struct sr_packet *packetStruct, uint32_t ipAddr)
{
  if(!packetStruct)
    return ;
  struct sr_if* interface = sr_get_interface(sr, packetStruct->iface);
  if(!interface)
    return;

  int dataLen = sizeof(sr_ethernet_hdr_t)+sizeof(sr_arp_hdr_t);
  uint8_t *resData = (uint8_t*)malloc(sizeof(uint8_t)* dataLen);
  sr_ethernet_hdr_t *ethData = (sr_ethernet_hdr_t *)resData;   
  sr_arp_hdr_t * arpData = (sr_arp_hdr_t*)(resData+sizeof(sr_ethernet_hdr_t));

  memcpy(ethData->ether_shost, interface->addr, ETHER_ADDR_LEN);
  uint8_t broadcastAddr[ETHER_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  memcpy(ethData->ether_dhost, &broadcastAddr , ETHER_ADDR_LEN);
  ethData->ether_type = htons(0x0806);

  arpData->ar_hrd = htons(0x0001);
  arpData->ar_pro = htons(0x0800);
  arpData->ar_hln = 6;
  arpData->ar_pln = 4;
  arpData->ar_op = htons(0x01);

  memcpy(arpData->ar_sha, interface->addr, ETHER_ADDR_LEN);
  arpData->ar_sip = interface->ip;
  memcpy(arpData->ar_tha, &broadcastAddr , ETHER_ADDR_LEN);
  arpData->ar_tip = ipAddr;

  sr_send_packet(sr, resData, dataLen, interface->name);

  free(resData);


}

void forwardPacket(struct sr_instance* sr, uint8_t * packet, unsigned int len, char* interface, struct sr_rt* rt)
{
  uint8_t *resData = (uint8_t*)malloc(sizeof(uint8_t)* len);

  memcpy(resData, packet, len);

  struct sr_if* sourceInterface = sr_get_interface(sr, rt->interface);
  if(!sourceInterface)
  {
    free(resData);
    return;
  }

  struct sr_arpentry *arpLookUpResult = sr_arpcache_lookup(&(sr->cache), rt->gw.s_addr);
  
  
  sr_ethernet_hdr_t *ethData = (sr_ethernet_hdr_t *)resData;    

  memcpy(ethData->ether_shost, sourceInterface->addr, ETHER_ADDR_LEN);
  if(arpLookUpResult)
  {
    memcpy(ethData->ether_dhost, arpLookUpResult->mac, ETHER_ADDR_LEN);
    free(arpLookUpResult);
  }

  sr_ip_hdr_t *ipData = (sr_ip_hdr_t*)((uint8_t*)resData + sizeof(sr_ethernet_hdr_t));
  ipData->ip_ttl -= 1;
  if(ipData->ip_ttl == 0)
  {
    /* TTL == 0, drop the packet*/
    generateICMP(sr, packet, len, interface, TYPE_TIME_EXCEEDED, CODE_TIME_EXCEEDED);
    free(resData);
    return;
  }
  ipData->ip_sum = 0;
  ipData->ip_sum = cksum(ipData, sizeof(sr_ip_hdr_t));

  if(arpLookUpResult)
  {
    sr_send_packet(sr, resData, len, rt->interface);
    free(resData);
  }
  else
  {
    struct sr_arpreq *arpReq = sr_arpcache_queuereq(&(sr->cache), rt->gw.s_addr, resData, len, rt->interface);

    handle_arpReq(sr, arpReq);
  }

}

void handleARPResponse(struct sr_instance* sr, uint8_t * packet/* lent */, unsigned int len, char* interface/* lent */)
{
  sr_arp_hdr_t * arpData = (sr_arp_hdr_t*)(packet+sizeof(sr_ethernet_hdr_t));

  struct sr_arpreq *req = sr_arpcache_insert(&(sr->cache), arpData->ar_sha, arpData->ar_sip);
  if(!req)
    return;

  struct sr_packet *watingPacket = req->packets;
  

  while(watingPacket)
  {
    sr_ethernet_hdr_t *ethData = (sr_ethernet_hdr_t *)watingPacket->buf;
    memcpy(ethData->ether_dhost, arpData->ar_sha, ETHER_ADDR_LEN);
    

    sr_send_packet(sr, watingPacket->buf, watingPacket->len, watingPacket->iface);
    
    watingPacket = watingPacket->next;
    
  }
  sr_arpreq_destroy(&(sr->cache), req);

}

void sr_handlepacket(struct sr_instance* sr,
        uint8_t * packet/* lent */,
        unsigned int len,
        char* interface/* lent */)
{
  /* REQUIRES */
  assert(sr);
  assert(packet);
  assert(interface);

  printf("*** -> Received packet of length %d \n",len);

  if(isARP(packet, len))
  {
    uint8_t* senderMAC = extractSenderMAC(packet, len);
    uint32_t senderIP = extractSenderIP(packet, len);
    uint8_t* targetMAC = extractTargetMAC(packet, len);
    uint32_t targetIP = extractTargetIP(packet, len);

    if(isARPRequest(packet, len))
    {
      uint8_t* resultMAC = findMyMAC(sr, targetIP);
      if(resultMAC)
      {
        uint8_t *data = (uint8_t*)malloc(sizeof(uint8_t)* (sizeof(sr_ethernet_hdr_t)+sizeof(sr_arp_hdr_t)));
        int dataLen = generateARPReply(data, senderMAC, senderIP, resultMAC, targetIP);
        sr_send_packet(sr, data, dataLen, interface);
        free(data);
      }
    }
    else{
      /*get arp response*/
      handleARPResponse(sr, packet, len, interface);
    }

    free(senderMAC);
    free(targetMAC);
  }
  else if(isValidIPPacket(packet, len))
  {
    if(isForMe(sr, packet, len))
    {
      int protocol = ip_protocol(packet+sizeof(sr_ethernet_hdr_t));
      switch (protocol)
      {
          case 1:
          {
            handleIncomingICMP(sr, packet, len , interface);
            break;
          }
          case 17:
          {
            handleUDP(sr, packet, len, interface);
            break;
          }
          case 6:
          {
            handleTCP(sr, packet, len, interface);
            break;
          }
          default:
          {
          }
      }
      
    }
    else
    {
      /*forward to other*/
      struct sr_rt* routingTableItem = checkRoutingTable(sr, packet, len);
      if(routingTableItem != NULL)
      {
        forwardPacket(sr, packet, len, interface, routingTableItem);
      }
      else
      {
        generateICMP(sr, packet, len, interface, TYPE_DST_UNREACHABLE, NET_UNREACHABLE);
      }
    }
    
  }
  else
  {
    /*drop it*/
    printf("drop one");
  }

}/* end sr_ForwardPacket */

