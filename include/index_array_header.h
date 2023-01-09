/* globals that are mostly involved in parsing the Intel PT packets */

#ifndef INDEX_ARRAY_H
#define INDEX_ARRAY_H

struct tip_address_info {
  char *address;
  int  is_useful;
  int  ip_bytes;
};

struct fup_address_info {
  char *address;
  char type;  // a single byte to indicate whether this is a FUP associated interrupt or VMEXIT
};

/*
 * it is rare with limited experiment time,
 * to have a TSC without a TMA. We ignore
 * its absence for now.
 */
struct tsc_val_meta {
  uint64_t tsc_value;
  // a marker that says if TSC value appears when guest is executing
  bool is_useful;
};

/* these are associated with per-cpu precomputed timer values */
extern unsigned long *precomputed_tsc_values_index;
/* keep only 'useful TSC values in the below array
 * we will eventually delete the array with both useful and non-useful values  */
extern unsigned long **useful_precomputed_tsc_values;

extern int is_within_block;

extern bool start_recording;

//extern char **pip_cr3_values;

extern int stopped_execution_of_tb_chain;
extern int is_upcoming_page_fault;

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
