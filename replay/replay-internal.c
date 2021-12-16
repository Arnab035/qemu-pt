/*
 * replay-internal.c
 *
 * Copyright (c) 2010-2015 Institute for System Programming
 *                         of the Russian Academy of Sciences.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "sysemu/replay.h"
#include "sysemu/runstate.h"
#include "replay-internal.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"

/* Mutex to protect reading and writing events to the log.
   data_kind and has_unread_data are also protected
   by this mutex.
   It also protects replay events queue which stores events to be
   written or read to the log. */
static QemuMutex lock;

/* File for replay writing */
static bool write_error;
FILE *replay_file;

/* Files for writing/replaying individual I/O events */
FILE *arnab_clock_replay_file; 
FILE *arnab_network_replay_file;
FILE *arnab_disk_replay_file;
FILE *arnab_host_clock_replay_file_cpu0;
FILE *arnab_host_clock_replay_file_cpu1;

static void replay_write_error(void)
{
    if (!write_error) {
        error_report("replay write error");
        write_error = true;
    }
}

static void replay_read_error(void)
{
    error_report("error reading the replay data");
    exit(1);
}

void replay_put_byte(uint8_t byte)
{
    if (replay_file) {
        if (putc(byte, replay_file) == EOF) {
            replay_write_error();
        }
    }
}

/* all replay_put* and replay_get* functions will now be
 * modified to arnab_replay_put* and arnab_replay_get*
 * methods. The only difference between the new ones with
 * the prior ones is that I now use my own replay_file */

/* Instead of writing separate functions, I could have
 * used a single function and separated the two logical
 * blocks- one which uses my replay file and the other 
 * which uses the previous replay file */

/* the CPU id is now added */

void arnab_replay_put_byte(uint8_t byte, const char *event_type, int cpu)
{
    if (strcmp(event_type, "clock") == 0) {	
        if (arnab_clock_replay_file) {
            if (putc(byte, arnab_clock_replay_file) == EOF) {
	        replay_write_error();
	    }
	}
    }
    else if (strcmp(event_type, "network") == 0) {
        if (arnab_network_replay_file) {
	    if (putc(byte, arnab_network_replay_file) == EOF) {
	        replay_write_error();
	    }
	}
    }
    else if (strcmp(event_type, "disk") == 0) {
	if (arnab_disk_replay_file) {
	    if (putc(byte, arnab_disk_replay_file) == EOF) {
	        replay_write_error();
	    }
	}
    }
    // we'll only write host clock values, other clock types do not lead to non-determinism.
    // since we do not use icount here, so even the 'virtual instruction' clock won't lead to 
    // non-determinism
    else if (strcmp(event_type, "host-clock") == 0) {
        if (cpu == 0) {
            if (arnab_host_clock_replay_file_cpu0) {
	        if (putc(byte, arnab_host_clock_replay_file_cpu0) == EOF) {
	            replay_write_error();
	        }
	    }
        } else if (cpu == 1) {
            if (arnab_host_clock_replay_file_cpu1) {
                if (putc(byte, arnab_host_clock_replay_file_cpu1) == EOF) {
                    replay_write_error();
                }
            }
        }
    }
    else {
        error_report("Invalid event type");   
    }
}

void replay_put_event(uint8_t event)
{
    assert(event < EVENT_COUNT);
    replay_put_byte(event);
}

void arnab_replay_put_event(uint8_t event, const char *event_type, int cpu)
{
    assert(event < EVENT_COUNT);
    arnab_replay_put_byte(event, event_type, cpu);
}	


void replay_put_word(uint16_t word)
{
    replay_put_byte(word >> 8);
    replay_put_byte(word);
}

void arnab_replay_put_word(uint16_t word, const char *event_type, int cpu)
{
    arnab_replay_put_byte(word >> 8, event_type, cpu);
    arnab_replay_put_byte(word, event_type, cpu);
}

void replay_put_dword(uint32_t dword)
{
    replay_put_word(dword >> 16);
    replay_put_word(dword);
}

