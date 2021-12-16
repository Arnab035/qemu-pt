/*
 * replay.c
 *
 * Copyright (c) 2010-2015 Institute for System Programming
 *                         of the Russian Academy of Sciences.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "sysemu/replay.h"
#include "sysemu/runstate.h"
#include "replay-internal.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "sysemu/cpus.h"
#include "qemu/error-report.h"

/* Current version of the replay mechanism.
   Increase it when file format changes. */
#define REPLAY_VERSION              0xe02009
/* Size of replay log header */
#define HEADER_SIZE                 (sizeof(uint32_t) + sizeof(uint64_t))

ReplayMode replay_mode = REPLAY_MODE_NONE;
ReplayMode arnab_replay_mode = REPLAY_MODE_NONE;

char *replay_snapshot;

/* Name of replay file  */
static char *replay_filename;

/* individual files for each I/O event to be replayed */
static char *arnab_clock_replay_filename;
static char *arnab_network_replay_filename;
static char *arnab_disk_replay_filename;
static char *arnab_host_clock_replay_filename_cpu0;
static char *arnab_host_clock_replay_filename_cpu1;

ReplayState replay_state;
static GSList *replay_blockers;

bool replay_next_event_is(int event)
{
    bool res = false;

    /* nothing to skip - not all instructions used */
    if (replay_state.instruction_count != 0) {
        assert(replay_state.data_kind == EVENT_INSTRUCTION);
        return event == EVENT_INSTRUCTION;
    }

    while (true) {
        unsigned int data_kind = replay_state.data_kind;
        if (event == data_kind) {
            res = true;
        }
        switch (data_kind) {
        case EVENT_SHUTDOWN ... EVENT_SHUTDOWN_LAST:
            replay_finish_event();
            qemu_system_shutdown_request(data_kind - EVENT_SHUTDOWN);
            break;
        default:
            /* clock, time_t, checkpoint and other events */
            return res;
        }
    }
    return res;
}

uint64_t replay_get_current_icount(void)
{
    return cpu_get_icount_raw();
}

int replay_get_instructions(void)
{
    int res = 0;
    replay_mutex_lock();
    if (replay_next_event_is(EVENT_INSTRUCTION)) {
        res = replay_state.instruction_count;
    }
    replay_mutex_unlock();
    return res;
}

void replay_account_executed_instructions(void)
{
    if (replay_mode == REPLAY_MODE_PLAY) {
      /*  g_assert(replay_mutex_locked()); */
        if (replay_state.instruction_count > 0) {
            int count = (int)(replay_get_current_icount()
                              - replay_state.current_icount);

            /* Time can only go forward */ 
            assert(count >= 0);

            replay_state.instruction_count -= count;
            replay_state.current_icount += count;
            if (replay_state.instruction_count == 0) {
                assert(replay_state.data_kind == EVENT_INSTRUCTION);
                replay_finish_event();
                /* Wake up iothread. This is required because
                   timers will not expire until clock counters
                   will be read from the log. */ 
                qemu_notify_event();
            }
        }
    }
}

bool replay_exception(void)
{
    if (replay_mode == REPLAY_MODE_RECORD) {
        //g_assert(replay_mutex_locked());
        replay_save_instructions();
        replay_put_event(EVENT_EXCEPTION);
        return true;
    } else if (replay_mode == REPLAY_MODE_PLAY) {
        //g_assert(replay_mutex_locked());
        bool res = replay_has_exception();
        if (res) {
            replay_finish_event();
        }
        return res;
    }
    return true;
}

bool replay_has_exception(void)
{
    bool res = false;
    if (replay_mode == REPLAY_MODE_PLAY) {
        //g_assert(replay_mutex_locked());
        replay_account_executed_instructions();
        res = replay_next_event_is(EVENT_EXCEPTION);
    }

    return res;
}

bool replay_interrupt(void)
{
    if (replay_mode == REPLAY_MODE_RECORD) {
        //g_assert(replay_mutex_locked());
        replay_save_instructions();
        replay_put_event(EVENT_INTERRUPT);
        return true;
    } else if (replay_mode == REPLAY_MODE_PLAY) {
        //g_assert(replay_mutex_locked());
        bool res = replay_has_interrupt();
        if (res) {
           replay_finish_event();
        }
        return res;
    }

    return true;
}

