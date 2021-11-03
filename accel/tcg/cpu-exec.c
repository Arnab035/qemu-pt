/*
 *  emulator main execution loop
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "cpu.h"
#include "trace.h"
#include "disas/disas.h"
#include "exec/exec-all.h"
#include "tcg/tcg.h"
#include "qemu/atomic.h"
#include "sysemu/qtest.h"
#include "qemu/timer.h"

#include "qemu/rcu.h"
#include "exec/tb-hash.h"
#include "exec/tb-lookup.h"
#include "exec/log.h"
#include "qemu/main-loop.h"
#if defined(TARGET_I386) && !defined(CONFIG_USER_ONLY)
#include "hw/i386/apic.h"
#endif
#include "sysemu/cpus.h"
#include "sysemu/replay.h"
#include "index_array_header.h"
#include "hw/ide/ahci.h"

/* added header files to handle gzlib files */
#include <zlib.h>
#include <errno.h>

// use below includes for replay functions
#include "replay/replay-internal.h"
#include "events.h"
#include "qemu/iov.h"

/*
#ifdef CONFIG_USER_ONLY
#define MEMSUFFIX _kernel
#define DATA_SIZE 1
#include "exec/cpu_ldst_useronly_template.h"

#define DATA_SIZE 2
#include "exec/cpu_ldst_useronly_template.h"

#define DATA_SIZE 4
#include "exec/cpu_ldst_template.h"

#define DATA_SIZE 8
#include "exec/cpu_ldst_template.h"
#undef MEMSUFFIX
#else
#define CPU_MMU_INDEX (cpu_mmu_index_kernel(env))
#define MEMSUFFIX _kernel
#define DATA_SIZE 1
#include "exec/cpu_ldst_template.h"

#define DATA_SIZE 2
#include "exec/cpu_ldst_template.h"

#define DATA_SIZE 4
#include "exec/cpu_ldst_template.h"

#define DATA_SIZE 8
#include "exec/cpu_ldst_template.h"
#undef CPU_MMU_INDEX
#undef MEMSUFFIX
#endif
*/

/* definition of tb_insn_array */

int stopped_execution_of_tb_chain = 0;

struct hash_buckets *interrupt_hash_table = NULL;

/* -icount align implementation. */

typedef struct SyncClocks {
    int64_t diff_clk;
    int64_t last_cpu_icount;
    int64_t realtime_clock;
} SyncClocks;

#if !defined(CONFIG_USER_ONLY)
/* Allow the guest to have a max 3ms advance.
 * The difference between the 2 clocks could therefore
 * oscillate around 0.
 */
#define VM_CLOCK_ADVANCE 3000000
#define THRESHOLD_REDUCE 1.5
#define MAX_DELAY_PRINT_RATE 2000000000LL
#define MAX_NB_PRINTS 100

#define LENGTH  0x1000

struct tip_address_info *tip_addresses = NULL;
struct fup_address_info *fup_addresses = NULL;
//char **pip_cr3_values = NULL;

static void align_clocks(SyncClocks *sc, CPUState *cpu)
{
    int64_t cpu_icount;

    if (!icount_align_option) {
        return;
    }

    cpu_icount = cpu->icount_extra + cpu_neg(cpu)->icount_decr.u16.low;
    sc->diff_clk += cpu_icount_to_ns(sc->last_cpu_icount - cpu_icount);
    sc->last_cpu_icount = cpu_icount;

    if (sc->diff_clk > VM_CLOCK_ADVANCE) {
#ifndef _WIN32
        struct timespec sleep_delay, rem_delay;
        sleep_delay.tv_sec = sc->diff_clk / 1000000000LL;
        sleep_delay.tv_nsec = sc->diff_clk % 1000000000LL;
        if (nanosleep(&sleep_delay, &rem_delay) < 0) {
            sc->diff_clk = rem_delay.tv_sec * 1000000000LL + rem_delay.tv_nsec;
        } else {
            sc->diff_clk = 0;
        }
#else
        Sleep(sc->diff_clk / SCALE_MS);
        sc->diff_clk = 0;
#endif
    }
}

static void print_delay(const SyncClocks *sc)
{
    static float threshold_delay;
    static int64_t last_realtime_clock;
    static int nb_prints;

    if (icount_align_option &&
        sc->realtime_clock - last_realtime_clock >= MAX_DELAY_PRINT_RATE &&
        nb_prints < MAX_NB_PRINTS) {
        if ((-sc->diff_clk / (float)1000000000LL > threshold_delay) ||
            (-sc->diff_clk / (float)1000000000LL <
             (threshold_delay - THRESHOLD_REDUCE))) {
            threshold_delay = (-sc->diff_clk / 1000000000LL) + 1;
            printf("Warning: The guest is now late by %.1f to %.1f seconds\n",
                   threshold_delay - 1,
                   threshold_delay);
            nb_prints++;
            last_realtime_clock = sc->realtime_clock;
        }
    }
}