void arnab_replay_put_dword(uint32_t dword, const char *event_type, int cpu)
{
    arnab_replay_put_word(dword >> 16, event_type, cpu);
    arnab_replay_put_word(dword, event_type, cpu);
}

void replay_put_qword(int64_t qword)
{
    replay_put_dword(qword >> 32);
    replay_put_dword(qword);
}

void arnab_replay_put_qword(int64_t qword, const char *event_type, int cpu)
{
    arnab_replay_put_dword(qword >> 32, event_type, cpu);
    arnab_replay_put_dword(qword, event_type, cpu);
}

void replay_put_array(const uint8_t *buf, size_t size)
{
    if (replay_file) {
        replay_put_dword(size);
        if (fwrite(buf, 1, size, replay_file) != size) {
            replay_write_error();
        }
    }
}

void arnab_replay_put_array(const uint8_t *buf, size_t size, const char *event_type, int cpu)
{
    if (strcmp(event_type, "clock") == 0) {
        if (arnab_clock_replay_file) {
            arnab_replay_put_dword(size, event_type, cpu);
            if (fwrite(buf, 1, size, arnab_clock_replay_file) != size) {
                replay_write_error();
	    }
        }
    }
    else if (strcmp(event_type, "network") == 0) {
        if (arnab_network_replay_file) {
	    arnab_replay_put_dword(size, event_type, cpu);
	    if (fwrite(buf, 1 , size, arnab_network_replay_file) != size) {
	        replay_write_error();
	    }
	}
    }
    else if (strcmp(event_type, "disk") == 0) {
        if (arnab_disk_replay_file) {
	    arnab_replay_put_dword(size, event_type, cpu);
	    if (fwrite(buf, 1, size, arnab_disk_replay_file) != size) {
	        replay_write_error();
	    }
	}
    }
    else if (strcmp(event_type, "host-clock") == 0) {
        if (cpu == 0) {
            if (arnab_host_clock_replay_file_cpu0) {
                arnab_replay_put_dword(size, event_type, cpu);
                if (fwrite(buf, 1, size, arnab_host_clock_replay_file_cpu0) != size) {
                    replay_write_error();
                }
            }
        }
        else if (cpu == 1) {
            if (arnab_host_clock_replay_file_cpu1) {
                arnab_replay_put_dword(size, event_type, cpu);
                if (fwrite(buf, 1, size, arnab_host_clock_replay_file_cpu1) != size) {
                    replay_write_error();
                }
            }
	}
    }
    else {
        error_report("Invalid event type");
    } 
}

uint8_t replay_get_byte(void)
{
    uint8_t byte = 0;
    if (replay_file) {
        int r = getc(replay_file);
        if (r == EOF) {
            replay_read_error();
        }
        byte = r;
    }
    return byte;
}

uint8_t arnab_replay_get_byte(const char *event_type, int cpu)
{
    uint8_t byte = 0;
    if (strcmp(event_type, "clock") == 0) {
        if (arnab_clock_replay_file) {
            byte = getc(arnab_clock_replay_file);
        }
    }
    else if (strcmp(event_type, "network") == 0) {
        if (arnab_network_replay_file) {
	    byte = getc(arnab_network_replay_file);
	}
    }
    else if (strcmp(event_type, "disk") == 0) {
        if (arnab_disk_replay_file) {
	    byte = getc(arnab_disk_replay_file);
	}
    }
    else if (strcmp(event_type, "host-clock") == 0) {
        if (cpu == 0) {
            if (arnab_host_clock_replay_file_cpu0) {
	        byte = getc(arnab_host_clock_replay_file_cpu0);
	    }
        }
        else if (cpu == 1) {
            if (arnab_host_clock_replay_file_cpu1) {
                byte = getc(arnab_host_clock_replay_file_cpu1);
            }
        }
    }
    else {
        error_report("Invalid event type");
    }
    return byte;
}

uint8_t arnab_replay_read_event(const char *event_type, int cpu)
{
    return arnab_replay_get_byte(event_type, cpu);
}

