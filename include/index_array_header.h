/* header file that contains the declaration of index_array global variable - being used in multiple files */

#ifndef INDEX_ARRAY_H
#define INDEX_ARRAY_H


#define FUP_TYPE_VMEXIT 1
#define FUP_TYPE_INTERRUPT 2

struct fup_address_info {
    char *address;
    int  type_of_fup;  // could be interrupt/vmexit
};

struct tip_address_info {
    char *address;
    int  is_useful;
    int  ip_bytes;
};

struct hash_buckets {
    int interrupt_number;   // is the value
    char *interrupt_handler_address;  // this is the key
    struct hash_buckets *pointer;
};

// hash table where the key is the interrupt handler function pointer and interrupt number is the value
extern struct hash_buckets *interrupt_hash_table;  
//
extern unsigned long *tb_insn_array;
extern unsigned int size_of_tb_insn_array;

extern unsigned long last_pc;
extern int is_nonio_vmexit;
extern int is_within_block;
extern char *tnt_array;
extern struct tip_address_info *tip_addresses;
extern struct fup_address_info *fup_addresses;
extern unsigned long long index_array;
// //extern char **pip_cr3_values;

extern unsigned long tb_jump_address;
extern int is_tnt_return;
extern int stopped_execution_of_tb_chain;
extern int index_tip_address;
extern int index_fup_address;
extern int index_cr3_value;

extern int index_array_incremented;
extern int index_tip_address_incremented;

extern int is_io_instruction;
extern int is_handle_interrupt_in_userspace;

unsigned long do_strtoul(char *address);   // function declared

#endif
