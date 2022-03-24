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

#include "ctcp_bbr.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"
#define DEBUG

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
  uint16_t inflightData;
  int rtt;
  int timer;
  uint32_t seqNum, ackNum, lastAckedSeq;
  bool stopRecv; //I received FIN from peer
  bool singleACKUpdate; //No data to send, just ACK empty packet

  //0--NO FIN, 1--prepare to send FIN 2--has sent FIN
  uint8_t prepareSendFINStatus;

  //for bbr
  long lastTouchMinRTTTime, lastTouchMaxBWTime;
  uint32_t estimateBandWidthBs; // B/s
  double gain;
  long lastSentTime;
  FILE *bdpFile;


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
  state->ackNum = 1;
  state->lastAckedSeq = 1;
  state->inflightData = 0;
  state->stopRecv = false;
  state->prepareSendFINStatus = 0;
  state->singleACKUpdate = false;

  state->lastSentTime = state->lastTouchMinRTTTime = state->lastTouchMaxBWTime = current_time();
  state->estimateBandWidthBs = state->sendWindow;
  state->gain = 1.1;
  char temp[255]="random";
  srand((unsigned int)state->lastSentTime);
  sprintf(temp+6, "%d", rand());
  state->bdpFile = fopen(temp, "a+");
  

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
    void *obj = ll_remove(list, p);
    cleanBuffer((buffer_t *)obj);
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
  fclose(state->bdpFile);
}

void printBDP(ctcp_state_t *state, int thisTimeSentData)
{
    long currentTime = current_time();
    #ifndef DEBUG
    int bdp = state->estimateBandWidthBs * state->rtt / 1000;
    #endif
    char tempString [255];
    int endPos = sprintf(tempString, "%ld", currentTime);
    tempString[endPos++] = ',';
    #ifdef DEBUG
    endPos += sprintf(&(tempString[endPos]), "%d * ", (int)(state->estimateBandWidthBs));
    endPos += sprintf(&(tempString[endPos]), "%d = ", (int)(state->rtt));
    endPos += sprintf(&(tempString[endPos]), "%d", (state->estimateBandWidthBs)*(state->rtt) / 1000);
    endPos += sprintf(&(tempString[endPos]), " <-> %u  ", (state->inflightData + thisTimeSentData));
    tempString[endPos++] = ',';
    endPos += sprintf(&(tempString[endPos]), "%lf", (state->gain));
    tempString[endPos++] = ',';
    endPos += sprintf(&(tempString[endPos]), "%d", thisTimeSentData);
    #else 
    endPos += sprintf(&(tempString[endPos]), "%d", bdp*8);
    #endif
    tempString[endPos++] = '\n';
    tempString[endPos] = '\0';
    fprintf(state->bdpFile, "%s", tempString);
    fflush(state->bdpFile);
}

uint32_t getBBRLimit(ctcp_state_t *state)
{
    long currentTime = current_time();
    int bdp = state->estimateBandWidthBs * state->rtt / 1000;
    if(state->inflightData >= bdp * state->gain)
    {
        return 0;
    }

    long waitTime = currentTime - state->lastSentTime;
    return state->gain * waitTime/1000 * state->estimateBandWidthBs;
}

