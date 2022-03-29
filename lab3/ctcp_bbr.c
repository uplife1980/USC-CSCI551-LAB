#include "ctcp_bbr.h"
static const double bbr_high_gain = 2.88;
static const double bbr_drain_gain = 1.0/2.88;
static const double bbr_cwnd_gain_for_probe_bw = 2.0;
static const double bbr_probe_bw_pacing_gain[] = {5.0/4.0, 3.0/4.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
static const int bbr_cycle_size = 8;

static const int bbr_keep_in_flight_packet = 4;
static const double bbr_full_bw_threshold = 1.25;
static const int bbr_full_bw_count = 5;

static const int bbr_bw_sample_expire_period_ms = 200;
static const int bbr_rtt_expire_period_ms = 10*1000;
static const int bbr_probe_rtt_lasting_time_ms = 100;
static const int bbr_alert_havenot_reached_full_bw_count = 10;
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

void debug_maxQueue(bbr_status_t *bbr, maxQueue_t* q)
{
  fprintf(bbr->bdpFile, "maxQueue: %ld-%d, %ld-%d, %ld-%d\n", q->sample[0].timestamp, q->sample[0].bw, q->sample[1].timestamp, q->sample[1].bw,q->sample[2].timestamp, q->sample[2].bw);
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

  if(rec.timestamp - q->sample[0].timestamp > bbr_bw_sample_expire_period_ms)
  {
    q->sample[0] = q->sample[1];
    q->sample[1] = q->sample[2];
    q->sample[2] = rec;
  }
  else if(rec.timestamp - q->sample[1].timestamp > bbr_bw_sample_expire_period_ms / 2)
  {
    q->sample[1] = q->sample[2];
    q->sample[2] = rec;
  }
  else if(rec.timestamp - q->sample[2].timestamp > bbr_bw_sample_expire_period_ms / 4)
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

  bbr->have_gotten_rtt_sample = false;
  //bbr->idle_restart = false;

  bbr->inflightData = 0;
  bbr->lastSentTime = currentTime;

  bbr->have_gotten_rtt_sample = false;
  bbr->min_rtt_ms = min_rtt_estimate;
  bbr->min_rtt_timestamp = currentTime;
  bbr->time_to_stop_probe_rtt = 0;
  bbr->probe_rtt_done = false;
  //bbr->rtt_count = 0;
  //bbr->next_round_have_gotten_acked = 1;

  bbr->cycle_timestamp = currentTime;
  bbr->cycle_index = 0;
  bbr->current_phase = STARTUP;

  
  bbr->reached_full_bw = false;
  bbr->reached_full_bw_count = 0;
  bbr->alert_cannot_reach_full_bw_count = 0;
  

  bbr->pacing_gain = bbr_high_gain;
  bbr->cwnd_gain = bbr_high_gain;
  bbr->current_cwnd = default_sendWindw;
  bbr->prior_cwnd = default_sendWindw;

  bbr->full_bw = bbr->current_cwnd * 1000 / bbr->min_rtt_ms;
  bw_record_t bw_record = {
    .bw = bbr->full_bw,
    .timestamp = currentTime
  };
  reset_maxQueue(&(bbr->bw_sample_queue), bw_record);

  bbr->applimit_left = 0;

}

void clean_bbr(bbr_status_t *bbr)
{
  if(!bbr)
    return;
  
  fclose(bbr->bdpFile);
}

void update_bw(bbr_status_t*bbr, ack_sample_t* sample)
{
  //record the lastest sample bw, but don't update full bw: we will do that in update_check_full_bw()

  uint32_t bw = sample->ackedDataCount *1000 / sample->estimateRTT;
  bbr->round_start = 1;
  bbr->inflightData -= sample->ackedDataCountTotal;

  if(!sample->app_limit || bw >= get_max_maxQueue(&(bbr->bw_sample_queue)) )
  {
    bw_record_t record = {
    .bw = bw,
    .timestamp = sample->timestamp
    };

    insert_data_maxQueue(&(bbr->bw_sample_queue), record);
  }
  #ifdef DEBUG
  fprintf(bbr->bdpFile, "%ld: Current bw: %d, current rtt: %d\n",sample->timestamp,  bw, sample->estimateRTT);
  debug_maxQueue(bbr, &bbr->bw_sample_queue);
  #endif
}

uint32_t calculate_bdp(bbr_status_t *bbr)
{
  return bbr->min_rtt_ms *bbr->full_bw / 1000;
}

bool should_shift_to_next_cycle(bbr_status_t *bbr, ack_sample_t* sample)
{
  bool has_waited_for_enough_time = (sample->timestamp - bbr->cycle_timestamp >= bbr->min_rtt_ms);

  if(bbr->pacing_gain == 1.0)
    return has_waited_for_enough_time;

  else if(bbr->pacing_gain > 1.0)
  {
    return has_waited_for_enough_time && (bbr->inflightData >= calculate_bdp(bbr) * bbr->pacing_gain);
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
    bbr->cycle_index = (bbr->cycle_index + 1 ) % bbr_cycle_size;
    bbr->cycle_timestamp = current_time();
  }
}

void update_check_bw_full(bbr_status_t *bbr, ack_sample_t* sample)
{
  if(!bbr->round_start || sample->app_limit)
    return;
  
  // //sometimes mininext will overestimate the bw, so we need to wipe out that sample.
  // if(bbr->reached_full_bw && bbr->current_phase == PROBE_BW)
  // {
  //   if(bbr->full_bw > get_max_maxQueue(&(bbr->bw_sample_queue)) * 1.2)
  //   {
  //     bbr->alert_cannot_reach_full_bw_count ++;
  //   }
  //   else
  //   {
  //     bbr->alert_cannot_reach_full_bw_count = 0;
  //   }

  //   if(bbr->alert_cannot_reach_full_bw_count == bbr_alert_havenot_reached_full_bw_count)
  //   {
  //     bbr->reached_full_bw = false;
  //     bbr->reached_full_bw_count = 0;
  //     bbr->alert_cannot_reach_full_bw_count = 0;
  //     reset_phase(bbr);
  //   }
  // }
  else if(bbr->reached_full_bw)
  {
    return;
  }

  uint32_t bw_expect = bbr->full_bw * bbr_full_bw_threshold;
  uint32_t bw = sample->ackedDataCount *1000 / sample->estimateRTT;

  if(get_max_maxQueue(&(bbr->bw_sample_queue)) > bw_expect)
  {
    //we can expect more
    bbr->full_bw = get_max_maxQueue(&(bbr->bw_sample_queue));
    bbr->reached_full_bw_count = 0;
    bbr->alert_cannot_reach_full_bw_count = 0;
    return;
  }
  else
  {
    if(++bbr->reached_full_bw_count == bbr_full_bw_count)
    {
      bbr->reached_full_bw = true; //Once we reached, we never reset again.(Unless in mininext's overestimation)
      #ifdef DEBUG
      fprintf(bbr->bdpFile, "full_bw found: %d\n", bbr->full_bw);
      #endif
    }
    else if(bw * 1.5 < bbr->full_bw)
    {
      //sometimes mininext will overestimate the bw, so we need to wipe out that sample.
      bw_record_t record = {
        .bw = bw,
        .timestamp = sample->timestamp
      };
      reset_maxQueue(&(bbr->bw_sample_queue), record);
      bbr->full_bw = bw;
      bbr->reached_full_bw_count = 0;
      //clear the last "fake" data, and check bw_full again
      update_check_bw_full(bbr, sample);
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
    if(bbr->inflightData < calculate_bdp(bbr) * bbr->pacing_gain)
    {
      shift_to_probe_bw(bbr);
    }
  }

  if(bbr->current_phase == PROBE_BW && sample->estimateRTT > 7 * bbr->min_rtt_ms) 
  {
    bbr->current_phase = DRAIN;
  }
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
  }

  if(isExpired && bbr->current_phase != PROBE_RTT)
  {
    bbr->current_phase = PROBE_RTT;
    bbr->prior_cwnd = bbr->current_cwnd;
    bbr->probe_rtt_done = false;
    bbr->time_to_stop_probe_rtt = 0;
  }

  if(bbr->current_phase == PROBE_RTT)
  {
    bbr->applimit_left = bbr->inflightData? bbr->inflightData:1;

    if(!bbr->time_to_stop_probe_rtt && sample->packetInflight <= bbr_keep_in_flight_packet)
    {
      //make sure we don't push too many packet into the pipe.
      bbr->time_to_stop_probe_rtt = sample->timestamp + bbr_probe_rtt_lasting_time_ms;
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

uint32_t get_current_pacing(bbr_status_t *bbr, ack_sample_t* sample)
{
  if(!bbr->have_gotten_rtt_sample)
  {
    return bbr->current_cwnd *1000 / bbr->min_rtt_ms;
  }

  return bbr->full_bw * bbr->pacing_gain;
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

  bbr->current_cwnd += sample->ackedDataCountTotal;
  

  if(bbr->current_phase == PROBE_RTT)
  {
    bbr->current_cwnd = 4;
  }
}

void bbr_update(bbr_status_t*bbr, ack_sample_t* sample)
{
  update_bw(bbr, sample);
  update_cycle(bbr, sample);
  update_check_bw_full(bbr, sample);
  update_check_drain(bbr, sample);
  update_min_rtt(bbr, sample);
  update_gain(bbr, sample);
  //update_pacing(bbr, sample);
  update_cwnd(bbr, sample);

}

void bbr_retransmission_notice(bbr_status_t *bbr)
{
  bbr->current_cwnd = 4;
  fprintf(bbr->bdpFile, "retransmission occured \n");
  fflush(bbr->bdpFile);
}

void printBDP(bbr_status_t *bbr)
{
    long currentTime = current_time();
    fprintf(bbr->bdpFile, "%ld, ", currentTime);
    #ifdef DEBUG
    fprintf(bbr->bdpFile, "%lf, %d, %d <-> %d, %d, ",bbr->pacing_gain, bbr->min_rtt_ms, bbr->full_bw,bbr->inflightData, bbr->current_cwnd);  
    #endif
    uint32_t bdp = calculate_bdp(bbr);
    fprintf(bbr->bdpFile, "%d\n",bdp*8);
    
    fflush(bbr->bdpFile);
    
}

uint32_t bbr_thisTimeSendPacing(bbr_status_t *bbr, bool shouldPrint)
{
  long currentTime = current_time();
  long time_dt_ms = currentTime - bbr->lastSentTime;
  uint32_t dataFromPacing = time_dt_ms * bbr->full_bw * bbr->pacing_gain/ 1000;
  if(shouldPrint)
    printBDP(bbr);

  bbr->lastSentTime = currentTime;
  return dataFromPacing;
  
}

uint32_t bbr_thisTimeSendCwnd(bbr_status_t *bbr)
{
  return bbr->current_cwnd;
}

void bbr_sentNotice(bbr_status_t *bbr, int *limit_by_what)
{
  #ifdef DEBUG
  int i = 0;
  for(; i < 6; i++)
  {
    fprintf(bbr->bdpFile, "%d, " , limit_by_what[i]);
  }
  if(limit_by_what[0] == limit_by_what[1])
  {
    fprintf(bbr->bdpFile, "[limit: NO]" );
  }
  else if(limit_by_what[0] == limit_by_what[2])
  {
    fprintf(bbr->bdpFile, "[limit: cwnd]");
  }
  else if(limit_by_what[0] == limit_by_what[3])
  {
    fprintf(bbr->bdpFile, "[limit: data]");
  }
  else if(limit_by_what[0] == limit_by_what[4])
  {
    fprintf(bbr->bdpFile, "[limit: MSS]");
  }
  else if(limit_by_what[0] == limit_by_what[5])
  {
    fprintf(bbr->bdpFile, "[limit: sendWindow]");
  }


  bbr->inflightData += limit_by_what[0];

  fprintf(bbr->bdpFile, " After send inflight: %d\n", bbr->inflightData);
    
  fflush(bbr->bdpFile);
  #endif

}


