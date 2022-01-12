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

#define ARPLEN 42

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

int isIPPacket(uint8_t *data, unsigned int len)
{
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

  return ARPLEN;
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
      printf("get arp request!\n");
      uint8_t* resultMAC = findMyMAC(sr, targetIP);
      if(resultMAC)
      {
        uint8_t *data = (uint8_t*)malloc(sizeof(uint8_t)* ARPLEN);
        int dataLen = generateARPReply(data, senderMAC, senderIP, resultMAC, targetIP);
        sr_send_packet(sr, data, dataLen, interface);
        printf("send data to client\n");
        free(data);
      }
    }
    else{
      /*get arp response*/
    }

    free(senderMAC);
    free(targetMAC);
  }
  else if(isIPPacket(packet, len))
  {

  }
  else
  {
    /*drop it*/
  }

}/* end sr_ForwardPacket */