void updateBBR(ctcp_state_t *state, ctcp_segment_t *segment)
{
    //uint16_t seglen = ntohs(segment->len);
    uint32_t ackno = ntohl(segment->ackno);
    //uint32_t seqno = ntohl(segment->seqno);
    long currentTime = current_time();

    //find the max seqNum < ackno in sentUnackList
    ll_node_t *bufNode = ll_back(state->sentUnackList);
    while(bufNode)
    {
        buffer_t *buf = (buffer_t*)(bufNode->object);
        uint32_t currentSeqNumInNetOrder = ((ctcp_segment_t*)(buf->data))->seqno;
        if( ntohl(currentSeqNumInNetOrder) < ackno)
        {
            break;
        }
        bufNode = bufNode->prev;
    }

    if(!bufNode)
        return;
    buffer_t *buf = (buffer_t*)(bufNode->object);
    //we cannot use retry packet.
    if(buf->retryTime)
        return;

    int currentRTT = currentTime - buf->lastSentTime;
    if(currentRTT == 0)
      currentRTT = 1;

    //update RTT
    do
    {
        //over 120% * minRTT, need to do something!
        if(currentRTT > state->rtt * ABNORMAL_RANGE){
            state->gain = 0.9;
            #ifdef DEBUG
            fprintf(state->bdpFile, "[rtt update] terrible rtt: %d -> %d\n",state->rtt, currentRTT);
            fflush(state->bdpFile);
            #endif
        }
        // still operate good, we can add more!
        else if(currentRTT * ABNORMAL_RANGE< state->rtt)
        {
            #ifdef DEBUG
            fprintf(state->bdpFile, "[rtt update] good rtt: %d -> %d\n",state->rtt, currentRTT);
            fflush(state->bdpFile);
            #endif
            state->rtt = currentRTT;
            state->lastTouchMinRTTTime = currentTime;
            state->gain = 1.1;
            
        }
        else
        {
          state->gain = 1.1;
          #ifdef DEBUG
          fprintf(state->bdpFile, "[rtt update] rtt: %d -> %d\n",state->rtt, currentRTT);
          fflush(state->bdpFile);
          #endif
        }

        //outdated, forced to update min RTT
        if(currentTime - state->lastTouchMinRTTTime > 10*1000)
        {
            #ifdef DEBUG
            fprintf(state->bdpFile, "[rtt update] forced update rtt: %d -> %d\n",state->rtt, currentRTT);
            fflush(state->bdpFile);
            #endif
            state->rtt = currentRTT;
            state->lastTouchMinRTTTime = currentTime;
            
        }

    } while (0);

    //update BW
    do
    {
        if(buf->appLimit)
          break;
        int currentAckedAmount = ackno - buf->currentLastestAck;
        if(currentAckedAmount == 0)
            break;
        
        if(currentRTT == 0)
          break;
        uint32_t currentBW = (double)currentAckedAmount / (double)currentRTT * 1000;

        //BW goes up
        if(currentBW > state->estimateBandWidthBs * ABNORMAL_RANGE)
        {
            #ifdef DEBUG
            fprintf(state->bdpFile, "[bw update] bw up: %d -> %d\n",state->estimateBandWidthBs, currentBW);
            fflush(state->bdpFile);
            #endif
            state->estimateBandWidthBs = currentBW;
            // still operate good, we can add more!
            state->gain = state->gain <= 1.1? 1.1:(state->gain);

        }
        //bw too low
        else if(!buf->appLimit &&  currentBW * ABNORMAL_RANGE < state->estimateBandWidthBs)
        {
            #ifdef DEBUG
            fprintf(state->bdpFile, "[bw update] bw down: %d -> %d\n",state->estimateBandWidthBs, currentBW);
            fflush(state->bdpFile);
            #endif
            state->estimateBandWidthBs = currentBW;
            state->gain = 0.9;

        }
    } while (0);
    
}

void trySend(ctcp_state_t *state)
{
  long currentTime = current_time();
  ll_node_t *bufNode = ll_front(state->unsentList);
  if(!bufNode && !(state->prepareSendFINStatus == 1) && !state->singleACKUpdate)
    return;
  

  bool shouldInsertToUnackList = true;
  if(!bufNode && (state->prepareSendFINStatus !=1 ))
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
  //decision from unsentList
  int dataLen = MAX_SEG_DATA_SIZE > totalUnsentLen? totalUnsentLen: MAX_SEG_DATA_SIZE;
  //decision from bbr
  int bbrRes = getBBRLimit(state);
  dataLen = dataLen > bbrRes ? bbrRes : dataLen;
  //decision from sendWindow
  uint16_t limitBySendWindow = (state->sendWindow - state->inflightData >= 0)? (state->sendWindow - state->inflightData): 0;
  dataLen = dataLen > limitBySendWindow? limitBySendWindow: dataLen;

  if(dataLen == 0)
  {
    shouldInsertToUnackList = false;
  }
  else
  {
    printBDP(state, dataLen < bbrRes? 0: bbrRes);
  }

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
      memcpy(data+pos, buf->data+buf->usedLen, buf->len - buf->usedLen);
      pos += buf->len - buf->usedLen;
      dataLen -= buf->len - buf->usedLen;

      ll_remove(state->unsentList, bufNode);
      cleanBuffer(buf);
    }
    else
    {
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
  //fprintf(stderr, "I sent %d \n", (int)ntohs((segment->len)));
  segment->flags = 0;
  segment->flags |= TH_ACK;
  state->singleACKUpdate = false;
  
  if(totalUnsentLen == dataLen && (state->prepareSendFINStatus == 1))
  {
    segment->flags |= TH_FIN;
    state->prepareSendFINStatus |= 2;
    if(dataLen == 0)
    {
      state->seqNum ++;
    }
  }
  
  segment->window = htons(state->recvWindow);
  segment->cksum = 0;

  segment->cksum = cksum((void*)segment, sizeof(ctcp_segment_t) + dataLen);

  state->seqNum += dataLen;
  state->lastSentTime = currentTime;


  conn_send(state->conn, segment, sizeof(ctcp_segment_t) + dataLen);

  

  //backupData
  if(shouldInsertToUnackList)
  {
    buffer_t *unackedBuf = malloc(sizeof(buffer_t));
    unackedBuf->usedLen = 0;
    unackedBuf->lastSentTime = currentTime;
    unackedBuf->retryTime = 0;
    unackedBuf->currentLastestAck = state->lastAckedSeq;
    if(dataLen < bbrRes)
        unackedBuf->appLimit = true;
    else
        unackedBuf->appLimit = false;
    ll_add(state->sentUnackList, unackedBuf);
    unackedBuf->data = (char*)segment;
    unackedBuf->len = sizeof(ctcp_segment_t) + dataLen;
    if((state->prepareSendFINStatus & 1) && dataLen == 0)
    {
      unackedBuf->len++;
    }
    state->inflightData += dataLen;
  }
  else
  {
    free(segment);
  }

  //Switch prepareSendFINStatus to 2
  if(state->prepareSendFINStatus & 1)
  {
    state->prepareSendFINStatus -= 1;
  }
  
  
}


