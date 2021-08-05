/*
 * replay-events.c
 *
 * Copyright (c) 2010-2015 Institute for System Programming
 *                         of the Russian Academy of Sciences.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */


/*
 * We have modified replay-events such that
 * only the network and disk events are
 * stored inside of the queue 
 *
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "sysemu/replay.h"
#include "replay-internal.h"
#include "block/aio.h"
#include "ui/input.h"
#include "index_array_header.h"

typedef struct Event {
    ReplayAsyncEventKind event_kind;
    void *opaque;
    void *opaque2;
    uint64_t id;

    QTAILQ_ENTRY(Event) events;
} Event;

QTAILQ_HEAD(, BlockEvent) blk_events_list = QTAILQ_HEAD_INITIALIZER(blk_events_list);

static QTAILQ_HEAD(, Event) events_list = QTAILQ_HEAD_INITIALIZER(events_list);
static bool events_enabled;

/* Functions */

/* network and disk events have been enabled only */
static void replay_run_event(Event *event)
{
    switch (event->event_kind) {
    case REPLAY_ASYNC_EVENT_BH:
        aio_bh_call(event->opaque);
        break;
    case REPLAY_ASYNC_EVENT_BH_ONESHOT:
        ((QEMUBHFunc *)event->opaque)(event->opaque2);
        break;
    case REPLAY_ASYNC_EVENT_INPUT:
        qemu_input_event_send_impl(NULL, (InputEvent *)event->opaque);
        qapi_free_InputEvent((InputEvent *)event->opaque);
        break;
    case REPLAY_ASYNC_EVENT_INPUT_SYNC:
        qemu_input_event_sync_impl();
        break;
    case REPLAY_ASYNC_EVENT_CHAR_READ:
        replay_event_char_read_run(event->opaque);
        break;
    case REPLAY_ASYNC_EVENT_BLOCK:
        aio_bh_call(event->opaque);
        break;
    case REPLAY_ASYNC_EVENT_NET:
        replay_event_net_run(event->opaque);
        break;
    default:
        error_report("Replay: invalid async event ID (%d) in the queue",
                    event->event_kind);
        exit(1);
        break;
    }
}

void replay_enable_events(void)
{
    if (replay_mode != REPLAY_MODE_NONE) {
        events_enabled = true;
    }
}

bool replay_has_events(void)
{
    return !QTAILQ_EMPTY(&events_list);
}

void replay_flush_events(void)
{
    //g_assert(replay_mutex_locked());

    while (!QTAILQ_EMPTY(&events_list)) {
        Event *event = QTAILQ_FIRST(&events_list);
        replay_run_event(event);
        QTAILQ_REMOVE(&events_list, event, events);
        g_free(event);
    }
}

void replay_disable_events(void)
{
    if (replay_mode != REPLAY_MODE_NONE) {
        events_enabled = false;
        /* Flush events queue before waiting of completion */
        replay_flush_events();
    }
}

/*! Adds specified async event to the queue */
void replay_add_event(ReplayAsyncEventKind event_kind,
                      void *opaque,
                      void *opaque2, uint64_t id)
{
    assert(event_kind < REPLAY_ASYNC_COUNT);

    if (!replay_file || replay_mode == REPLAY_MODE_NONE
        || !events_enabled) {
        Event e;
        e.event_kind = event_kind;
        e.opaque = opaque;
        e.opaque2 = opaque2;
        e.id = id;
        replay_run_event(&e);
        return;
    }

    Event *event = g_malloc0(sizeof(Event));
    event->event_kind = event_kind;
    event->opaque = opaque;
    event->opaque2 = opaque2;
    event->id = id;

    //g_assert(replay_mutex_locked());
    QTAILQ_INSERT_TAIL(&events_list, event, events);	
}

void replay_add_block_event(void *opaque, void *opaque2, uint64_t id)
{
    BlockEvent *blk_event = g_malloc0(sizeof(BlockEvent));

    blk_event->opaque = opaque;
    blk_event->opaque2 = opaque2;
    blk_event->id = id;

    QTAILQ_INSERT_TAIL(&blk_events_list, blk_event, blk_events);
}

void replay_bh_schedule_event(QEMUBH *bh)
{
    if (events_enabled) {
        uint64_t id = replay_get_current_icount();
        replay_add_event(REPLAY_ASYNC_EVENT_BH, bh, NULL, id);
    } else {
        qemu_bh_schedule(bh);
    }
}

void replay_bh_schedule_oneshot_event(AioContext *ctx,
    QEMUBHFunc *cb, void *opaque)
{
    if (events_enabled) {
        uint64_t id = replay_get_current_icount();
        replay_add_event(REPLAY_ASYNC_EVENT_BH_ONESHOT, cb, opaque, id);
    } else {
        aio_bh_schedule_oneshot(ctx, cb, opaque);
    }
}

void replay_add_input_event(struct InputEvent *event)
{
    replay_add_event(REPLAY_ASYNC_EVENT_INPUT, event, NULL, 0);
}

void replay_add_input_sync_event(void)
{
    replay_add_event(REPLAY_ASYNC_EVENT_INPUT_SYNC, NULL, NULL, 0);
}

void replay_block_event(QEMUBH *bh, uint64_t id)
{
    if (arnab_replay_mode == REPLAY_MODE_RECORD) {
        // write to file here (instead of writing to queue)
        if (start_recording) {
            if (arnab_disk_replay_file) {
                if (arnab_replay_mode == REPLAY_MODE_RECORD) {
                    arnab_replay_put_qword(id, "disk");
                }
            }
        }
    }
    /* TODO: replay*/
    qemu_bh_schedule(bh);
}