bool replay_has_interrupt(void)
{
    bool res = false;
    if (replay_mode == REPLAY_MODE_PLAY) {
        //g_assert(replay_mutex_locked());
        //replay_account_executed_instructions();
        res = replay_next_event_is(EVENT_INTERRUPT);
    }
    return res;
}

void replay_shutdown_request(ShutdownCause cause)
{
    if (replay_mode == REPLAY_MODE_RECORD) {
        //g_assert(replay_mutex_locked());
        replay_put_event(EVENT_SHUTDOWN + cause);
    }
}

/* we will only store checkpoint events in the file - that too
 * checkpoints CHECKPOINT_VMENTRY and CHECKPOINT_VMEXIT
 * so when this function runs, make sure that it never runs
 * replay_save_instructions() */

bool replay_checkpoint(ReplayCheckpoint checkpoint)
{
    bool res = false;
    static bool in_checkpoint;
    assert(EVENT_CHECKPOINT + checkpoint <= EVENT_CHECKPOINT_LAST);

    if (!replay_file) {
        return true;
    }

    if (in_checkpoint) {
        /* If we are already in checkpoint, then there is no need
           for additional synchronization.
           Recursion occurs when HW event modifies timers.
           Timer modification may invoke the checkpoint and
           proceed to recursion. */
        return true;
    }
    in_checkpoint = true;

    replay_save_instructions();

    if (replay_mode == REPLAY_MODE_PLAY) {
        //g_assert(replay_mutex_locked());
        if (replay_next_event_is(EVENT_CHECKPOINT + checkpoint)) {
            replay_finish_event();
        } else if (replay_state.data_kind != EVENT_ASYNC) {
            res = false;
            goto out;
        }
        replay_read_events(checkpoint);
        /* replay_read_events may leave some unread events.
           Return false if not all of the events associated with
           checkpoint were processed */
        res = replay_state.data_kind != EVENT_ASYNC;
    } else if (replay_mode == REPLAY_MODE_RECORD) {
        //g_assert(replay_mutex_locked());
        replay_put_event(EVENT_CHECKPOINT + checkpoint);
        /* This checkpoint belongs to several threads.
           Processing events from different threads is
           non-deterministic */
        if (checkpoint != CHECKPOINT_CLOCK_WARP_START
            /* FIXME: this is temporary fix, other checkpoints
                      may also be invoked from the different threads someday.
                      Asynchronous event processing should be refactored
                      to create additional replay event kind which is
                      nailed to the one of the threads and which processes
                      the event queue. */
            && checkpoint != CHECKPOINT_CLOCK_VIRTUAL) {
            replay_save_events(checkpoint);
        }
        res = true;
    }
out:
    in_checkpoint = false;
    return res;
}

bool replay_has_checkpoint(void)
{
    bool res = false;
    if (replay_mode == REPLAY_MODE_PLAY) {
        g_assert(replay_mutex_locked());
        replay_account_executed_instructions();
        res = EVENT_CHECKPOINT <= replay_state.data_kind
              && replay_state.data_kind <= EVENT_CHECKPOINT_LAST;
    }
    return res;
}

static void replay_enable(const char *fname, int mode)
{
    const char *fmode = NULL;
    assert(!replay_file);

    switch (mode) {
    case REPLAY_MODE_RECORD:
        fmode = "wb";
        break;
    case REPLAY_MODE_PLAY:
        fmode = "rb";
        break;
    default:
        fprintf(stderr, "Replay: internal error: invalid replay mode\n");
        exit(1);
    }

    atexit(replay_finish);

    replay_file = fopen(fname, fmode);
    if (replay_file == NULL) {
        fprintf(stderr, "Replay: open %s: %s\n", fname, strerror(errno));
        exit(1);
    }

    replay_filename = g_strdup(fname);
    replay_mode = mode;
    replay_mutex_init();

    replay_state.data_kind = -1;
    replay_state.instruction_count = 0;
    replay_state.current_icount = 0;
    replay_state.has_unread_data = 0;

    /* skip file header for RECORD and check it for PLAY */
    if (replay_mode == REPLAY_MODE_RECORD) {
        fseek(replay_file, HEADER_SIZE, SEEK_SET);
    } else if (replay_mode == REPLAY_MODE_PLAY) {
        unsigned int version = replay_get_dword();
        if (version != REPLAY_VERSION) {
            fprintf(stderr, "Replay: invalid input log file version\n");
            exit(1);
        }
        /* go to the beginning */
        fseek(replay_file, HEADER_SIZE, SEEK_SET);
        replay_fetch_data_kind();
    }

    replay_init_events();
}

