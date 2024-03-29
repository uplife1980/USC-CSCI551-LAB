#include "ctcp_bbr.h"
static const double bbr_high_gain = 2.88;
static const double bbr_drain_gain = 1.0/2.88;
static const double bbr_cwnd_gain_for_probe_bw = 2.0;
static const double bbr_probe_bw_pacing_gain[] = { 5.0/4.0, 3.0/4.0, 1, 1, 1, 1, 1, 1};
static const int bbr_cycle_size = 8;

static const int bbr_keep_in_flight_packet = 4;
static const double bbr_full_bw_threshold = 1.25;
static const int bbr_full_bw_count = 5;
static const double bbr_inflight_over_bdp_threshold = 1.1;

static const double bbr_min_rtt_threashold_weight = 1.75;
static const int bbr_bw_sample_expire_period_round = 10;
static const int bbr_rtt_expire_period_ms = 30*1000;
static const int bbr_probe_rtt_lasting_time_ms = 200;
#define DEBUG

void reset_phase(bbr_status_t *bbr);


uint32_t get_max_maxQueue(maxQueue_t* q)
{
  return q->sample[0].bw;
}

uint32_t reset_maxQueue(maxQueue_t* q, bw_record_t rec)
{
  q->sample[0] = rec;
  q->sample[1] = q->sample[2] = q->sample[0];
  return q->sample[0].bw;
}

//need repair in the future (https://code.woboq.org/linux/linux/lib/win_minmax.c.html#minmax_running_max)
uint32_t insert_data_maxQueue(maxQueue_t *q, bw_record_t rec)
{
  if(rec.bw > q->sample[0].bw)
  {
    reset_maxQueue(q, rec);
  }
  else if (q->sample[0].timestamp == q->sample[1].timestamp || rec.bw >= q->sample[1].bw)
  {
    //only one sample here, or a 2nd biggest new bw shows
    q->sample[1] = q->sample[2] = rec;
  }
  else if(q->sample[2].timestamp == q->sample[1].timestamp || rec.bw >= q->sample[2].bw)
  {
    //only two samples here
     q->sample[2] = rec;
  }

  if(rec.timestamp - q->sample[0].timestamp > bbr_bw_sample_expire_period_round)
  {
    q->sample[0] = q->sample[1];
    q->sample[1] = q->sample[2];
    q->sample[2] = rec;
  }
  else if(rec.timestamp - q->sample[1].timestamp > bbr_bw_sample_expire_period_round / 2)
  {
    q->sample[1] = q->sample[2];
    q->sample[2] = rec;
  }
  else if(rec.timestamp - q->sample[2].timestamp > bbr_bw_sample_expire_period_round / 4)
  {
    q->sample[2] = rec;
  }
  
  
 

  return q->sample[0].bw;
}



void init_bbr(bbr_status_t *bbr, uint32_t min_rtt_estimate, uint16_t default_sendWindw)
{
  long currentTime = current_time();

  if(!bbr)
    return;
  //file
  char temp[255]="random";
  srand((unsigned int)currentTime);
  sprintf(temp+6, "%p", bbr);
  bbr->bdpFile = fopen(temp, "a+");
  #ifdef DEBUG
  char debug[255] = "debug";
  sprintf(debug+5, "%p", bbr);
  bbr->debugFile = fopen(debug, "a+");
  #endif

  bbr->have_gotten_rtt_sample = false;
  //bbr->idle_restart = false;

  bbr->inflightData = 0;
  bbr->lastSentTime = currentTime;

  bbr->have_gotten_rtt_sample = false;
  bbr->min_rtt_ms = min_rtt_estimate;
  bbr->min_rtt_timestamp = currentTime;
  bbr->time_to_stop_probe_rtt = 0;
  bbr->probe_rtt_done = false;
  bbr->rtt_count = 0;
  //bbr->next_round_have_gotten_acked = 1;

  bbr->cycle_timestamp = currentTime;
  bbr->cycle_index = rand();
  bbr->current_phase = STARTUP;

  
  bbr->reached_full_bw = false;
  bbr->reached_full_bw_count = 0;
  

  bbr->pacing_gain = bbr_high_gain;
  bbr->cwnd_gain = bbr_high_gain;
  bbr->current_cwnd = default_sendWindw;
  bbr->prior_cwnd = default_sendWindw;

  bbr->full_bw = bbr->current_cwnd * 1000 / bbr->min_rtt_ms;
  bw_record_t bw_record = {
    .bw = bbr->full_bw,
    .timestamp = ++bbr->rtt_count
  };
  reset_maxQueue(&(bbr->bw_sample_queue), bw_record);

  bbr->congestion_limit_left = 0;

}