static void replay_save_event(Event *event, int checkpoint)
{
    if (replay_mode != REPLAY_MODE_PLAY) {
        /* put the event into the file */
        replay_put_event(EVENT_ASYNC);
        replay_put_byte(checkpoint);
        replay_put_byte(event->event_kind);

        /* save event-specific data */
        switch (event->event_kind) {
        
        case REPLAY_ASYNC_EVENT_BH:
        case REPLAY_ASYNC_EVENT_BH_ONESHOT:
            replay_put_qword(event->id);
            break;
        case REPLAY_ASYNC_EVENT_INPUT:
            replay_save_input_event(event->opaque);
            break;
        case REPLAY_ASYNC_EVENT_INPUT_SYNC:
            break;
        case REPLAY_ASYNC_EVENT_CHAR_READ:
            replay_event_char_read_save(event->opaque);
            break;
        case REPLAY_ASYNC_EVENT_BLOCK:
            replay_put_qword(event->id);
            break;
        case REPLAY_ASYNC_EVENT_NET:
	    //printf("REPLAY_ASYNC_EVENT_NET\n");
            replay_event_net_save(event->opaque);
            break;
        default:
            error_report("Unknown ID %" PRId64 " of replay event", event->id);
            exit(1);
        }
    }
}

/* Called with replay mutex locked - disable all locks */
void replay_save_events(int checkpoint)
{
    //g_assert(replay_mutex_locked());
    g_assert(checkpoint != CHECKPOINT_CLOCK_WARP_START);
    g_assert(checkpoint != CHECKPOINT_CLOCK_VIRTUAL);
    while (!QTAILQ_EMPTY(&events_list)) {
        Event *event = QTAILQ_FIRST(&events_list);
	    
        replay_save_event(event, checkpoint);      // nothing is written to the file
	replay_run_event(event);
        QTAILQ_REMOVE(&events_list, event, events);
        g_free(event);
    }
}

BlockEvent *replay_read_block_event(void)
{
    BlockEvent *event;
    uint64_t event_id;
    QTAILQ_FOREACH(event, &blk_events_list, blk_events) {
        event_id = arnab_replay_get_qword("disk");
        if (event_id == EVENT_BLK_INTERRUPT) {
            return NULL;
        }
        if (event_id == event->id) {
            break;
        }
    }
    if (event) {
        QTAILQ_REMOVE(&blk_events_list, event, blk_events);
    } else {
        return NULL;
    }
    return event;
}

static Event *replay_read_event(int checkpoint)
{
    Event *event;
    if (replay_state.read_event_kind == -1) {
        replay_state.read_event_checkpoint = replay_get_byte();
        replay_state.read_event_kind = replay_get_byte();
        replay_state.read_event_id = -1;
        replay_check_error();
    }

    if (checkpoint != replay_state.read_event_checkpoint) {
        return NULL;
    }

    /* Events that has not to be in the queue */
    switch (replay_state.read_event_kind) {
    case REPLAY_ASYNC_EVENT_BH:
    case REPLAY_ASYNC_EVENT_BH_ONESHOT:
        if (replay_state.read_event_id == -1) {
            replay_state.read_event_id = replay_get_qword();
        }
        break;
    case REPLAY_ASYNC_EVENT_INPUT:
        event = g_malloc0(sizeof(Event));
        event->event_kind = replay_state.read_event_kind;
        event->opaque = replay_read_input_event();
        return event;
    case REPLAY_ASYNC_EVENT_INPUT_SYNC:
        event = g_malloc0(sizeof(Event));
        event->event_kind = replay_state.read_event_kind;
        event->opaque = 0;
        return event;
    case REPLAY_ASYNC_EVENT_CHAR_READ:
        event = g_malloc0(sizeof(Event));
        event->event_kind = replay_state.read_event_kind;
        event->opaque = replay_event_char_read_load();
        return event;
    case REPLAY_ASYNC_EVENT_BLOCK:
        if (replay_state.read_event_id == -1) {
            replay_state.read_event_id = replay_get_qword();
        }
        break;
    case REPLAY_ASYNC_EVENT_NET:
        event = g_malloc0(sizeof(Event));
        event->event_kind = replay_state.read_event_kind;
        event->opaque = replay_event_net_load();
        return event;
    default:
        error_report("Unknown ID %d of replay event",
            replay_state.read_event_kind);
        exit(1);
        break;
    }

    QTAILQ_FOREACH(event, &events_list, events) {
        if (event->event_kind == replay_state.read_event_kind
            && (replay_state.read_event_id == -1
                || replay_state.read_event_id == event->id)) {
            break;
        }
    }

    if (event) {
        QTAILQ_REMOVE(&events_list, event, events);
    } else {
        return NULL;
    }

    /* Read event-specific data */

    return event;
}

/* Called with replay mutex locked */
void replay_read_events(int checkpoint)
{
    //g_assert(replay_mutex_locked());
    while (replay_state.data_kind == EVENT_ASYNC) {
        Event *event = replay_read_event(checkpoint);
        if (!event) {
            break;
        }
        replay_finish_event();
        replay_state.read_event_kind = -1;

        replay_run_event(event);
	g_free(event);
    }
}

void replay_init_events(void)
{
    replay_state.read_event_kind = -1;
}

void replay_finish_events(void)
{
    events_enabled = false;
    replay_flush_events();
}

bool replay_events_enabled(void)
{
    return events_enabled;
}

static uint64_t block_request_id = 0;

uint64_t blkreplay_next_id(void)
{
    if (arnab_replay_mode == REPLAY_MODE_RECORD) {
        if (start_recording) {
            return block_request_id++;
        }
    }
    else if (arnab_replay_mode == REPLAY_MODE_PLAY) {
        return block_request_id++;
    }
    return 0;
}