uint16_t replay_get_word(void)
{
    uint16_t word = 0;
    if (replay_file) {
        word = replay_get_byte();
        word = (word << 8) + replay_get_byte();
    }

    return word;
}

uint16_t arnab_replay_get_word(const char *event_type, int cpu)
{
    uint16_t word = 0;
    if (strcmp(event_type, "clock") == 0) {
        if(arnab_clock_replay_file) {
            word = arnab_replay_get_byte(event_type, cpu);
	    word = (word << 8) + arnab_replay_get_byte(event_type, cpu);
        }
    } 
    else if (strcmp(event_type, "network") == 0) {
        if(arnab_network_replay_file) {
	    word = arnab_replay_get_byte(event_type, cpu);
	    word = (word << 8) + arnab_replay_get_byte(event_type, cpu);
	}
    }
    else if (strcmp(event_type, "disk") == 0) {
        if (arnab_disk_replay_file) {
	    word = arnab_replay_get_byte(event_type, cpu);
	    word = (word << 8) + arnab_replay_get_byte(event_type, cpu);
	}
    }
    else if (strcmp(event_type, "host-clock") == 0) {
        if (cpu == 0) {
            if (arnab_host_clock_replay_file_cpu0) {
                word = arnab_replay_get_byte(event_type, cpu);
                word = (word << 8) + arnab_replay_get_byte(event_type, cpu);
            }
        }
	else if (cpu == 1) {
            if (arnab_host_clock_replay_file_cpu1) {
                word = arnab_replay_get_byte(event_type, cpu);
                word = (word << 8) + arnab_replay_get_byte(event_type, cpu);
            }
        }
    }
    else {
        error_report("Invalid event type");
    }
    return word;
}

uint32_t replay_get_dword(void)
{
    uint32_t dword = 0;
    if (replay_file) {
        dword = replay_get_word();
        dword = (dword << 16) + replay_get_word();
    }

    return dword;
}

uint32_t arnab_replay_get_dword(const char *event_type, int cpu)
{
    uint32_t dword = 0;
    if (strcmp(event_type, "clock") == 0) {
        if (arnab_clock_replay_file) {
            dword = arnab_replay_get_word(event_type, cpu);
            dword = (dword << 16) + arnab_replay_get_word(event_type, cpu);
	}
    }
    else if (strcmp(event_type, "network") == 0) {
        if (arnab_network_replay_file) {
	    dword = arnab_replay_get_word(event_type, cpu);
	    dword = (dword << 16) + arnab_replay_get_word(event_type, cpu);
	}
    }
    else if (strcmp(event_type, "disk") == 0) {
        if (arnab_disk_replay_file) {
	    dword = arnab_replay_get_word(event_type, cpu);
	    dword = (dword << 16) + arnab_replay_get_word(event_type, cpu);
	}
    }
    else if (strcmp(event_type, "host-clock") == 0) {
        if (cpu == 0) {
            if (arnab_host_clock_replay_file_cpu0) {
                dword = arnab_replay_get_word(event_type, cpu);
                dword = (dword << 16) + arnab_replay_get_word(event_type, cpu);
            }
        }
        else if (cpu == 1) {
            if (arnab_host_clock_replay_file_cpu1) {
                dword = arnab_replay_get_word(event_type, cpu);
                dword = (dword << 16) + arnab_replay_get_word(event_type, cpu);
            }
        }
    }
    else {
        error_report("Invalid event type");
    }
    return dword;
}

int64_t replay_get_qword(void)
{
    int64_t qword = 0;
    if (replay_file) {
        qword = replay_get_dword();
        qword = (qword << 32) + replay_get_dword();
    }

    return qword;
}