void clean_bbr(bbr_status_t *bbr)
{
  if(!bbr)
    return;
  
  fclose(bbr->bdpFile);
  #ifdef DEBUG
  fclose(bbr->debugFile);
  #endif
}

int min_rtt_threashold(bbr_status_t*bbr)
{
  if(bbr->min_rtt_ms < 10)
  {
    return 10;
  }
  else
  {
    return bbr->min_rtt_ms;
  }
}

void update_congestion(bbr_status_t*bbr, ack_sample_t* sample)
{
  if(bbr->congestion_limit_left)
  {
    if(sample->ackedDataCountReal >= bbr->congestion_limit_left)
    {
      bbr->congestion_limit_left = 0;
      bbr->current_cwnd = bbr->prior_cwnd;
      bw_record_t record = {
        .bw = bbr->prior_bw,
        .timestamp = ++bbr->rtt_count
      };
      insert_data_maxQueue(&(bbr->bw_sample_queue), record);
      #ifdef DEBUG
      fprintf(bbr->debugFile,"[state] exit congestion mode, reset to bw=%d \n", record.bw);
      fflush(bbr->debugFile);
      #endif
    }
    else
    {
      bbr->congestion_limit_left -= sample->ackedDataCountReal;
      #ifdef DEBUG
      fprintf(bbr->debugFile,"congestion left %d \n", bbr->congestion_limit_left);
      fflush(bbr->debugFile);
      #endif
    }
  }
}
void update_bw(bbr_status_t*bbr, ack_sample_t* sample)
{
  bbr->round_start = 0;
  //record the lastest sample bw, but don't update full bw: we will do that in update_check_full_bw()
  
  bbr->inflightData -= sample->ackedDataCountReal;
  if(sample->isRetried)
  {
    return;
  }

  uint32_t bw = sample->ackedDataCountFromBuffer *1000 / sample->estimateRTT;
  bbr->round_start = 1;

  if(!sample->app_limit || bw >= get_max_maxQueue(&(bbr->bw_sample_queue)) )
  {
    bw_record_t record = {
    .bw = bw,
    .timestamp = ++bbr->rtt_count
    };

    insert_data_maxQueue(&(bbr->bw_sample_queue), record);
  }
  #ifdef DEBUG
  fprintf(bbr->debugFile, "%ld: Current bw: %d, current rtt: %d, current inflight : %d\n",sample->timestamp,  bw, sample->estimateRTT, bbr->inflightData);
  fflush(bbr->debugFile);
  //debug_maxQueue(bbr, &bbr->bw_sample_queue);
  #endif
}

uint32_t calculate_bdp(bbr_status_t *bbr)
{
  return bbr->min_rtt_ms * get_max_maxQueue(&(bbr->bw_sample_queue)) / 1000;
}

bool should_shift_to_next_cycle(bbr_status_t *bbr, ack_sample_t* sample)
{
  int circle_max_period = min_rtt_threashold(bbr) > 30 ? 30:min_rtt_threashold(bbr);
  bool has_waited_for_enough_time = (sample->timestamp - bbr->cycle_timestamp >= circle_max_period);

  if(bbr->pacing_gain == 1)
    return has_waited_for_enough_time;

  else if(bbr->pacing_gain > 1.0)
  {
    return (has_waited_for_enough_time && (bbr->inflightData >= calculate_bdp(bbr) * bbr->pacing_gain)) || sample->isRetried;
  }
  else 
  {
    //bbr->pacing_gain < 1.0
    //because we are trying to drain the pipe, check if we achieve that goal
    return has_waited_for_enough_time || bbr->inflightData < calculate_bdp(bbr) * bbr->pacing_gain;
  }
}

