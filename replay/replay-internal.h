#ifndef REPLAY_INTERNAL_H
#define REPLAY_INTERNAL_H

/*
 * replay-internal.h
 *
 * Copyright (c) 2010-2015 Institute for System Programming
 *                         of the Russian Academy of Sciences.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* Any changes to order/number of events will need to bump REPLAY_VERSION */
enum ReplayEvents {
    /* for instruction event */
    EVENT_INSTRUCTION,
    /* for software interrupt */
    EVENT_INTERRUPT,
    /* for emulated exceptions */
    EVENT_EXCEPTION,
    /* for async events */
    EVENT_ASYNC,
    /* for shutdown requests, range allows recovery of ShutdownCause */
    EVENT_SHUTDOWN,
    EVENT_SHUTDOWN_LAST = EVENT_SHUTDOWN + SHUTDOWN_CAUSE__MAX,
    /* for character device write event */
    EVENT_CHAR_WRITE,
    /* for character device read all event */
    EVENT_CHAR_READ_ALL,
    EVENT_CHAR_READ_ALL_ERROR,
    /* for audio out event */
    EVENT_AUDIO_OUT,
    /* for audio in event */
    EVENT_AUDIO_IN,
    /* for random number generator */
    EVENT_RANDOM,
    /* for clock read/writes */
    /* some of greater codes are reserved for clocks */
    EVENT_CLOCK,
    EVENT_CLOCK_LAST = EVENT_CLOCK + REPLAY_CLOCK_COUNT - 1,
    /* for checkpoint event */
    /* some of greater codes are reserved for checkpoints */
    EVENT_CHECKPOINT,
    EVENT_CHECKPOINT_LAST = EVENT_CHECKPOINT + CHECKPOINT_COUNT - 1,
    /* vmexit event (due to timer reads only) */
    EVENT_VMEXIT,
    /* network interrupts for virtio TX and RX */
    EVENT_NET_RX_INTERRUPT,
    EVENT_NET_TX_INTERRUPT, 
    /* end of log event */
    EVENT_END,
    EVENT_COUNT
};

extern uint16_t EVENT_BLK_INTERRUPT;

/* Asynchronous events IDs */

enum ReplayAsyncEventKind {
    REPLAY_ASYNC_EVENT_BH,
    REPLAY_ASYNC_EVENT_BH_ONESHOT,
    REPLAY_ASYNC_EVENT_INPUT,
    REPLAY_ASYNC_EVENT_INPUT_SYNC,
    REPLAY_ASYNC_EVENT_CHAR_READ,
    REPLAY_ASYNC_EVENT_BLOCK,
    REPLAY_ASYNC_EVENT_NET,
    REPLAY_ASYNC_COUNT
};

typedef enum ReplayAsyncEventKind ReplayAsyncEventKind;

typedef struct ReplayIOEvent {
    ReplayAsyncEventKind event_kind;
    void *opaque;
    uint64_t id;
} ReplayIOEvent;

typedef struct ReplayState {
    /*! Cached clock values. */
    int64_t cached_clock[REPLAY_CLOCK_COUNT];
    /*! Current icount - number of processed instructions. */
    uint64_t current_icount;
    /*! Number of instructions to be executed before other events happen. */
    int instruction_count;
    /*! Type of the currently executed event. */
    unsigned int data_kind;
    /*! Flag which indicates that event is not processed yet. */
    unsigned int has_unread_data;
    /*! Temporary variable for saving current log offset. */
    uint64_t file_offset;
    /*! Next block operation id.
        This counter is global, because requests from different
        block devices should not get overlapping ids. */
    uint64_t block_request_id;
    /*! Prior value of the host clock */
    uint64_t host_clock_last;
    /*! Asynchronous event type read from the log */
    int32_t read_event_kind;
    /*! Asynchronous event id read from the log */
    uint64_t read_event_id;
    /*! Asynchronous event checkpoint id read from the log */
    int32_t read_event_checkpoint;
} ReplayState;
extern ReplayState replay_state;

/* File for replay writing */
extern FILE *replay_file;

/* Arnab's files for replay writing */
extern FILE *arnab_clock_replay_file;
extern FILE *arnab_network_replay_file;
extern FILE *arnab_disk_replay_file;
extern FILE *arnab_host_clock_replay_file;