static void init_delay_params(SyncClocks *sc, CPUState *cpu)
{
    if (!icount_align_option) {
        return;
    }
    sc->realtime_clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
    sc->diff_clk = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - sc->realtime_clock;
    sc->last_cpu_icount
        = cpu->icount_extra + cpu_neg(cpu)->icount_decr.u16.low;
    if (sc->diff_clk < max_delay) {
        max_delay = sc->diff_clk;
    }
    if (sc->diff_clk > max_advance) {
        max_advance = sc->diff_clk;
    }

    /* Print every 2s max if the guest is late. We limit the number
       of printed messages to NB_PRINT_MAX(currently 100) */
    print_delay(sc);
}
#else
static void align_clocks(SyncClocks *sc, const CPUState *cpu)
{
}

static void init_delay_params(SyncClocks *sc, const CPUState *cpu)
{
}
#endif /* CONFIG USER ONLY */

/* preprocess_tip_array - preprocesses the tip array so that truncated addresses contain the fully-qualified address */

static void construct_fully_qualified_address(int i, char *reference_address) {
    int j, chars_to_copy = 0;
    int short_length = 0;

    if(tip_addresses[i].ip_bytes==4) {   // some other value
      //chars_to_copy=12-strlen(tip_addresses[i].address);
        chars_to_copy=strlen(reference_address)-strlen(tip_addresses[i].address);
        if(chars_to_copy < 0) {
            chars_to_copy=0;
        }

        //short_length=8-strlen(tip_addresses[i].address);
        tip_addresses[i].address=realloc(tip_addresses[i].address,13);

    
        for(j=strlen(tip_addresses[i].address)-1; j>=0; j--) {
            tip_addresses[i].address[j+chars_to_copy]=tip_addresses[i].address[j];
        }
        if (chars_to_copy > 4) {
            for(j=0; j<chars_to_copy-4; j++) {
                tip_addresses[i].address[j+4] = '0';
            }
            chars_to_copy = 4;
        }
        for(j=0;j<chars_to_copy;j++) {
            tip_addresses[i].address[j] = reference_address[j];
        }
        tip_addresses[i].address[12]='\0';
    }
    else if(tip_addresses[i].ip_bytes==2) {
        if(strlen(reference_address)==6) {
            if(strlen(tip_addresses[i].address) < 4) {
                short_length = 4-strlen(tip_addresses[i].address);
                chars_to_copy=strlen(reference_address)-strlen(tip_addresses[i].address)-short_length;
                if(chars_to_copy<0) {
                    chars_to_copy=0;
                }
            }
            else if(strlen(tip_addresses[i].address)==4) {
                short_length=0;
                chars_to_copy=strlen(reference_address)-strlen(tip_addresses[i].address);
                if(chars_to_copy<0) {
                    chars_to_copy=0;
                }
            }
        }
        else {
            if(strlen(tip_addresses[i].address) < 4) {
                short_length = 4-strlen(tip_addresses[i].address);
                chars_to_copy = strlen(reference_address)-strlen(tip_addresses[i].address)-short_length;
                if(chars_to_copy<0) {
                    chars_to_copy=0;
                }
            }
            else if(strlen(tip_addresses[i].address)==4) {
                short_length = 0;
                chars_to_copy=strlen(reference_address)-strlen(tip_addresses[i].address);
                if(chars_to_copy<0) {
                    chars_to_copy=0;
                }
            }
        }
        tip_addresses[i].address=realloc(tip_addresses[i].address,13);
      /*
      if(short_length) {
        for(j=0;j<short_length;j++) {
          tip_addresses[i].address[chars_to_copy+j] = '0';
        }
      }*/

        for(j=strlen(tip_addresses[i].address)-1; j>=0; j--) {
            tip_addresses[i].address[j+chars_to_copy+short_length]=tip_addresses[i].address[j];
        }

        for(j=0;j<chars_to_copy;j++) {
            tip_addresses[i].address[j]=reference_address[j];
        }
        if(short_length) {
            for(j=0;j<short_length;j++) {
                tip_addresses[i].address[chars_to_copy+j]='0';
            }
        }
        tip_addresses[i].address[12]='\0';
    }
}

static void preprocess_tip_array(int size) {

    if (intel_pt_state.last_tip_address) {
        construct_fully_qualified_address(0, intel_pt_state.last_tip_address);
    }
    int i;
    for(i=1;i<=size;i++) {
        construct_fully_qualified_address(i, tip_addresses[i-1].address);
    }
}

/* find_newline_and_copy(char *buffer, int pos, int end, char *copy) 
 *    - copies characters into copy till a newline
 *    - returns number of characters copied
 */

int find_newline_and_copy(char *buffer, int pos, int end, char *copy) {
  int i = 0;
  int count = 0;
  while(buffer[pos+i] != '\n' && pos+i <= end) {
    copy[i] = buffer[pos+i];
    count++;
    i++;
  }
  if(pos+i > end) return -1;
  copy[i] = '\0'; return count;
}


/* get_array_of_tnt_bits()
 *  parameters : none
 *  returns : the array containing the TNT bits
 *  also maintains 2 arrays - one having the TIP addresses with some metadata
 *  the other being the FUP addresses with metadata
 *  use the gzlib standard library
 */

struct intel_pt_read_state intel_pt_state = {};