void update_cycle(bbr_status_t *bbr, ack_sample_t* sample)
{
  if(bbr->current_phase != PROBE_BW)
    return;

  if(should_shift_to_next_cycle(bbr, sample))
  {
    //shift to next cycle
    bbr->cycle_index = (bbr->cycle_index + 1 ) & bbr_cycle_size;
    bbr->cycle_timestamp = sample->timestamp;
  }
}

void update_check_bw_full(bbr_status_t *bbr, ack_sample_t* sample)
{
  if(!bbr->round_start || sample->app_limit||bbr->reached_full_bw)
    return;
  
  //uint32_t bw_expect = bbr->full_bw * bbr_full_bw_threshold;
  //uint32_t bw = sample->ackedDataCountTotal *1000 / sample->estimateRTT;

  //if(get_max_maxQueue(&(bbr->bw_sample_queue)) > bw_expect)
  if(!bbr->have_gotten_rtt_sample)
  {
    return;
  }


  if(sample->estimateRTT > min_rtt_threashold(bbr) * bbr_full_bw_threshold + 1)
  {
    //rtt is going up, we can collect bw data
    if(get_max_maxQueue(&(bbr->bw_sample_queue)) > bbr->full_bw )
    {
      //we can expect more
      bbr->full_bw = get_max_maxQueue(&(bbr->bw_sample_queue)) * bbr_full_bw_threshold;
      bbr->reached_full_bw_count = 0;
      return;
    }

    else if(!bbr->reached_full_bw)
    {
      if(++bbr->reached_full_bw_count == bbr_full_bw_count)
      {
        bbr->reached_full_bw = true; //Once we reached, we never reset again.(Unless in mininext's overestimation)
        #ifdef DEBUG
        fprintf(bbr->debugFile, "[state] full_bw found: %lf\n", bbr->full_bw/bbr_full_bw_threshold);
        fflush(bbr->debugFile);
        #endif
      }
    }
  
    return;
  }  
}

void shift_to_probe_bw(bbr_status_t *bbr)
{
  bbr->current_phase = PROBE_BW;
  bbr->cycle_index = rand() % bbr_cycle_size;
  bbr->cycle_timestamp = current_time();
}

void shift_to_start_up(bbr_status_t *bbr)
{
  bbr->current_phase = STARTUP;
}

void reset_phase(bbr_status_t *bbr)
{
  if(!bbr->reached_full_bw)
    shift_to_start_up(bbr);
  else
    shift_to_probe_bw(bbr);
}

void update_check_drain(bbr_status_t *bbr, ack_sample_t* sample)
{
  if(bbr->current_phase == STARTUP && bbr->reached_full_bw)
  {
    bbr->current_phase = DRAIN;
  } 
  if(bbr->current_phase == DRAIN)
  {
    if(bbr->inflightData < calculate_bdp(bbr))
    {
      shift_to_probe_bw(bbr);
    }
  }

  // if(bbr->current_phase == PROBE_BW && sample->estimateRTT > bbr_min_rtt_threashold_weight * min_rtt_threashold(bbr)) 
  // {
  //   bbr->current_phase = DRAIN;
  // }
}

void try_finish_probe_rtt(bbr_status_t *bbr, ack_sample_t* sample)
{
  if(bbr->time_to_stop_probe_rtt && sample->timestamp > bbr->time_to_stop_probe_rtt)
  {
    bbr->min_rtt_timestamp = sample->timestamp;
    bbr->current_cwnd = bbr->current_cwnd > bbr->prior_cwnd? bbr->current_cwnd :  bbr->prior_cwnd;
    reset_phase(bbr);
  }

}

