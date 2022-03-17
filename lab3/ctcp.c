/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"

/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */
  linked_list_t *unsentList, *sentUnackList, *unsubmitedList;  
  /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */

  uint16_t sendWindow, recvWindow;
  int rtt;
  int timer;
  uint32_t seqNum, ackNum, lastAckedSeq;
  bool stopRecv, prepareSendFIN, sentFIN;
  bool singleACKUpdate; //No data to send, just ACK empty packet


};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;


ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  if(!state)
  {
    free(cfg);
    return NULL;
  }
    

  state->unsentList = calloc(sizeof(linked_list_t), 1);
  state->sentUnackList = calloc(sizeof(linked_list_t), 1);
  state->unsubmitedList = calloc(sizeof(linked_list_t), 1);

  if(!(state->unsentList && state->sentUnackList && state->unsubmitedList))
  {
    ctcp_destroy(state);
    free(state->unsentList);
    free(state->sentUnackList);
    free(state->unsubmitedList);
    free(cfg);
    return NULL;
  }


  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  
  state->recvWindow = cfg->recv_window;
  state->sendWindow = cfg->send_window;
  state->rtt = cfg->rt_timeout;
  state->timer = cfg->timer;
  free(cfg);

  state->seqNum = 0;
  state->ackNum = 0;
  state->lastAckedSeq = 0;
  state->stopRecv = false;
  state->prepareSendFIN = false;
  state->singleACKUpdate = false;
  state->sentFIN = false;

  return state;
}

void cleanBuffer(buffer_t *buffer)
{
  if(!buffer)
    return;
    
  free(buffer->data);
  free(buffer);
}

void cleanBufferList(linked_list_t* list)
{
  if(!list)
    return;

  ll_node_t *p = ll_front(list);
  while(p)
  {
    cleanBuffer((buffer_t*)(p->object));
    ll_remove(list, p);
    p = ll_front(list);
  }


  free(list);
}

void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  //cleanBufferList(state->unsentList);
  //cleanBufferList(state->sentUnackList);
  //cleanBufferList(state->unsubmitedList);

  free(state);
  end_client();
}


void trySend(ctcp_state_t *state)
{
  ll_node_t *bufNode = ll_front(state->unsentList);
  if(!bufNode && !state->prepareSendFIN && !state->singleACKUpdate)
    return;
  
  //printf("\n The reason I send: %d %d %d \n", bufNode?1:0, (state->prepareSendFIN)? 1:0, (state->singleACKUpdate)? 1:0);

  bool shouldInsertToUnackList = true;
  if(!bufNode && !state->prepareSendFIN)
    shouldInsertToUnackList = false;
  
  uint32_t totalUnsentLen = 0;

  if(bufNode)
  {
     /*calculate all unsentData Length*/
      buffer_t *buf = bufNode->object;
      totalUnsentLen = buf->len - buf->usedLen;
      while(bufNode->next)
      {
        bufNode = bufNode->next;
        buf = bufNode->object;
        totalUnsentLen += buf->len - buf->usedLen;
      }
  }
 


  /*decide how much to send*/
  int dataLen = MAX_SEG_DATA_SIZE > totalUnsentLen? totalUnsentLen: MAX_SEG_DATA_SIZE;
  dataLen = dataLen > state->sendWindow? state->sendWindow: dataLen;

  int pos = 0;
  ctcp_segment_t *segment = malloc(sizeof(ctcp_segment_t) + dataLen);
  char *data = segment->data;

  
  
  int originDataLen = dataLen;
  while(dataLen)
  {
    bufNode = ll_front(state->unsentList);
    buffer_t* buf = (buffer_t*)(bufNode->object);

    if(dataLen >= buf->len - buf->usedLen)
    {
      printf("\n memcpy1 \n");
      memcpy(data+pos, buf->data+buf->usedLen, buf->len - buf->usedLen);
      pos += buf->len - buf->usedLen;
      dataLen -= buf->len - buf->usedLen;

      ll_remove(state->unsentList, bufNode);
      cleanBuffer(buf);
    }
    else
    {
      printf("\n memcpy2 \n");
      memcpy(data+pos, buf->data+buf->usedLen, dataLen);
      buf->usedLen += dataLen;   

      pos += dataLen;
      dataLen = 0;
    }
  }
  dataLen = originDataLen;
  
  segment->seqno = htonl(state->seqNum);
  segment->ackno = htonl(state->ackNum);
  segment->len = htons(sizeof(ctcp_segment_t) + dataLen);
  segment->flags = 0;
  segment->flags |= TH_ACK;
  //if(state->singleACKUpdate)  
  state->singleACKUpdate = false;
  
  if(totalUnsentLen == dataLen && state->prepareSendFIN)
  {
    segment->flags |= TH_FIN;
    state->sentFIN = true;
    if(dataLen == 0)
    {
      state->seqNum ++;
    }
  }
  state->sendWindow -= dataLen;
  segment->window = htons(state->recvWindow);
  segment->cksum = 0;

  segment->cksum = cksum((void*)segment, sizeof(ctcp_segment_t) + dataLen);
  conn_send(state->conn, segment, sizeof(ctcp_segment_t) + dataLen);
  int temp = sizeof(ctcp_segment_t) + dataLen;
  printf("\n I have sent %d data\n", temp);
  state->seqNum += dataLen;

  //backupData
  if(shouldInsertToUnackList)
  {
    buffer_t *unackedBuf = malloc(sizeof(buffer_t));
    unackedBuf->usedLen = 0;
    unackedBuf->lastSentTime = current_time();
    unackedBuf->retryTime = 0;
    ll_add(state->sentUnackList, unackedBuf);
    unackedBuf->data = (char*)segment;
    unackedBuf->len = sizeof(ctcp_segment_t) + dataLen;
  }
  
  
}