void get_array_of_tnt_bits(void) { 
    char *pch;
    char *pch_pip;
    bool stop_parsing_due_to_heartbeat = false;
    bool stop_parsing_due_to_overflow = false;
    //int len;

    int is_ignore_tip = 0;
    int is_ignore_pip = 0;
    int count_fup_after_ovf = 0;
    unsigned long long k, prev_count;
    unsigned long long j;
    int max_lines_read = 300000, curr_lines_read = 0;

    //TODO: make this commandline
    const char *filename = "/home/arnabjyoti/linux-4.14.3/tools/perf/linux_05may21.txt.gz";
    if (!tnt_array) {
        tnt_array = malloc(1);
    }

    //tnt_array[0] = 'P';
    if (!intel_pt_state.intel_pt_file) { 
        intel_pt_state.intel_pt_file = gzopen(filename, "r");
    }
    intel_pt_state.tnt_index_limit = 0;
    intel_pt_state.fup_address_index_limit = 0;
    intel_pt_state.tip_address_index_limit = 0;

    int count = 0;

    int count_tip = 0;
    int count_fup = 0;

    tip_addresses = malloc(1 * sizeof(struct tip_address_info));
    fup_addresses = malloc(1 * sizeof(struct fup_address_info));

    if(!intel_pt_state.intel_pt_file) {
        fprintf(stderr, "gzopen of %s failed.\n", filename);
        exit(EXIT_FAILURE);
    }

    //int err;
    //int bytes_read;
    //int start=0,pos=0;
    //char buffer[LENGTH];
    //bytes_read =gzread(file,buffer,LENGTH-1);
    //buffer[bytes_read]='\0';
    char copy[50];
    while(1) {
        if(gzgets(intel_pt_state.intel_pt_file, copy, 50) != 0) {
            copy[strcspn(copy, "\n")] = 0;
            curr_lines_read += 1;
        } else {
            printf("Incorrect read from gz file. Simulation probably finished...\n");
            exit(EXIT_SUCCESS);
        }
        //pos = find_newline_and_copy(buffer, start, bytes_read, copy+remainder);
        if (strncmp(copy, "PSBEND", 6) == 0) {
            stop_parsing_due_to_heartbeat = false;
            continue;
        }
        else if (strncmp(copy, "PSB", 3) == 0) {
            stop_parsing_due_to_heartbeat = true;
            continue;
        }
        else if (strncmp(copy, "OVF", 3) == 0) {
            stop_parsing_due_to_overflow = true;
            continue;
        }
        if (stop_parsing_due_to_overflow) {
            if (strncmp(copy, "FUP", 3) == 0) {
                if (strncmp(copy+6, "ffffc", 5) == 0) {
                    count_fup_after_ovf += 1;
                    if (stop_parsing_due_to_heartbeat) {
                        stop_parsing_due_to_heartbeat = false;
                    }
                } else {
                    /* this is a useful TIP, not FUP packet */
                    tnt_array = realloc(tnt_array, count+1);
                    tnt_array[count] = 'P';
                    count++;
                    tip_addresses = realloc(tip_addresses, (count_tip+1)*sizeof(struct tip_address_info));
                    tip_addresses[count_tip].address = malloc(strlen(copy+6)-3 * sizeof(char));
                    memcpy(tip_addresses[count_tip].address, copy+6, strlen(copy+6)-3);
                    tip_addresses[count_tip].address[strlen(copy+6)-3] = '\0';
                    tip_addresses[count_tip].is_useful=1;
	                /* ip bytes appear in the trace as "TIP 0x40184c 6d" here 6 is the IP Bytes */
                    tip_addresses[count_tip].ip_bytes=6;
                    count_tip++;
                    stop_parsing_due_to_overflow = false;
                }
            }
            if (count_fup_after_ovf == 2) {
                count_fup_after_ovf = 0;
                stop_parsing_due_to_overflow = false;
            }
            continue;
        }
        if (!stop_parsing_due_to_heartbeat) {
	    if (strncmp(copy, "TNT", 3) == 0) {
                if(is_ignore_tip == 1) {
	            is_ignore_tip = 0;
	        }
	        pch = strchr(copy, '(');
	        prev_count = count;
	        count += ((*++pch) - '0');
	        tnt_array = realloc(tnt_array, count);
	        for(j=prev_count,k=0; j<count; j++, k++) {
	            tnt_array[j]=copy[4+k];
	        }
            }
            else if(strncmp(copy, "PIP", 3) == 0) {
                pch_pip = strchr(copy, '=');
	  
	  /* VMEXIT */
	        if((*++pch_pip - '0') == 0) {
	    // the next PIP bit should be (NR = 1) - you need to ignore those PIP bits
	    // only stray PIP (NR=1) packets should be considered and stored
	    // these stray PIP packets indicate context switch events 
	            is_ignore_pip = 1;
            // the FUP preceding this PIP will represent the source address for a VMEXIT
                    fup_addresses[count_fup-1].type = 'V';
	        }
          
	  /* VMENTRY */
	        else {
                    is_ignore_tip = 1;
	            if(is_ignore_pip == 1) {
	                is_ignore_pip = 0;
	            }
	        }
            }
            else {
                if(strncmp(copy, "TIP", 3) == 0) {
	            if(is_ignore_tip == 0) {
	                tnt_array = realloc(tnt_array, count+1);
	                tnt_array[count] = 'P';
	                count++;
	                // enter TIP addresses into global tip_address_array //
	                tip_addresses = realloc(tip_addresses, (count_tip+1)*sizeof(struct tip_address_info));
	                tip_addresses[count_tip].address = malloc(strlen(copy+6)-3 * sizeof(char));
	                memcpy(tip_addresses[count_tip].address, copy+6, strlen(copy+6)-3);
	                tip_addresses[count_tip].address[strlen(copy+6)-3] = '\0';
	                tip_addresses[count_tip].is_useful=1;
	                /* ip bytes appear in the trace as "TIP 0x40184c 6d" here 6 is the IP Bytes */
	                tip_addresses[count_tip].ip_bytes=copy[strlen(copy)-2]-'0';
	                count_tip++;
	            }
	            else {
	                tip_addresses = realloc(tip_addresses, (count_tip+1)*sizeof(struct tip_address_info));
	                tip_addresses[count_tip].address = malloc(strlen(copy+6)*sizeof(char));
	                memcpy(tip_addresses[count_tip].address,copy+6,strlen(copy+6)-3);
	                tip_addresses[count_tip].address[strlen(copy+6)-3] = '\0';
	                tip_addresses[count_tip].is_useful=0;
	                tip_addresses[count_tip].ip_bytes=copy[strlen(copy)-2]-'0';
	                count_tip++;
                        //printf("count: %llu\n", count);
	                is_ignore_tip=0;
                    }
	        }
	        else if(strncmp(copy, "FUP", 3) == 0) {
                    tnt_array = realloc(tnt_array, count+1);
                    tnt_array[count] = 'F';
                    count++;
                    fup_addresses = realloc(fup_addresses, (count_fup+1)*sizeof(struct fup_address_info));
                    fup_addresses[count_fup].address = malloc(strlen(copy+6)-3 * sizeof(char));
                    memcpy(fup_addresses[count_fup].address, copy+6, strlen(copy+6)-3);
                    fup_addresses[count_fup].address[strlen(copy+6)-3] = '\0';
                    fup_addresses[count_fup].type = 'I';
                    count_fup++;
                }
            }
	}
        //start += pos+1;
        if (curr_lines_read >= max_lines_read) {
            if (strncmp(copy, "TNT", 3) == 0) {
                break;
            }
        }
    }
   
    intel_pt_state.tnt_index_limit = count;
    intel_pt_state.fup_address_index_limit = count_fup;
    intel_pt_state.tip_address_index_limit = count_tip;
    intel_pt_state.number_of_lines_consumed += curr_lines_read;
    printf("Number of lines consumed: %llu\n", intel_pt_state.number_of_lines_consumed);

#if 0 
    printf("TNT array: %d\n", count);
    printf("FUP array: %d\n", count_fup);
    printf("TIP array: %d\n", count_tip);
# endif
 
   // preprocess the tip addresses //
    preprocess_tip_array(count_tip);

    intel_pt_state.last_tip_address = malloc(sizeof(tip_addresses[count_tip-1].address));
    strcpy(intel_pt_state.last_tip_address, tip_addresses[count_tip-1].address);

#if 0
    printf("final count : %d\n", count);  
#endif
}