void update_min_rtt(bbr_status_t *bbr, ack_sample_t* sample)
{
  bool isExpired = (sample->timestamp - bbr->min_rtt_timestamp > bbr_rtt_expire_period_ms);
  
  if(sample->estimateRTT < bbr->min_rtt_ms || isExpired)
  {
    bbr->min_rtt_ms = sample->estimateRTT;
    bbr->min_rtt_timestamp = sample->timestamp;
    bbr->have_gotten_rtt_sample = true;
  }

  if(isExpired && bbr->current_phase != PROBE_RTT)
  {
    bbr->current_phase = PROBE_RTT;
    bbr->have_gotten_rtt_sample = true;
    bbr->prior_cwnd = bbr->current_cwnd;
    bbr->probe_rtt_done = false;
    bbr->time_to_stop_probe_rtt = 0;
  }

  if(bbr->current_phase == PROBE_RTT)
  {

    if(!bbr->time_to_stop_probe_rtt && bbr->inflightData <= bbr_keep_in_flight_packet * 2 * sample->estimateRTT)
    {
      //make sure we don't push too many packet into the pipe.
      bbr->time_to_stop_probe_rtt = (bbr_probe_rtt_lasting_time_ms > sample->estimateRTT ? bbr_probe_rtt_lasting_time_ms:sample->estimateRTT) + sample->timestamp;
      bbr->probe_rtt_done = false;
    }
    else if(bbr->time_to_stop_probe_rtt)
    {
      if(bbr->round_start)
        bbr->probe_rtt_done = true;
      
      if(bbr->probe_rtt_done)
        try_finish_probe_rtt(bbr, sample);
    }
  }
}

void update_gain(bbr_status_t *bbr, ack_sample_t* sample)
{
  switch(bbr->current_phase)
  {
    case STARTUP:
      bbr->pacing_gain = bbr_high_gain;
      bbr->cwnd_gain = bbr_high_gain;
      break;

    case DRAIN:
      bbr->pacing_gain = bbr_drain_gain;
      bbr->cwnd_gain = bbr_drain_gain;
      break;

    case PROBE_BW:
      bbr->pacing_gain = bbr_probe_bw_pacing_gain[bbr->cycle_index];
      bbr->cwnd_gain = bbr_cwnd_gain_for_probe_bw;
      break;

    case PROBE_RTT:
      bbr->pacing_gain = 1.0;
      bbr->cwnd_gain = 1.0;
      break;
  }   
  

}

void update_cwnd(bbr_status_t *bbr, ack_sample_t* sample)
{
  uint32_t expected_cwnd = calculate_bdp(bbr) * bbr->cwnd_gain;

  if(bbr->reached_full_bw)
  {
    bbr->current_cwnd = bbr->current_cwnd > expected_cwnd? expected_cwnd : bbr->current_cwnd; //use the smaller one 
  }
  else
  {
    bbr->current_cwnd = expected_cwnd;
  }

  bbr->current_cwnd += sample->ackedDataCountReal;
  

  if(bbr->current_phase == PROBE_RTT)
  {
    bbr->current_cwnd = 4;
  }
}

void bbr_update(bbr_status_t*bbr, ack_sample_t* sample)
{
  update_congestion(bbr, sample);
  update_bw(bbr, sample);

  if(sample->isRetried == false)
  { 
    update_cycle(bbr, sample);
    update_check_bw_full(bbr, sample);
    update_check_drain(bbr, sample);
    update_min_rtt(bbr, sample);
    update_gain(bbr, sample);
    update_cwnd(bbr, sample);
  }
  
}

void bbr_retransmission_notice(bbr_status_t *bbr, long currentTime)
{
  //bbr->current_cwnd = 4;
  bbr->lastSentTime = currentTime;
  
  if(!bbr->congestion_limit_left)
  {
    bbr->congestion_limit_left = bbr->inflightData;
    bbr->prior_bw = get_max_maxQueue(&(bbr->bw_sample_queue));
    bbr->prior_cwnd = bbr->current_cwnd;
    bbr->current_cwnd = 4;
    #ifdef DEBUG
    fprintf(bbr->debugFile, "[state] enter congestion mode\n");
    fflush(bbr->debugFile);
    #endif
  }
  
  
  // bbr->reached_full_bw = false;
  // bbr->full_bw = get_max_maxQueue(&(bbr->bw_sample_queue))/4;
  // reset_phase(bbr);

  #ifdef DEBUG
  fprintf(bbr->debugFile, "retransmission occured, size %d \n", bbr->congestion_limit_left);
  fflush(bbr->debugFile);
  #endif
}