/* copied from stack overflow */
static char** str_split(char* a_str, const char a_delim)
{
    char** result    = 0;
    size_t count     = 0;
    char* tmp        = a_str;
    char* last_comma = 0;
    char delim[2];
    delim[0] = a_delim;
    delim[1] = 0;

    /* Count how many elements will be extracted. */
    while (*tmp)
    {
        if (a_delim == *tmp)
        {
            count++;
            last_comma = tmp;
        }
        tmp++;
    }

    /* Add space for trailing token. */
    count += last_comma < (a_str + strlen(a_str) - 1);

    /* Add space for terminating null string so caller
       knows where the list of returned strings ends. */
    count++;

    result = malloc(sizeof(char*) * count);

    if (result)
    {
        size_t idx  = 0;
        char* token = strtok(a_str, delim);

        while (token)
        {
            assert(idx < count);
            *(result + idx++) = strdup(token);
            token = strtok(0, delim);
        }
        assert(idx == count - 1);
        *(result + idx) = 0;
    }

    return result;
}


// # this function is a modified version of replay_enable
// we use different global variables to store the replay mode 
// and the replay filename. So the original replay functions do 
// not get called inadvertently.
static void arnab_replay_enable(const char *fname, int mode, const char *event_type) {
    const char *fmode = NULL;
    printf("Replay file: %s\n", fname);

    switch (mode) {
    case REPLAY_MODE_RECORD:
	    fmode = "wb";
	    break;
    case REPLAY_MODE_PLAY:
	    fmode = "rb";
	    break;
    default:
	    fprintf(stderr, "Replay: internal error: invalid replay mode\n");
	    exit(1);
    }

    arnab_replay_mode = mode;

    if(strcmp(event_type, "clock") == 0) {
            atexit(arnab_clock_replay_finish);
            arnab_clock_replay_file = fopen(fname, fmode);
            if (arnab_clock_replay_file == NULL) {
                fprintf(stderr, "Replay: open %s: %s\n", fname, strerror(errno));
                exit(1);
            }
            arnab_clock_replay_filename = g_strdup(fname);
            if (mode == REPLAY_MODE_RECORD) {
                fseek(arnab_clock_replay_file, HEADER_SIZE, SEEK_SET);
            } else if (mode == REPLAY_MODE_PLAY) {
                unsigned int version = arnab_replay_get_dword(event_type, -1);
                if (version != REPLAY_VERSION) {
                    fprintf(stderr, "%s Replay: invalid input log file version\n", event_type);
                    exit(1);
                }
                fseek(arnab_clock_replay_file, HEADER_SIZE, SEEK_SET);
            }
    }
    else if (strcmp(event_type, "network") == 0) {
            atexit(arnab_network_replay_finish);
            arnab_network_replay_file = fopen(fname, fmode);
            if (arnab_network_replay_file == NULL) {
                fprintf(stderr, "Replay: open %s: %s\n", fname, strerror(errno));
                exit(1);
            }
            arnab_network_replay_filename = g_strdup(fname);
            if (mode == REPLAY_MODE_RECORD) {
                fseek(arnab_network_replay_file, HEADER_SIZE, SEEK_SET);
            } else if (mode == REPLAY_MODE_PLAY) {
                unsigned int version = arnab_replay_get_dword(event_type, -1);
                if (version != REPLAY_VERSION) {
                    fprintf(stderr, "%s Replay: invalid input log file version\n", event_type);
                    exit(1);
                }
                fseek(arnab_network_replay_file, HEADER_SIZE, SEEK_SET);
            }
    } else if (strcmp(event_type, "disk") == 0) {
            atexit(arnab_disk_replay_finish);
            arnab_disk_replay_file = fopen(fname, fmode);
            if (arnab_disk_replay_file == NULL) {
                fprintf(stderr, "Replay: open %s: %s\n", fname, strerror(errno));
                exit(1);
            }
            arnab_disk_replay_filename = g_strdup(fname);
            if (mode == REPLAY_MODE_RECORD) {
                fseek(arnab_disk_replay_file, HEADER_SIZE, SEEK_SET);
            } else if (mode == REPLAY_MODE_PLAY) {
                unsigned int version = arnab_replay_get_dword(event_type, -1);
                if (version != REPLAY_VERSION) {
                    fprintf(stderr, "%s Replay: invalid input log file version\n", event_type);
                    exit(1);
                }
                fseek(arnab_disk_replay_file, HEADER_SIZE, SEEK_SET);
            }
    } else if (strcmp(event_type, "host-clock") == 0) {
            atexit(arnab_host_clock_replay_finish);
            char** files;
            files = str_split((char *)fname, ',');
            if (files) {
                arnab_host_clock_replay_file_cpu0 = fopen(*(files+0), fmode);
                if (arnab_host_clock_replay_file_cpu0 == NULL) {
                    fprintf(stderr, "Replay: open %s: %s\n", *(files+0), strerror(errno));
                    exit(1);
	        }
		/*
		 * current use: the code only works for a 2-core setup
		 * where the guest vcpus are mapped to physical cores 0 & 1
		 *
		 * cpu 0 
		 * */
                arnab_host_clock_replay_filename_cpu0 = g_strdup(*(files+0));
                if (mode == REPLAY_MODE_RECORD) {
                    fseek(arnab_host_clock_replay_file_cpu0, HEADER_SIZE, SEEK_SET);
                } else if (mode == REPLAY_MODE_PLAY) {
                    unsigned int version = arnab_replay_get_dword(event_type, 0);
                    if (version != REPLAY_VERSION) {
                        fprintf(stderr, "%s Replay: invalid input log file version\n", event_type);
                        exit(1);
                    }
                    fseek(arnab_host_clock_replay_file_cpu0, HEADER_SIZE, SEEK_SET);
                }

		/* cpu 1*/
                arnab_host_clock_replay_file_cpu1 = fopen(*(files+1), fmode);
                if (arnab_host_clock_replay_file_cpu1 == NULL) {
                    fprintf(stderr, "Replay: open: %s: %s\n", *(files+1), fmode);
                    exit(1);
                }
                arnab_host_clock_replay_filename_cpu1 = g_strdup(*(files+1));
                if (mode == REPLAY_MODE_RECORD) {
                    fseek(arnab_host_clock_replay_file_cpu1, HEADER_SIZE, SEEK_SET);
                } else if (mode == REPLAY_MODE_PLAY) {
                    unsigned int version = arnab_replay_get_dword(event_type, 1);
                    if (version != REPLAY_VERSION) {
                        fprintf(stderr, "%s Replay: invalid input log file version\n", event_type);
                        exit(1);
                    }
                    fseek(arnab_host_clock_replay_file_cpu1, HEADER_SIZE, SEEK_SET);
                }
            }
            free(*(files+0)); free(*(files+1));
            free(files);
    } else {
            fprintf(stderr, "I/O event not supported\n");
            exit(1);
    }
}