/* Execute a TB, and fix up the CPU state afterwards if necessary */
static inline tcg_target_ulong cpu_tb_exec(CPUState *cpu, TranslationBlock *itb)
{
    CPUArchState *env = cpu->env_ptr;
    uintptr_t ret;
    TranslationBlock *last_tb;
    int tb_exit;
    uint8_t *tb_ptr = itb->tc.ptr;

    qemu_log_mask_and_addr(CPU_LOG_EXEC, itb->pc,
                           "Trace %d: %p ["
                           TARGET_FMT_lx "/" TARGET_FMT_lx "/%#x] %s\n",
                           cpu->cpu_index, itb->tc.ptr,
                           itb->cs_base, itb->pc, itb->flags,
                           lookup_symbol(itb->pc));

#if defined(DEBUG_DISAS)
    if (qemu_loglevel_mask(CPU_LOG_TB_CPU)
        && qemu_log_in_addr_range(itb->pc)) {
        FILE *logfile = qemu_log_lock();
        int flags = 0;
        if (qemu_loglevel_mask(CPU_LOG_TB_FPU)) {
            flags |= CPU_DUMP_FPU;
        }
#if defined(TARGET_I386)
        flags |= CPU_DUMP_CCOP;
#endif
        log_cpu_state(cpu, flags);
        qemu_log_unlock(logfile);
    }
#endif /* DEBUG_DISAS */
    /* hack: we do a replay of transmitted network packets right before virtqueue_kick is called. 
     * This is very opportunistic in the sense we try to flush a queue even if there is no tx packet */
    const char *virtqueue_get_buf_trap = "ffff814bfc20";
    if (env->eip == do_strtoul((char *)virtqueue_get_buf_trap)) {
        virtio_net_tx_replay(replay_tx_bh);
        virtio_net_handle_ctrl_replay(replay_ctrl_vdev, replay_ctrl_vq);
    }
    ret = tcg_qemu_tb_exec(env, tb_ptr);
    cpu->can_do_io = 1;
    last_tb = (TranslationBlock *)(ret & ~TB_EXIT_MASK);
    tb_exit = ret & TB_EXIT_MASK;
    trace_exec_tb_exit(last_tb, tb_exit);

    if (stopped_execution_of_tb_chain) {
        stopped_execution_of_tb_chain = 0;
    }

    if (tb_exit > TB_EXIT_IDX1) {
        /* We didn't start executing this TB (eg because the instruction
         * counter hit zero); we must restore the guest PC to the address
         * of the start of the TB.
         */
	/* Change by Arnab : stopped execution of tb chain is a global 
	 * variable - this indicates if the exception that occurs later           
	 * occurred as a result of cpu->exit_request becoming 1  
	 */
	stopped_execution_of_tb_chain = 1;

	index_array = prev_index_array;
	index_tip_address = prev_index_tip_address;
	index_fup_address = prev_index_fup_address;

        CPUClass *cc = CPU_GET_CLASS(cpu);
        qemu_log_mask_and_addr(CPU_LOG_EXEC, last_tb->pc,
                               "Stopped execution of TB chain before %p ["
                               TARGET_FMT_lx "] %s\n",
                               last_tb->tc.ptr, last_tb->pc,
                               lookup_symbol(last_tb->pc));
        
        if (cc->synchronize_from_tb) {
            cc->synchronize_from_tb(cpu, last_tb);
        } else {
            assert(cc->set_pc);
            cc->set_pc(cpu, last_tb->pc);
        }
    } else {
        if (index_array_incremented && 
                tnt_array[index_array-1] == 'T' &&
                env->eip == itb->jmp_target2) {
#if 1
            printf("Divergence here: Should go to 0x%lx\n", itb->jmp_target1);
#endif
            env->eip = itb->jmp_target1;
        }
        if (index_array_incremented &&
                tnt_array[index_array-1] == 'N' &&
                env->eip == itb->jmp_target1) {
#if 1
            printf("Divergence here: Should go to 0x%lx\n", itb->jmp_target2);
#endif
            env->eip = itb->jmp_target2;
        }
    }
    return ret;
}