void printBDP(bbr_status_t *bbr, long currentTime)
{
    fprintf(bbr->bdpFile, "%ld, ", currentTime);

    #ifdef DEBUG
    fprintf(bbr->debugFile, "%ld, ", currentTime);
    fprintf(bbr->debugFile, "%lf,%d,  %d <->  ",bbr->pacing_gain,bbr->min_rtt_ms, bbr->inflightData*8);  
    #endif
    uint32_t bdp = calculate_bdp(bbr);
    fprintf(bbr->bdpFile, "%d\n",bdp);
    
    #ifdef DEBUG
    fprintf(bbr->debugFile, "%d\n",bdp*8);
    fflush(bbr->debugFile);
    #endif

    fflush(bbr->bdpFile);
    
}

uint32_t bbr_thisTimeSendPacing(bbr_status_t *bbr, bool shouldPrint, long currentTime)
{
  long time_dt_ms = currentTime - bbr->lastSentTime;
  if(bbr->congestion_limit_left && bbr->inflightData > calculate_bdp(bbr) * bbr_inflight_over_bdp_threshold)
  {
    return 0;
  }
  
  uint32_t current_bw = get_max_maxQueue(&(bbr->bw_sample_queue));
  uint32_t dataFromPacing = time_dt_ms * current_bw * bbr->pacing_gain/ 1000;

  
  // if(bbr->inflightData >= calculate_bdp(bbr) * bbr->pacing_gain)
  // {
  //   return 0;
  // }

  // if(dataFromPacing + bbr->inflightData > calculate_bdp(bbr) * bbr->pacing_gain)
  // {
  //   dataFromPacing = calculate_bdp(bbr) * bbr->pacing_gain - bbr->inflightData;
  // }


  return dataFromPacing;
  
}

uint32_t bbr_thisTimeSendCwnd(bbr_status_t *bbr)
{
  return bbr->current_cwnd;
}

//[0:actual data, 1:pacing, 2:cwnd, 3: app data, 4: MSS, 5:send window]
void bbr_sentNotice(bbr_status_t *bbr, int *limit_by_what, long currentTime)
{

  bbr->inflightData += limit_by_what[0];
  
  if(limit_by_what[0] != 0)
  {
    printBDP(bbr, currentTime);

    #ifdef DEBUG
    long time_dt_ms = currentTime - bbr->lastSentTime;
    uint32_t current_bw = get_max_maxQueue(&(bbr->bw_sample_queue));
    uint32_t dataFromPacing = time_dt_ms * current_bw * bbr->pacing_gain/ 1000;
    fprintf(bbr->debugFile, "lastTime: %ld, thisTime: %ld, send: %ld*%d*%lf=%d\n",bbr->lastSentTime, currentTime, time_dt_ms, current_bw , bbr->pacing_gain ,dataFromPacing);
    fprintf(bbr->debugFile, "[data Limit] ");
    int i = 0;
    for(; i < 6; i++)
    {
      fprintf(bbr->debugFile, "%d, " , limit_by_what[i]);
    }
    if(limit_by_what[0] == limit_by_what[1])
    {
      fprintf(bbr->debugFile, "[limit: NO]" );
    }
    else if(limit_by_what[0] == limit_by_what[2])
    {
      fprintf(bbr->debugFile, "[limit: cwnd]");
    }
    else if(limit_by_what[0] == limit_by_what[3])
    {
      fprintf(bbr->debugFile, "[limit: data]");
    }
    else if(limit_by_what[0] == limit_by_what[4])
    {
      fprintf(bbr->debugFile, "[limit: MSS]");
    }
    else if(limit_by_what[0] == limit_by_what[5])
    {
      fprintf(bbr->debugFile, "[limit: sendWindow]");
    }

    fprintf(bbr->debugFile, " After send inflight: %d\n", bbr->inflightData);
    
    fflush(bbr->debugFile);
    #endif
  }

  bbr->lastSentTime = currentTime;
  

}