void configure_artifact_generation(QemuOpts *opts)
{
    const char *insns_fname;
    const char *mem_fname;

    Location loc;
    if (!opts)
        return;

    if (arnab_replay_mode != REPLAY_MODE_PLAY) {
        error_report("Artifacts can only be generated in replay mode");
        exit(0);
    }

    loc_push_none(&loc);
    qemu_opts_loc_restore(opts);
    insns_fname = qemu_opt_get(opts, "insns");
    mem_fname = qemu_opt_get(opts, "mem");
    if (!mem_fname && !insns_fname) {
        error_report("Must either specify memory access trace filename or instruction access trace filename");
        exit(0);
    }
    atexit(finish_artifact_generation);
    if (insns_fname) {
        arnab_trace_insns_file = fopen(insns_fname, "w");
        if (!arnab_trace_insns_file) {
            printf("Could not open file to record instruction trace.. \n");
            exit(1);
        }
    }
    if (mem_fname) {
        arnab_trace_mem_file = fopen(mem_fname, "w");
        if (!arnab_trace_mem_file) {
            printf("Could not open file to record memory trace...\n");
            exit(1);
        }
    }
    loc_pop(&loc);
}

void arnab_replay_configure(QemuOpts *opts, const char *event_type)
{
    const char *fname;
    const char *rmode;

    const char *host_clock_fname;

    ReplayMode mode = REPLAY_MODE_NONE;
    Location loc;

    if(!opts) {
        return;
    }
    printf("About to start replaying\n");
    loc_push_none(&loc);
    qemu_opts_loc_restore(opts);
    fname = qemu_opt_get(opts, "file");
    if (!fname) {
	error_report("File name not specified for replay");
	exit(1);
    }
    rmode = qemu_opt_get(opts, "mode");
    if (!rmode) {
	error_report("Mode not specified for replay");
	exit(1);
    } else if(strcmp(rmode, "record") == 0) {
	mode = REPLAY_MODE_RECORD;
    } else if(strcmp(rmode, "replay") == 0) {
	mode = REPLAY_MODE_PLAY;
    }
    printf("About to enable replay for %s\n", event_type);
    if (strcmp(event_type, "network") == 0 || strcmp(event_type, "disk") == 0) {
        arnab_replay_enable(fname, mode, event_type);
    } else if (strcmp(event_type, "clock") == 0) {
        arnab_replay_enable(fname, mode, "clock");
        /* 
	 * host clock fname is the file used to store tsc values
	 * we store hpet & tsc values in separate files for ease of reading
	 * now for a two-core setup, there will be 2 files where tsc values will be written
	 * expect the host clock filename to be of the format - <filename 1, filename 2>
	 */
        host_clock_fname = qemu_opt_get(opts, "host-clock-file");
        if (!host_clock_fname) {
            error_report("File name not specified for storing host clock values");
        }
        else {
            arnab_replay_enable(host_clock_fname, mode, "host-clock");
        }
    } else {
        error_report("Invalid event type");
        exit(1);
    }
    loc_pop(&loc); 
}