#ifndef CONFIG_USER_ONLY
/* Execute the code without caching the generated code. An interpreter
   could be used if available. */
static void cpu_exec_nocache(CPUState *cpu, int max_cycles,
                             TranslationBlock *orig_tb, bool ignore_icount)
{
    TranslationBlock *tb;
    uint32_t cflags = curr_cflags() | CF_NOCACHE;

    if (ignore_icount) {
        cflags &= ~CF_USE_ICOUNT;
    }

    /* Should never happen.
       We only end up here when an existing TB is too long.  */
    cflags |= MIN(max_cycles, CF_COUNT_MASK);

    mmap_lock();
    tb = tb_gen_code(cpu, orig_tb->pc, orig_tb->cs_base,
                     orig_tb->flags, cflags);
    tb->orig_tb = orig_tb;
    mmap_unlock();

    /* execute the generated code */
    trace_exec_tb_nocache(tb, tb->pc);
    cpu_tb_exec(cpu, tb);

    mmap_lock();
    tb_phys_invalidate(tb, -1);
    mmap_unlock();
    tcg_tb_remove(tb);
}
#endif

void cpu_exec_step_atomic(CPUState *cpu)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);
    TranslationBlock *tb;
    target_ulong cs_base, pc;
    uint32_t flags;
    uint32_t cflags = 1;
    uint32_t cf_mask = cflags & CF_HASH_MASK;

    if (sigsetjmp(cpu->jmp_env, 0) == 0) {
        start_exclusive();

        tb = tb_lookup__cpu_state(cpu, &pc, &cs_base, &flags, cf_mask);
        if (tb == NULL) {
            mmap_lock();
            tb = tb_gen_code(cpu, pc, cs_base, flags, cflags);
            mmap_unlock();
        }

        /* Since we got here, we know that parallel_cpus must be true.  */
        parallel_cpus = false;
        cc->cpu_exec_enter(cpu);
        /* execute the generated code */
        trace_exec_tb(tb, pc);
        cpu_tb_exec(cpu, tb);
        cc->cpu_exec_exit(cpu);
    } else {
        /*
         * The mmap_lock is dropped by tb_gen_code if it runs out of
         * memory.
         */
#ifndef CONFIG_SOFTMMU
        tcg_debug_assert(!have_mmap_lock());
#endif
        if (qemu_mutex_iothread_locked()) {
            qemu_mutex_unlock_iothread();
        }
        assert_no_pages_locked();
        qemu_plugin_disable_mem_helpers(cpu);
    }


    /*
     * As we start the exclusive region before codegen we must still
     * be in the region if we longjump out of either the codegen or
     * the execution.
     */
    g_assert(cpu_in_exclusive_context(cpu));
    parallel_cpus = true;
    end_exclusive();
}

struct tb_desc {
    target_ulong pc;
    target_ulong cs_base;
    CPUArchState *env;
    tb_page_addr_t phys_page1;
    uint32_t flags;
    uint32_t cf_mask;
    uint32_t trace_vcpu_dstate;
};

