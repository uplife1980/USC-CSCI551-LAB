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
  bool stopRecv, prepareSendFIN;
  bool singleACKUpdate;


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

  state->seqNum = 1;
  state->ackNum = 0;
  state->lastAckedSeq = 0;
  state->stopRecv = false;
  state->prepareSendFIN = false;
  state->singleACKUpdate = false;

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

  cleanBufferList(state->unsentList);
  cleanBufferList(state->sentUnackList);
  cleanBufferList(state->unsubmitedList);

  free(state);
  end_client();
}

uint16_t calculateCkSum(ctcp_state_t *state, ctcp_segment_t *segment)
{
  return 0;
}

void trySend(ctcp_state_t *state)
{

  ll_node_t *bufNode = ll_front(state->unsentList);
  if(!bufNode && !state->prepareSendFIN && !state->singleACKUpdate)
    return;

  /*calculate all unsentData Length*/
  buffer_t *buf = bufNode->object;
  uint32_t totalUnsentLen = buf->len - buf->usedLen;
  while(bufNode->next)
  {
    bufNode = bufNode->next;
    buf = bufNode->object;
    totalUnsentLen += buf->len - buf->usedLen;
  }


  /*decide how much to send*/
  int dataLen = MAX_SEG_DATA_SIZE > totalUnsentLen? totalUnsentLen: MAX_SEG_DATA_SIZE;
  dataLen = dataLen > state->sendWindow? state->sendWindow: dataLen;

  int pos = 0;
  ctcp_segment_t *segment = malloc(sizeof(ctcp_segment_t) + dataLen);
  char *data = segment->data;
  
  
  while(dataLen)
  {
    bufNode = ll_front(state->unsentList);
    buf = (buffer_t*)(bufNode->object);
    if(dataLen >= buf->len - buf->usedLen)
    {
      memcpy(data+pos, buf->data+buf->usedLen, buf->len - buf->usedLen);
      pos += buf->len - buf->usedLen;
      dataLen -= buf->len - buf->usedLen;

      buffer_t *unackedBuf = malloc(sizeof(buffer_t));
      unackedBuf->usedLen = 0;
      unackedBuf->len = buf->len - buf->usedLen;
      unackedBuf->data = malloc(unackedBuf->len);
      memcpy(unackedBuf->data, buf->data + buf->usedLen, unackedBuf->len);
      ll_add(state->sentUnackList, unackedBuf);

      ll_remove(state->unsentList, bufNode);
      cleanBuffer(buf);

    }
    else
    {
      memcpy(data+pos, buf->data+buf->usedLen, dataLen);
      buf->usedLen += dataLen;
      
      buffer_t *unackedBuf = malloc(sizeof(buffer_t));
      unackedBuf->usedLen = 0;
      unackedBuf->len = dataLen;
      unackedBuf->data = malloc(unackedBuf->len);
      memcpy(unackedBuf->data, buf->data + buf->usedLen, unackedBuf->len);
      ll_add(state->sentUnackList, unackedBuf);

      pos += dataLen;
      dataLen = 0;
    }
  }

  
  segment->seqno = state->seqNum;
  state->seqNum += dataLen;
  segment->ackno = state->ackNum;
  segment->len = sizeof(ctcp_segment_t) + dataLen;
  segment->flags = 0;
  if(state->singleACKUpdate)
  {
    segment->flags |= ACK;
    state->singleACKUpdate = false;
  }
  if(totalUnsentLen == dataLen && state->prepareSendFIN)
  {
    segment->flags |= FIN;
  }
  segment->window = state->recvWindow;
  segment->cksum = 0;

  segment->cksum = calculateCkSum(state, segment);



  //update seqNum, delete list, free data, sendW
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
    ll_add(state->unsentList, buf);  
  }while(buf->len > 0);
  
  cleanBuffer(buf);
  
  trySend(state);//notice send half of a buf, and unacked being acked
  

}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /*checksum*/
  if(calculateCkSum(state, segment) || (len < segment->len))
  {
    print_hdr_ctcp(segment);
    fprintf(stderr, "invaild checksum\n");
    free(segment->data);
    free(segment);
    return;
  }

  state->singleACKUpdate = true;


  if(segment->seqno != state->ackNum)
  {
    /*TODO: fast retransmission*/
    fprintf(stderr, "invaild seqNum\n");
    free(segment->data);
    free(segment);
    trySend(state);
    return;
  }

  /*adjust ACK number*/
  state->ackNum += segment->len - sizeof(ctcp_segment_t);
  

  /*adjust lastAckedSeq number*/
  if(segment->ackno > state->lastAckedSeq)
  {
    uint32_t ackedDataLen = segment->ackno - state->lastAckedSeq;
    state->lastAckedSeq =  segment->ackno;
    state->sendWindow += ackedDataLen;

    while(ackedDataLen)
    {
      ll_node_t *bufNode = ll_front(state->sentUnackList);
      buffer_t* buf = ((buffer_t*)(bufNode->object));
      if(ackedDataLen >= buf->len - buf->usedLen)
      {
        ackedDataLen -= buf->len - buf->usedLen;
        ll_remove(state->sentUnackList, bufNode);
        cleanBuffer(buf);
      }
      else
      {
        buf->usedLen += ackedDataLen;
        ackedDataLen = 0;
      }
    }
  }
  
  /*adjust recvWindow*/
  state->recvWindow += segment->len - sizeof(ctcp_segment_t);

  if(segment->flags & ACK)
  {
    /*deal with FIN*/
    state->ackNum++;
    conn_output(state->conn, NULL, 0);
    state->stopRecv = true;
  }


  buffer_t *buf = calloc(sizeof(buffer_t), 1);
  buf->len = segment->len - sizeof(ctcp_segment_t);
  buf->data = segment->data;

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
  if(!state_list)
    return;

  
}
