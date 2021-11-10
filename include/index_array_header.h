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

struct hash_buckets {
  int interrupt_number;   // is the value
  char *interrupt_handler_address;  // this is the key
  struct hash_buckets *pointer;
};

struct intel_pt_execution_state {
  gzFile intel_pt_file;
  int tnt_index_limit;
  int fup_address_index_limit;
  int tip_address_index_limit;
  int divergence_count;
  bool is_simulation_finished;
  char *last_tip_address;
  unsigned long long total_packets_consumed;
  unsigned long long number_of_lines_consumed;
};

// hash table where the key is the interrupt handler 
// function pointer and interrupt number is the value
extern struct hash_buckets *interrupt_hash_table;  

extern unsigned long *tb_insn_array;

extern int is_within_block;

extern bool start_recording;

extern char *tnt_array;
extern struct tip_address_info *tip_addresses;
extern struct fup_address_info *fup_addresses;

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

extern int is_io_instruction;
extern int is_handle_interrupt_in_userspace;

void get_array_of_tnt_bits(void);

extern struct intel_pt_execution_state intel_pt_state;

/* I/O replay structures */
extern void *replay_tx_bh;
extern void *replay_ctrl_vq;
extern void *replay_ctrl_vdev;

void virtio_net_tx_replay(void *);
void virtio_net_handle_ctrl_replay(void *, void *);

unsigned long do_strtoul(char *address);   // function declared
int get_interrupt_number_from_hashtable(char *);

extern uint64_t first_pc_of_tb;
extern uint64_t size_of_tb;

#endif