void replay_configure(QemuOpts *opts)
{
    const char *fname;
    const char *rr;
    ReplayMode mode = REPLAY_MODE_NONE;
    Location loc;

    
    if (!opts) {
        return;
    }

    loc_push_none(&loc);
    qemu_opts_loc_restore(opts);

    
    rr = qemu_opt_get(opts, "rr");
    if (!rr) {
        goto out;
    } else if (!strcmp(rr, "record")) {
        mode = REPLAY_MODE_RECORD;
    } else if (!strcmp(rr, "replay")) {
        mode = REPLAY_MODE_PLAY;
    } else {
        error_report("Invalid icount rr option: %s", rr);
        exit(1);
    }

    fname = qemu_opt_get(opts, "rrfile");
    if (!fname) {
        error_report("File name not specified for replay");
        exit(1);
    }

        //mode = REPLAY_MODE_RECORD;
        //fname = "replay.bin";

    replay_snapshot = g_strdup(qemu_opt_get(opts, "rrsnapshot"));
    replay_vmstate_register();
    replay_enable(fname, mode);
out:
    loc_pop(&loc);
}

void replay_start(void)
{
    if (replay_mode == REPLAY_MODE_NONE) {
        return;
    }

    if (replay_blockers) {
        error_reportf_err(replay_blockers->data, "Record/replay: ");
        exit(1);
    }
    /*
    if (!use_icount) {
        error_report("Please enable icount to use record/replay");
        exit(1);
    }*/

    /* Timer for snapshotting will be set up here. */

    replay_enable_events();
}

void replay_finish(void)
{
    if (replay_mode == REPLAY_MODE_NONE) {
        return;
    }

    replay_save_instructions();

    /* finalize the file */
    if (replay_file) {
        if (replay_mode == REPLAY_MODE_RECORD) {
            /* write end event */
            replay_put_event(EVENT_END);

            /* write header */
            fseek(replay_file, 0, SEEK_SET);
            replay_put_dword(REPLAY_VERSION);
        }

        fclose(replay_file);
        replay_file = NULL;
    }
    if (replay_filename) {
        g_free(replay_filename);
        replay_filename = NULL;
    }

    g_free(replay_snapshot);
    replay_snapshot = NULL;

    replay_mode = REPLAY_MODE_NONE;

    replay_finish_events();
}