int64_t arnab_replay_get_qword(const char *event_type, int cpu) 
{
    int64_t qword = 0;
    if (strcmp(event_type, "clock") == 0) {
        if (arnab_clock_replay_file) {
            qword = arnab_replay_get_dword(event_type, cpu);
    	    qword = (qword << 32) + arnab_replay_get_dword(event_type, cpu);
        }	    
    }
    else if (strcmp(event_type, "network") == 0) {
        if (arnab_network_replay_file) {
	    qword = arnab_replay_get_dword(event_type, cpu);
	    qword = (qword << 32) + arnab_replay_get_dword(event_type, cpu);
	}
    }
    else if (strcmp(event_type, "disk") == 0) {
        if (arnab_disk_replay_file) {
	    qword = arnab_replay_get_dword(event_type, cpu);
	    qword = (qword << 32) + arnab_replay_get_dword(event_type, cpu);
	}
    }
    else if (strcmp(event_type, "host-clock") == 0) {
        if (cpu == 0) {
            if (arnab_host_clock_replay_file_cpu0) {
                qword = arnab_replay_get_dword(event_type, cpu);
                qword = (qword << 32) + arnab_replay_get_dword(event_type, cpu);
            }
        }
        else if (cpu == 1) {
            if (arnab_host_clock_replay_file_cpu1) {
                qword = arnab_replay_get_dword(event_type, cpu);
                qword = (qword << 32) + arnab_replay_get_dword(event_type, cpu);
            }
        }
    }
    else {
        error_report("Invalid event type");
    }
    return qword;
}

void replay_get_array(uint8_t *buf, size_t *size)
{
    if (replay_file) {
        *size = replay_get_dword();
        if (fread(buf, 1, *size, replay_file) != *size) {
            replay_read_error();
        }
    }
}

void arnab_replay_get_array(uint8_t *buf, size_t *size, const char *event_type, int cpu)
{
    if (strcmp(event_type, "clock") == 0) {
        if (arnab_clock_replay_file) {
            *size = arnab_replay_get_dword(event_type, cpu);
	    if (fread(buf, 1, *size, arnab_clock_replay_file) != *size) {
	        error_report("replay read error");
	    }
        }
    }
    else if (strcmp(event_type, "network") == 0) {
        if (arnab_network_replay_file) {
	    *size = arnab_replay_get_dword(event_type, cpu);
	    if (fread(buf, 1, *size, arnab_network_replay_file) != *size) {
	        error_report("replay read error");
	    }
	}
    }
    else if (strcmp(event_type, "disk") == 0) {
        if (arnab_disk_replay_file) {
	    *size = arnab_replay_get_dword(event_type, cpu);
	    if (fread(buf, 1, *size, arnab_disk_replay_file) != *size) {
	        error_report("replay read error");
	    }
	}
    }
    else if (strcmp(event_type, "host-clock") == 0) {
        if (cpu == 0) {
            if (arnab_host_clock_replay_file_cpu0) {
                *size = arnab_replay_get_dword(event_type, cpu);
                if (fread(buf, 1, *size, arnab_host_clock_replay_file_cpu0) != *size) {
                    error_report("replay read error");
                }
            }
        }
        else if (cpu == 1) {
            if (arnab_host_clock_replay_file_cpu1) {
                *size = arnab_replay_get_dword(event_type, cpu);
                if (fread(buf, 1, *size, arnab_host_clock_replay_file_cpu1) != *size) {
                    error_report("replay read error");
                }
            }
        }
    }
    else {
        error_report("replay read error");
    }    
}	

void replay_get_array_alloc(uint8_t **buf, size_t *size)
{
    if (replay_file) {
        *size = replay_get_dword();
        *buf = g_malloc(*size);
        if (fread(*buf, 1, *size, replay_file) != *size) {
            replay_read_error();
        }
    }
}