static bool tb_lookup_cmp(const void *p, const void *d)
{
    const TranslationBlock *tb = p;
    const struct tb_desc *desc = d;

    if (tb->pc == desc->pc &&
        tb->page_addr[0] == desc->phys_page1 &&
        tb->cs_base == desc->cs_base &&
        tb->flags == desc->flags &&
        tb->trace_vcpu_dstate == desc->trace_vcpu_dstate &&
        (tb_cflags(tb) & (CF_HASH_MASK | CF_INVALID)) == desc->cf_mask) {
        /* check next page if needed */
        if (tb->page_addr[1] == -1) {
            return true;
        } else {
            tb_page_addr_t phys_page2;
            target_ulong virt_page2;

            virt_page2 = (desc->pc & TARGET_PAGE_MASK) + TARGET_PAGE_SIZE;
            phys_page2 = get_page_addr_code(desc->env, virt_page2);
            if (tb->page_addr[1] == phys_page2) {
                return true;
            }
        }
    }
    return false;
}

TranslationBlock *tb_htable_lookup(CPUState *cpu, target_ulong pc,
                                   target_ulong cs_base, uint32_t flags,
                                   uint32_t cf_mask)
{
    tb_page_addr_t phys_pc;
    struct tb_desc desc;
    uint32_t h;

    desc.env = (CPUArchState *)cpu->env_ptr;
    desc.cs_base = cs_base;
    desc.flags = flags;
    desc.cf_mask = cf_mask;
    desc.trace_vcpu_dstate = *cpu->trace_dstate;
    desc.pc = pc;
    phys_pc = get_page_addr_code(desc.env, pc);
    if (phys_pc == -1) {
        return NULL;
    }
    desc.phys_page1 = phys_pc & TARGET_PAGE_MASK;
    h = tb_hash_func(phys_pc, pc, flags, cf_mask, *cpu->trace_dstate);
    return qht_lookup_custom(&tb_ctx.htable, &desc, h, tb_lookup_cmp);
}

void tb_set_jmp_target(TranslationBlock *tb, int n, uintptr_t addr)
{
    if (TCG_TARGET_HAS_direct_jump) {
        uintptr_t offset = tb->jmp_target_arg[n];
        uintptr_t tc_ptr = (uintptr_t)tb->tc.ptr;
        tb_target_set_jmp_target(tc_ptr, tc_ptr + offset, addr);
    } else {
        tb->jmp_target_arg[n] = addr;
    }
}

static inline void tb_add_jump(TranslationBlock *tb, int n,
                               TranslationBlock *tb_next)
{
    uintptr_t old;

    assert(n < ARRAY_SIZE(tb->jmp_list_next));
    qemu_spin_lock(&tb_next->jmp_lock);

    /* make sure the destination TB is valid */
    if (tb_next->cflags & CF_INVALID) {
        goto out_unlock_next;
    }
    /* Atomically claim the jump destination slot only if it was NULL */
    old = atomic_cmpxchg(&tb->jmp_dest[n], (uintptr_t)NULL, (uintptr_t)tb_next);
    if (old) {
        goto out_unlock_next;
    }

    /* patch the native jump address */
    tb_set_jmp_target(tb, n, (uintptr_t)tb_next->tc.ptr);

    /* add in TB jmp list */
    tb->jmp_list_next[n] = tb_next->jmp_list_head;
    tb_next->jmp_list_head = (uintptr_t)tb | n;

    qemu_spin_unlock(&tb_next->jmp_lock);

    qemu_log_mask_and_addr(CPU_LOG_EXEC, tb->pc,
                           "Linking TBs %p [" TARGET_FMT_lx
                           "] index %d -> %p [" TARGET_FMT_lx "]\n",
                           tb->tc.ptr, tb->pc, n,
                           tb_next->tc.ptr, tb_next->pc);
    return;

 out_unlock_next:
    qemu_spin_unlock(&tb_next->jmp_lock);
    return;
}


static inline TranslationBlock *tb_find(CPUState *cpu,
                                        TranslationBlock *last_tb,
                                        int tb_exit, uint32_t cf_mask)
{

    TranslationBlock *tb;
    target_ulong cs_base, pc;
    uint32_t flags;

    tb = tb_lookup__cpu_state(cpu, &pc, &cs_base, &flags, cf_mask);
    if (tb == NULL) {
        mmap_lock();
        tb = tb_gen_code(cpu, pc, cs_base, flags, cf_mask);
        mmap_unlock();
        /* We add the TB in the virtual pc hash table for the fast lookup */
        // atomic_set(&cpu->tb_jmp_cache[tb_jmp_cache_hash_func(pc)], tb);
    }
#ifndef CONFIG_USER_ONLY
    /* We don't take care of direct jumps when address mapping changes in
     * system emulation. So it's not safe to make a direct jump to a TB
     * spanning two pages because the mapping for the second page can change.
     */
    if (tb->page_addr[1] != -1) {
        last_tb = NULL;
    }
#endif
    /* See if we can patch the calling TB. */
    if (last_tb) {
        tb_add_jump(last_tb, tb_exit, tb);
    }
    return tb;
}

static inline bool cpu_handle_halt(CPUState *cpu)
{
    if (cpu->halted) {
#if defined(TARGET_I386) && !defined(CONFIG_USER_ONLY)
        if ((cpu->interrupt_request & CPU_INTERRUPT_POLL)
            && replay_interrupt()) {
            X86CPU *x86_cpu = X86_CPU(cpu);
            qemu_mutex_lock_iothread();
            apic_poll_irq(x86_cpu->apic_state);
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_POLL);
            qemu_mutex_unlock_iothread();
        }
#endif
        if (!cpu_has_work(cpu)) {
            return true;
        }

        cpu->halted = 0;
    }

    return false;
}

