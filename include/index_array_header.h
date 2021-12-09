/* globals that are mostly involved in parsing the Intel PT packets */

#ifndef INDEX_ARRAY_H
#define INDEX_ARRAY_H

#include <zlib.h>

struct tip_address_info {
  char *address;
  int  is_useful;
  int  ip_bytes;
};

struct fup_address_info {
  char *address;
  char type;  // a single byte to indicate whether this is a FUP associated interrupt or VMEXIT
};

struct intel_pt_execution_state {
  gzFile intel_pt_file;
  int divergence_count;
  char *last_tip_address;
  int tnt_index_limit;
  unsigned long long total_packets_consumed;
  unsigned long long number_of_lines_consumed;
};

extern unsigned long *tb_insn_array;

extern int is_within_block;

extern bool start_recording;

extern unsigned long long index_array;
extern unsigned long long prev_index_array;
//extern char **pip_cr3_values;

extern int stopped_execution_of_tb_chain;
extern int index_tip_address;
extern int index_fup_address;
extern int index_cr3_value;

extern int prev_index_tip_address;
extern int prev_index_fup_address;
extern int is_upcoming_page_fault;

extern int index_array_incremented;
extern int index_tip_address_incremented;

extern struct intel_pt_execution_state intel_pt_state;

/* I/O replay structures */
extern void *replay_tx_bh;
extern void *replay_ctrl_vq;
extern void *replay_ctrl_vdev;

void virtio_net_tx_replay(void *);
void virtio_net_handle_ctrl_replay(void *, void *);

unsigned long do_strtoul(char *address);   // function declared

extern uint64_t first_pc_of_tb;
extern uint64_t size_of_tb;

#endif