extern bool is_rx_queue_empty;

/* Files to record artifacts - maintain it here as well */
extern FILE *arnab_trace_insns_file;
extern FILE *arnab_trace_mem_file;

void replay_put_byte(uint8_t byte);
void arnab_replay_put_byte(uint8_t byte, const char *);

void replay_put_event(uint8_t event);
void arnab_replay_put_event(uint8_t event, const char *);

void replay_put_word(uint16_t word);
void arnab_replay_put_word(uint16_t word, const char *);

void replay_put_dword(uint32_t dword);
void arnab_replay_put_dword(uint32_t dword, const char *);

void replay_put_qword(int64_t qword);
void arnab_replay_put_qword(int64_t qword, const char *);

void replay_put_array(const uint8_t *buf, size_t size);
void arnab_replay_put_array(const uint8_t *buf, size_t size, const char *);

uint8_t replay_get_byte(void);
uint8_t arnab_replay_get_byte(const char *);

uint8_t arnab_replay_read_event(const char *);

uint16_t replay_get_word(void);
uint16_t arnab_replay_get_word(const char *);

uint32_t replay_get_dword(void);
uint32_t arnab_replay_get_dword(const char *);

int64_t replay_get_qword(void);
int64_t arnab_replay_get_qword(const char *);

void replay_get_array(uint8_t *buf, size_t *size);
void arnab_replay_get_array(uint8_t *buf, size_t *size, const char *);

void replay_get_array_alloc(uint8_t **buf, size_t *size);
void arnab_replay_get_array_alloc(uint8_t **buf, size_t *size, const char *);

/* Mutex functions for protecting replay log file and ensuring
 * synchronisation between vCPU and main-loop threads. */

void replay_mutex_init(void);
bool replay_mutex_locked(void);

/*! Checks error status of the file. */
void replay_check_error(void);

/*! Finishes processing of the replayed event and fetches
    the next event from the log. */
void replay_finish_event(void);
/*! Reads data type from the file and stores it in the
    data_kind variable. */
void replay_fetch_data_kind(void);

/*! Advance replay_state.current_icount to the specified value. */
void replay_advance_current_icount(uint64_t current_icount);
/*! Saves queued events (like instructions and sound). */
void replay_save_instructions(void);

/*! Skips async events until some sync event will be found.
    \return true, if event was found */
bool replay_next_event_is(int event);

/*! Reads next clock value from the file.
    If clock kind read from the file is different from the parameter,
    the value is not used. */
void replay_read_next_clock(unsigned int kind);

/* Asynchronous events queue */

/*! Initializes events' processing internals */
void replay_init_events(void);
/*! Clears internal data structures for events handling */
void replay_finish_events(void);
/*! Flushes events queue */
void replay_flush_events(void);
/*! Returns true if there are any unsaved events in the queue */
bool replay_has_events(void);
/*! Saves events from queue into the file */
void replay_save_events(int checkpoint);
/*! Read events from the file into the input queue */
void replay_read_events(int checkpoint);
/*! Adds specified async event to the queue */
void replay_add_event(ReplayAsyncEventKind event_kind, void *opaque,
                      void *opaque2, uint64_t id);

/* Input events */

/*! Saves input event to the log */
void replay_save_input_event(InputEvent *evt);
/*! Reads input event from the log */
InputEvent *replay_read_input_event(void);
/*! Adds input event to the queue */
void replay_add_input_event(struct InputEvent *event);
/*! Adds input sync event to the queue */
void replay_add_input_sync_event(void);

/* Character devices */

/*! Called to run char device read event. */
void replay_event_char_read_run(void *opaque);
/*! Writes char read event to the file. */
void replay_event_char_read_save(void *opaque);
/*! Reads char event read from the file. */
void *replay_event_char_read_load(void);

/* Network devices */

/*! Called to run network event. */
void replay_event_net_run(void *opaque);
/*! Writes network event to the file. */
void replay_event_net_save(void *opaque);
/*! Reads network from the file. */
void *replay_event_net_load(void);

void *arnab_replay_event_net_load(void);

bool arnab_replay_net_flush_rx_queue(void);

/* VMState-related functions */

/* Registers replay VMState.
   Should be called before virtual devices initialization
   to make cached timers available for post_load functions. */
void replay_vmstate_register(void);

#endif