void ctcp_read(ctcp_state_t *state) {
 
  buffer_t *buf;
  do{
    buf = calloc(sizeof(buffer_t), 1);
    buf->len = BUFFER_SIZE;
    buf->data = malloc(BUFFER_SIZE);

    int resLen = conn_input(state->conn, buf->data, buf->len);
    if(resLen == -1)
    {
      state->prepareSendFIN = true;
      break;
    }
    buf->len = resLen;
    if(buf->len > 0)
      ll_add(state->unsentList, buf);  
  }while(buf->len > 0);
  
  cleanBuffer(buf);
  
  trySend(state);
  

}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /*checksum*/
  uint16_t seglen = ntohs(segment->len);
  uint32_t ackno = ntohl(segment->ackno);
  uint32_t seqno = ntohl(segment->seqno);
  //print_hdr_ctcp(segment);
  printf("\nI get something!\n");

  if((len < seglen))
  {
    
    fprintf(stderr, "invaild checksum %d, %d\n", (int)seglen, (int)len);
    free(segment);
    return;
  }

  
  if(seqno!= state->ackNum)
  {
    if(state->ackNum == 0)
    {
      state->ackNum = seqno;
    }
    else
    {
      fprintf(stderr, "invaild seqNum, I need %d, but get %d \n", (int)(state->ackNum), seqno);
      free(segment);
      trySend(state);
      return;
    }
    
  }

  /*adjust ACK number*/
  state->ackNum += seglen - sizeof(ctcp_segment_t);
  state->sendWindow = ntohs(segment->window);

  /*adjust lastAckedSeq number*/
  //if(seqno >= state->lastAckedSeq)
  //{
  if(seglen - sizeof(ctcp_segment_t) != 0)
    state->singleACKUpdate = true;

  uint32_t ackedDataLen = ackno - state->lastAckedSeq;
  state->lastAckedSeq =  ackno;

  while(ackedDataLen)
  {
    ll_node_t *bufNode = ll_front(state->sentUnackList);
    buffer_t* buf = ((buffer_t*)(bufNode->object));
    
    ackedDataLen -= buf->len - sizeof(ctcp_segment_t);
    ll_remove(state->sentUnackList, bufNode);
    free(buf); //this buf is a fake segment, not buf
    
    
  }
  //}
  
  /*adjust recvWindow*/
  state->recvWindow += seglen - sizeof(ctcp_segment_t);

  if(segment->flags & TH_FIN)
  {
    /*deal with FIN*/
    if(seglen == 0)
      state->ackNum++;
    conn_output(state->conn, NULL, 0);
    state->stopRecv = true;
  }

  buffer_t *buf = calloc(sizeof(buffer_t), 1);
  buf->len = seglen - sizeof(ctcp_segment_t);
  buf->data = malloc(buf->len);
  printf("\n memcpy3 \n");
  memcpy(buf->data, segment->data, buf->len);

  if(buf->len)
  {
    ll_add(state->unsubmitedList, buf);
  }
  else
  {
    cleanBuffer(buf);
  }

  free(segment);
  trySend(state);
  ctcp_output(state);
  

}

void ctcp_output(ctcp_state_t *state) {
  size_t availableSize = conn_bufspace(state->conn);

  while(availableSize && ll_length(state->unsubmitedList))
  {
    ll_node_t *bufNode = ll_front(state->unsubmitedList);
    buffer_t *buf = bufNode->object;
    if(availableSize > buf->len - buf->usedLen)
    {
      conn_output(state->conn, buf->data+buf->usedLen, buf->len - buf->usedLen);
      ll_remove(state->unsubmitedList, bufNode);
      cleanBuffer(buf);
      break;
    }
    else
    {
      conn_output(state->conn, buf->data + buf->usedLen, availableSize);
      buf->usedLen += availableSize;
    }
    
  }
}

void ctcp_timer() {

  ctcp_state_t *currentState = state_list;
  while(currentState)
  {
    ll_node_t *bufObj = ll_front(currentState->sentUnackList);
    bool shouldDestroy = false;
    if(!bufObj && currentState->sentFIN && currentState->stopRecv)
    {
      shouldDestroy = true;
    }
    while(bufObj)
    {
      buffer_t* buf = bufObj->object;
      long currentTime = current_time();
      if(currentTime - buf->lastSentTime > currentState->rtt)
      {
          if(buf->retryTime == 5)
          {
            shouldDestroy = true;
            break;
          }

          ctcp_segment_t *segment = (ctcp_segment_t*)(buf->data);
          int temp = buf->len ;
          conn_send(currentState->conn, segment ,buf->len);
          printf("\n I have sent %d retry data\n",temp);
          buf->lastSentTime = currentTime;
          buf->retryTime++;

      }
      bufObj = bufObj->next;
    }
    if(shouldDestroy)
    {
      ctcp_state_t *tempState = currentState;
      currentState = currentState->next;
      ctcp_destroy(tempState);
    }
    else
    {
      currentState = currentState->next;
    }
    
  }
  
}