void arnab_clock_replay_finish(void)
{
    if (arnab_replay_mode == REPLAY_MODE_NONE) {
        return;
    }
    if (arnab_clock_replay_file) {
        if (arnab_replay_mode == REPLAY_MODE_RECORD) {
				            /* write end event */
	     arnab_replay_put_event(EVENT_END, "clock", -1);
	     fseek(arnab_clock_replay_file, 0, SEEK_SET);
	     arnab_replay_put_dword(REPLAY_VERSION, "clock", -1);
	}
        fclose(arnab_clock_replay_file);
	arnab_clock_replay_file = NULL;
    }
    if (arnab_clock_replay_filename) {
       g_free(arnab_clock_replay_filename);
       arnab_clock_replay_filename = NULL;
    }
}

void arnab_host_clock_replay_finish(void)
{
    if (arnab_replay_mode == REPLAY_MODE_NONE) {
        return;
    }
    if (arnab_host_clock_replay_file_cpu0) {
        if (arnab_replay_mode == REPLAY_MODE_RECORD) {
            arnab_replay_put_event(EVENT_END, "host-clock", 0);
            fseek(arnab_host_clock_replay_file_cpu0, 0, SEEK_SET);
            arnab_replay_put_dword(REPLAY_VERSION, "host-clock", 0);
        }
        fclose(arnab_host_clock_replay_file_cpu0);
        arnab_host_clock_replay_file_cpu0 = NULL;
    }
    if (arnab_host_clock_replay_filename_cpu0) {
        g_free(arnab_host_clock_replay_filename_cpu0);
        arnab_host_clock_replay_filename_cpu0 = NULL;
    }

    /* do this for replay file for cpu 1 too */
    if (arnab_host_clock_replay_file_cpu1) {
        if (arnab_replay_mode == REPLAY_MODE_RECORD) {
            arnab_replay_put_event(EVENT_END, "host-clock", 1);
            fseek(arnab_host_clock_replay_file_cpu1, 0, SEEK_SET);
            arnab_replay_put_dword(REPLAY_VERSION, "host-clock", 1);
        }
        fclose(arnab_host_clock_replay_file_cpu1);
        arnab_host_clock_replay_file_cpu1 = NULL;
        
    }
    if (arnab_host_clock_replay_filename_cpu1) {
        g_free(arnab_host_clock_replay_filename_cpu1);
        arnab_host_clock_replay_filename_cpu1 = NULL;
    }
}	

void arnab_network_replay_finish(void)
{
    if (arnab_replay_mode == REPLAY_MODE_NONE) {
        return;
    }
    if (arnab_network_replay_file) {
        if (arnab_replay_mode == REPLAY_MODE_RECORD) {
            arnab_replay_put_event(EVENT_END, "network", -1);
            fseek(arnab_network_replay_file, 0, SEEK_SET);
            arnab_replay_put_dword(REPLAY_VERSION, "network", -1);
        }
        fclose(arnab_network_replay_file);
        arnab_network_replay_file = NULL;
    }
    if (arnab_network_replay_filename) {
        g_free(arnab_network_replay_filename);
        arnab_network_replay_filename = NULL;
    }
}

void arnab_disk_replay_finish(void)
{
    if (arnab_replay_mode == REPLAY_MODE_NONE) {
        return;
    }
    if (arnab_disk_replay_file) {
        if (arnab_replay_mode == REPLAY_MODE_RECORD) {
            arnab_replay_put_event(EVENT_END, "disk", -1);
            fseek(arnab_disk_replay_file, 0, SEEK_SET);
            arnab_replay_put_dword(REPLAY_VERSION, "disk", -1);
        }
        fclose(arnab_disk_replay_file);
        arnab_disk_replay_file = NULL;
    }
    if (arnab_disk_replay_filename) {
        g_free(arnab_disk_replay_filename);
        arnab_disk_replay_filename = NULL;
    }
}

void finish_artifact_generation(void)
{
    if (arnab_replay_mode != REPLAY_MODE_PLAY) {
        return;
    }
    if (arnab_trace_insns_file) {
        fclose(arnab_trace_insns_file);
        arnab_trace_insns_file = NULL;
    }
    if (arnab_trace_mem_file) {
        fclose(arnab_trace_mem_file);
        arnab_trace_mem_file = NULL;
    }
}

void replay_add_blocker(Error *reason)
{
    replay_blockers = g_slist_prepend(replay_blockers, reason);
}
