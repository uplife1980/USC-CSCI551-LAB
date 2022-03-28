#include "ctcp_sys.h"
#include "ctcp_utils.h"
typedef enum{
  STARTUP,
  DRAIN,
  PROBE_BW,
  PROBE_RTT
}bbr_phase_t;

typedef struct 
{
  int estimateRTT;
  int ackedDataCount;
  bool isRetried;
  bool app_limit;
  long timestamp;
  uint32_t packetInflight;
} ack_sample_t;

typedef struct bw_record
{
  long timestamp;
  uint32_t bw;
} bw_record_t;

typedef struct maxQueue
{
  bw_record_t sample[3];
} maxQueue_t;

typedef struct 
{
  bool round_start;
  //bool idle_restart;
  FILE *bdpFile;

  uint32_t inflightData;  
  long lastSentTime;

  //rtt
  bool have_gotten_rtt_sample;
  uint32_t min_rtt_ms;
  long min_rtt_timestamp;
  uint32_t time_to_stop_probe_rtt;
  bool probe_rtt_done;
  //uint32_t rtt_count;
  //uint32_t next_round_have_gotten_acked;

  //cycle, phase
  long cycle_timestamp;
  uint8_t cycle_index;

  bbr_phase_t current_phase;
  
  //bw
  maxQueue_t bw_sample_queue;
  bool reached_full_bw;
  uint32_t reached_full_bw_count;
  uint32_t full_bw; //recent full bw, used to determine a further larger bw

  //caller use
  double pacing_gain;
  double cwnd_gain;
  uint32_t current_cwnd;
  uint32_t prior_cwnd;

  uint32_t applimit_left;
    
  
  
} bbr_status_t;

void init_bbr(bbr_status_t *bbr, uint32_t min_rtt_estimate, uint16_t default_sendWindw);
void clean_bbr(bbr_status_t *bbr);

uint32_t bbr_thisTimeSend(bbr_status_t *bbr, bool shouldPrint);
void bbr_update(bbr_status_t*bbr, ack_sample_t* sample);
void bbr_sentNotice(bbr_status_t *bbr, uint32_t dataLen, bool is_app_limit);
void bbr_retransmission_notice(bbr_status_t *bbr);