static inline void cpu_handle_debug_exception(CPUState *cpu)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);
    CPUWatchpoint *wp;

    if (!cpu->watchpoint_hit) {
        QTAILQ_FOREACH(wp, &cpu->watchpoints, entry) {
            wp->flags &= ~BP_WATCHPOINT_HIT;
        }
    }

    cc->debug_excp_handler(cpu);
}

static inline bool cpu_handle_exception(CPUState *cpu, int *ret)
{
    if (cpu->exception_index < 0) {
#ifndef CONFIG_USER_ONLY
        if (replay_has_exception()
            && cpu_neg(cpu)->icount_decr.u16.low + cpu->icount_extra == 0) {
            /* try to cause an exception pending in the log */
            cpu_exec_nocache(cpu, 1, tb_find(cpu, NULL, 0, curr_cflags()), true);
        }
#endif
        if (cpu->exception_index < 0) {
            return false;
        }
    }

    if (cpu->exception_index >= EXCP_INTERRUPT) {
        *ret = cpu->exception_index;
        if (*ret == EXCP_DEBUG) {
            cpu_handle_debug_exception(cpu);
        }
        cpu->exception_index = -1;
        return true;
    } else {
#if defined(CONFIG_USER_ONLY)
        /* if user mode only, we simulate a fake exception
           which will be handled outside the cpu execution
           loop */
#if defined(TARGET_I386)
        CPUClass *cc = CPU_GET_CLASS(cpu);
        cc->do_interrupt(cpu);
#endif
        *ret = cpu->exception_index;
        cpu->exception_index = -1;
        return true;
#else
        if (replay_exception()) {
            CPUClass *cc = CPU_GET_CLASS(cpu);
            qemu_mutex_lock_iothread();
            cc->do_interrupt(cpu);
            qemu_mutex_unlock_iothread();
            cpu->exception_index = -1;

        } else if (!replay_has_interrupt()) {
            *ret = EXCP_INTERRUPT;
            return true;
        }
#endif
    }

    return false;
}

static inline bool cpu_handle_interrupt(CPUState *cpu,
                                        TranslationBlock **last_tb)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);

    /* Clear the interrupt flag now since we're processing
     * cpu->interrupt_request and cpu->exit_request.
     * Ensure zeroing happens before reading cpu->exit_request or
     * cpu->interrupt_request (see also smp_wmb in cpu_exit())
     */
    atomic_mb_set(&cpu_neg(cpu)->icount_decr.u16.high, 0);

    if (unlikely(atomic_read(&cpu->interrupt_request))) {
        int interrupt_request;
        qemu_mutex_lock_iothread();
        interrupt_request = cpu->interrupt_request;
        if (unlikely(cpu->singlestep_enabled & SSTEP_NOIRQ)) {
            /* Mask out external interrupts for this step. */
            interrupt_request &= ~CPU_INTERRUPT_SSTEP_MASK;
        }
        if (interrupt_request & CPU_INTERRUPT_DEBUG) {
            cpu->interrupt_request &= ~CPU_INTERRUPT_DEBUG;
            cpu->exception_index = EXCP_DEBUG;
            qemu_mutex_unlock_iothread();
            return true;
        }
        if (replay_mode == REPLAY_MODE_PLAY  && !replay_has_interrupt() ) {
            /* Do nothing */
        } else if (interrupt_request & CPU_INTERRUPT_HALT) {
            //replay_interrupt();
            cpu->interrupt_request &= ~CPU_INTERRUPT_HALT;
            cpu->halted = 1;
            cpu->exception_index = EXCP_HLT;
            qemu_mutex_unlock_iothread();
            return true;
        }
#if defined(TARGET_I386)
        else if (interrupt_request & CPU_INTERRUPT_INIT) {
            X86CPU *x86_cpu = X86_CPU(cpu);
            CPUArchState *env = &x86_cpu->env;
            //replay_interrupt();
            cpu_svm_check_intercept_param(env, SVM_EXIT_INIT, 0, 0);
            do_cpu_init(x86_cpu);
            cpu->exception_index = EXCP_HALTED;
            qemu_mutex_unlock_iothread();
            return true;
        }
#else
        else if (interrupt_request & CPU_INTERRUPT_RESET) {
            //replay_interrupt();
            cpu_reset(cpu);
            qemu_mutex_unlock_iothread();
            return true;
        }
#endif
        /* The target hook has 3 exit conditions:
           False when the interrupt isn't processed,
           True when it is, and we should restart on a new TB,
           and via longjmp via cpu_loop_exit.  */
        else {
            if (cc->cpu_exec_interrupt(cpu, interrupt_request)) {
                //replay_interrupt();
                cpu->exception_index = -1;
                *last_tb = NULL;
            }
            /* The target hook may have updated the 'cpu->interrupt_request';
             * reload the 'interrupt_request' value */
            interrupt_request = cpu->interrupt_request;
        }
        if (interrupt_request & CPU_INTERRUPT_EXITTB) {
            cpu->interrupt_request &= ~CPU_INTERRUPT_EXITTB;
            /* ensure that no TB jump will be modified as
               the program flow was changed */
            *last_tb = NULL;
        }

        /* If we exit via cpu_loop_exit/longjmp it is reset in cpu_exec */
        qemu_mutex_unlock_iothread();
    }

    /* Finally, check if we need to exit to the main loop.  */
    if (unlikely(atomic_read(&cpu->exit_request))
        || (use_icount
            && cpu_neg(cpu)->icount_decr.u16.low + cpu->icount_extra == 0)) {
        atomic_set(&cpu->exit_request, 0);
        if (cpu->exception_index == -1) {
            cpu->exception_index = EXCP_INTERRUPT;
        }
        return true;
    }
    return false;
}