void ctcp_read(ctcp_state_t *state) {
 
  buffer_t *buf;
  do{
    buf = calloc(sizeof(buffer_t), 1);
    buf->len = BUFFER_SIZE;
    buf->data = malloc(BUFFER_SIZE);
    buf->usedLen = 0;

    int resLen = conn_input(state->conn, buf->data, buf->len);
    if(resLen == -1)
    {
      state->prepareSendFINStatus |= 1;
      break;
    }
    buf->len = resLen;
    if(buf->len > 0)
      ll_add(state->unsentList, buf);  
  }while(buf->len > 0);
  
  cleanBuffer(buf);
  
  //trySend(state);
  

}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  uint16_t seglen = ntohs(segment->len);
  uint32_t ackno = ntohl(segment->ackno);
  uint32_t seqno = ntohl(segment->seqno);

  if(cksum((void*)segment, seglen) != 0xffff || (len < seglen))
  {
    
    //fprintf(stderr, "invaild checksum=%d, segLen=%d, len=%d\n", cksum((void*)segment, seglen), (int)seglen, (int)len);
    free(segment);
    return;
  }

  // I need former packet
  if(seqno != state->ackNum)
  {
    //fprintf(stderr, "invalid seqNum, I need %d, but get %d \n", (int)(state->ackNum), seqno);
    state->singleACKUpdate = true;
    free(segment);
    trySend(state);
    return;    
  }


  updateBBR(state, segment);
  /*adjust ACK number*/
  state->ackNum += seglen - sizeof(ctcp_segment_t);
  //fprintf(stderr, "acked to %d \n", (int)(state->ackNum));
  state->sendWindow = ntohs(segment->window);

  if(seglen - sizeof(ctcp_segment_t) != 0 || (segment->flags & TH_FIN))
    state->singleACKUpdate = true;

 //This is an ack packet I've seen.
  if(ackno < state->lastAckedSeq)
    return;
  int ackedDataLen = ackno - state->lastAckedSeq;
  state->lastAckedSeq =  ackno;

  state->inflightData -= ackedDataLen;

  //delete sentUnackList's object
  //Also, FIN packet's len == 1, but it's an empty segment.
  while(ackedDataLen > 0)
  {
    ll_node_t *bufNode = ll_front(state->sentUnackList);
    buffer_t* buf = ((buffer_t*)(bufNode->object));
    
    ackedDataLen -= buf->len - sizeof(ctcp_segment_t);
    
    ll_remove(state->sentUnackList, bufNode);
    cleanBuffer(buf);  
  }
  
  /*adjust recvWindow*/
  state->recvWindow -= seglen - sizeof(ctcp_segment_t);

  //deal with FIN
  if(segment->flags & TH_FIN)
  {
    if(seglen - sizeof(ctcp_segment_t) == 0)
      state->ackNum++;
    conn_output(state->conn, NULL, 0);
    state->stopRecv = true;
  }

  //prepare to submit peer's data
  buffer_t *buf = calloc(sizeof(buffer_t), 1);
  buf->len = seglen - sizeof(ctcp_segment_t);
  buf->data = malloc(buf->len);
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
  uint16_t hasOutputCount = 0;
  while(availableSize && ll_length(state->unsubmitedList))
  {
    ll_node_t *bufNode = ll_front(state->unsubmitedList);
    buffer_t *buf = bufNode->object;
    if(availableSize >= buf->len - buf->usedLen)
    {
      conn_output(state->conn, buf->data+buf->usedLen, buf->len - buf->usedLen);
      ll_remove(state->unsubmitedList, bufNode);
      
      hasOutputCount += buf->len - buf->usedLen;
      cleanBuffer(buf);
      break;
    }
    else
    {
      conn_output(state->conn, buf->data + buf->usedLen, availableSize);
      buf->usedLen += availableSize;
      hasOutputCount += availableSize;
    }
  }
  bool needTellSenderZeroWindow = false;
  if(state->recvWindow == 0)
  {
    needTellSenderZeroWindow = true;
  }
  state->recvWindow += hasOutputCount;
  if(needTellSenderZeroWindow)
  {
    state->singleACKUpdate = true;
    trySend(state);
  }
  
}

void ctcp_timer() {

  ctcp_state_t *currentState = state_list;
  while(currentState)
  {
    trySend(currentState);
    ll_node_t *bufObj = ll_front(currentState->sentUnackList);
    bool shouldDestroy = false;
    if(!bufObj && (currentState->prepareSendFINStatus & 2) && currentState->stopRecv)
    {
      shouldDestroy = true;
    }
    while(bufObj)
    {
      buffer_t* buf = bufObj->object;
      long currentTime = current_time();
      if(currentTime - buf->lastSentTime > currentState->rtt)
      {
          ctcp_segment_t *segment = (ctcp_segment_t*)(buf->data);
          if(buf->retryTime == 4)
          {
            shouldDestroy = true;
            break;
          }

          

          conn_send(currentState->conn, segment ,buf->len);

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