void arnab_replay_get_array_alloc(uint8_t **buf, size_t *size, const char *event_type, int cpu)
{
    if (strcmp(event_type, "clock") == 0) {
        if (arnab_clock_replay_file) {
            *size = arnab_replay_get_dword(event_type, cpu);
	    *buf = g_malloc(*size);
             if (fread(*buf, 1, *size, arnab_clock_replay_file) != *size) {
	         error_report("replay read error");
	     }
        }
    }
    else if (strcmp(event_type, "network") == 0) {
        if (arnab_network_replay_file) {
	    *size = arnab_replay_get_dword(event_type, cpu);
	    *buf = g_malloc(*size);
	    if (fread(*buf, 1, *size, arnab_network_replay_file) != *size) {
	        error_report("replay read error");
	    }
	}
    }
    else if (strcmp(event_type, "disk") == 0) {
        if (arnab_disk_replay_file) {
	    *size = arnab_replay_get_dword(event_type, cpu);
	    *buf = g_malloc(*size);
	    if (fread(*buf, 1, *size, arnab_disk_replay_file) != *size) {
	        error_report("replay read error");
	    }
	}
    }
    else if (strcmp(event_type, "host-clock") == 0) {
        if (cpu == 0) {
            if (arnab_host_clock_replay_file_cpu0) {
                *size = arnab_replay_get_dword(event_type, cpu);
                *buf = g_malloc(*size);
                if (fread(*buf, 1, *size, arnab_host_clock_replay_file_cpu0) != *size) {
                    error_report("replay read error");
                }
            }
        }
        else if (cpu == 1) {
            if (arnab_host_clock_replay_file_cpu1) {
                *size = arnab_replay_get_dword(event_type, cpu);
                *buf = g_malloc(*size);
                if (fread(*buf, 1, *size, arnab_host_clock_replay_file_cpu1) != *size) {
                    error_report("replay read error");
                }
            }
        }
    }
    else {
        error_report("Wrong event type");
    }
}

void replay_check_error(void)
{
    if (replay_file) {
        if (feof(replay_file)) {
            error_report("replay file is over");
            qemu_system_vmstop_request_prepare();
            qemu_system_vmstop_request(RUN_STATE_PAUSED);
        } else if (ferror(replay_file)) {
            error_report("replay file is over or something goes wrong");
            qemu_system_vmstop_request_prepare();
            qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
        }
    }
}

void replay_fetch_data_kind(void)
{
    if (replay_file) {
        if (!replay_state.has_unread_data) {
            replay_state.data_kind = replay_get_byte();
            if (replay_state.data_kind == EVENT_INSTRUCTION) {
                replay_state.instruction_count = replay_get_dword();
            }
            replay_check_error();
            replay_state.has_unread_data = 1;
            if (replay_state.data_kind >= EVENT_COUNT) {
                error_report("Replay: unknown event kind %d",
                             replay_state.data_kind);
                exit(1);
            }
        }
    }
}

void replay_finish_event(void)
{
    replay_state.has_unread_data = 0;
    replay_fetch_data_kind();
}

static __thread bool replay_locked;

void replay_mutex_init(void)
{
    qemu_mutex_init(&lock);
    /* Hold the mutex while we start-up */
    qemu_mutex_lock(&lock);
    replay_locked = true;
}

bool replay_mutex_locked(void)
{
    return replay_locked;
}

/* Ordering constraints, replay_lock must be taken before BQL */
void replay_mutex_lock(void)
{
    if (replay_mode != REPLAY_MODE_NONE) {
        g_assert(!qemu_mutex_iothread_locked());
        g_assert(!replay_mutex_locked());
        qemu_mutex_lock(&lock);
        replay_locked = true;
    }
}

void replay_mutex_unlock(void)
{
    if (replay_mode != REPLAY_MODE_NONE) {
        g_assert(replay_mutex_locked());
        replay_locked = false;
        qemu_mutex_unlock(&lock);
    }
}

void replay_advance_current_icount(uint64_t current_icount)
{
    int diff = (int)(current_icount - replay_state.current_icount);

    /* Time can only go forward */
    assert(diff >= 0);

    if (diff > 0) {
        replay_put_event(EVENT_INSTRUCTION);
        replay_put_dword(diff);
        replay_state.current_icount += diff;
    }
}

/*! Saves cached instructions. */

/*  this will never matter
 *  diff is bound to always
 *  return zero and hence
 *  nothing would be recorded to file
 */

void replay_save_instructions(void)
{
    if (replay_file && replay_mode == REPLAY_MODE_RECORD) {
        //g_assert(replay_mutex_locked());
        replay_advance_current_icount(replay_get_current_icount());
    }
}