static inline void cpu_loop_exec_tb(CPUState *cpu, TranslationBlock *tb,
                                    TranslationBlock **last_tb, int *tb_exit)
{
    uintptr_t ret;
    int32_t insns_left;
    trace_exec_tb(tb, tb->pc);
    ret = cpu_tb_exec(cpu, tb);
    tb = (TranslationBlock *)(ret & ~TB_EXIT_MASK);
    *tb_exit = ret & TB_EXIT_MASK;
    if (*tb_exit != TB_EXIT_REQUESTED) {
        *last_tb = tb;
        return;
    }

    *last_tb = NULL;
    insns_left = atomic_read(&cpu_neg(cpu)->icount_decr.u32);
    if (insns_left < 0) {
        /* Something asked us to stop executing chained TBs; just
         * continue round the main loop. Whatever requested the exit
         * will also have set something else (eg exit_request or
         * interrupt_request) which will be handled by
         * cpu_handle_interrupt.  cpu_handle_interrupt will also
         * clear cpu->icount_decr.u16.high.
         */
        return;
    }

    /* Instruction counter expired.  */
    assert(use_icount);
#ifndef CONFIG_USER_ONLY
    /* Ensure global icount has gone forward */
    cpu_update_icount(cpu);
    /* Refill decrementer and continue execution.  */
    insns_left = MIN(0xffff, cpu->icount_budget);
    cpu_neg(cpu)->icount_decr.u16.low = insns_left;
    cpu->icount_extra = cpu->icount_budget - insns_left;
    if (!cpu->icount_extra) {
        /* Execute any remaining instructions, then let the main loop
         * handle the next event.
         */
        if (insns_left > 0) {
            cpu_exec_nocache(cpu, insns_left, tb, false);
        }
    }
#endif
}

/* main execution loop */

int cpu_exec(CPUState *cpu)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);
    int ret;
    SyncClocks sc = { 0 };

    /* replay_interrupt may need current_cpu */
    current_cpu = cpu;

    if (cpu_handle_halt(cpu)) {
        return EXCP_HALTED;
    }

    rcu_read_lock();

    cc->cpu_exec_enter(cpu);

    /* Calculate difference between guest clock and host clock.
     * This delay includes the delay of the last cycle, so
     * what we have to do is sleep until it is 0. As for the
     * advance/delay we gain here, we try to fix it next time.
     */
    init_delay_params(&sc, cpu);

    /* prepare setjmp context for exception handling */
    if (sigsetjmp(cpu->jmp_env, 0) != 0) {
#if defined(__clang__) || !QEMU_GNUC_PREREQ(4, 6)
        /* Some compilers wrongly smash all local variables after
         * siglongjmp. There were bug reports for gcc 4.5.0 and clang.
         * Reload essential local variables here for those compilers.
         * Newer versions of gcc would complain about this code (-Wclobbered). */
        cpu = current_cpu;
        cc = CPU_GET_CLASS(cpu);
#else /* buggy compiler */
        /* Assert that the compiler does not smash local variables. */
        g_assert(cpu == current_cpu);
        g_assert(cc == CPU_GET_CLASS(cpu));
#endif /* buggy compiler */
#ifndef CONFIG_SOFTMMU
        tcg_debug_assert(!have_mmap_lock());
#endif
        if (qemu_mutex_iothread_locked()) {
            qemu_mutex_unlock_iothread();
        }
        qemu_plugin_disable_mem_helpers(cpu);

        assert_no_pages_locked();
    }
    if(tnt_array == NULL) {
        printf("tnt_array is empty\n");
        exit(1);
    }
    /* if an exception is pending, we execute it here */
    while (!cpu_handle_exception(cpu, &ret)) {
        TranslationBlock *last_tb = NULL;
        int tb_exit = 0;
        while (!cpu_handle_interrupt(cpu, &last_tb)) {
            uint32_t cflags = cpu->cflags_next_tb;
            TranslationBlock *tb;

            /* When requested, use an exact setting for cflags for the next
               execution.  This is used for icount, precise smc, and stop-
               after-access watchpoints.  Since this request should never
               have CF_INVALID set, -1 is a convenient invalid value that
               does not require tcg headers for cpu_common_reset.  */
            if (cflags == -1) {
                cflags = curr_cflags();
            } else {
                cpu->cflags_next_tb = -1;
            }

            tb = tb_find(cpu, last_tb, tb_exit, cflags);
            cpu_loop_exec_tb(cpu, tb, &last_tb, &tb_exit);
            /* Try to align the host and virtual clocks
               if the guest is in advance */
            align_clocks(&sc, cpu);
        }
    }

    cc->cpu_exec_exit(cpu);
    rcu_read_unlock();

    return ret;
}
