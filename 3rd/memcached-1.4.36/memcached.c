/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *  memcached - memory caching daemon
 *
 *       http://www.memcached.org/
 *
 *  Copyright 2003 Danga Interactive, Inc.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Anatoly Vorobey <mellon@pobox.com>
 *      Brad Fitzpatrick <brad@danga.com>
 */
#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <ctype.h>
#include <stdarg.h>

/* some POSIX systems need the following definition
 * to get mlockall flags out of sys/mman.h.  */
#ifndef _P1003_1B_VISIBLE
#define _P1003_1B_VISIBLE
#endif
/* need this to get IOV_MAX on some platforms. */
#ifndef __need_IOV_MAX
#define __need_IOV_MAX
#endif
#include <pwd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <limits.h>
#include <sysexits.h>
#include <stddef.h>

/* FreeBSD 4.x doesn't have IOV_MAX exposed. */
#ifndef IOV_MAX
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__GNU__)
# define IOV_MAX 1024
/* GNU/Hurd don't set MAXPATHLEN
 * http://www.gnu.org/software/hurd/hurd/porting/guidelines.html#PATH_MAX_tt_MAX_PATH_tt_MAXPATHL */
#ifndef MAXPATHLEN
#define MAXPATHLEN 4096
#endif
#endif
#endif

/*
 * forward declarations
 */
//static void drive_machine(conn *c);
static int new_socket(struct addrinfo *ai);
static int try_read_command(conn *c);

enum try_read_result {
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,            /** an error occurred (on the socket) (or client closed connection) */
    READ_MEMORY_ERROR      /** failed to allocate more memory */
};

static enum try_read_result try_read_network(conn *c);
static enum try_read_result try_read_udp(conn *c);

static void conn_set_state(conn *c, enum conn_states state);
//static int start_conn_timeout_thread();

/* stats */
static void stats_init(void);
static void server_stats(ADD_STAT add_stats, conn *c);
static void process_stat_settings(ADD_STAT add_stats, void *c);
static void conn_to_str(const conn *c, char *buf);


/* defaults */
static void settings_init(void);

/* event handling, network IO */
//static void event_handler(const int fd, const short which, void *arg);
//static void conn_close(conn *c);
//static void conn_init(void);
//static bool update_event(conn *c, const int new_flags);
//static void complete_nread(conn *c);
static void process_command(conn *c, char *command);
//static void write_and_free(conn *c, char *buf, int bytes);
static int ensure_iov_space(conn *c);
//static int add_iov(conn *c, const void *buf, int len);
static int add_chunked_item_iovs(conn *c, item *it, int len);
//static int add_msghdr(conn *c);
//static void write_bin_error(conn *c, protocol_binary_response_status err,
//                            const char *errstr, int swallow);

static void conn_free(conn *c);

/** exported globals **/
struct stats stats;
struct stats_state stats_state;
struct settings settings;
time_t process_started;     /* when the process was started */
conn **conns;

struct slab_rebalance slab_rebal;
volatile int slab_rebalance_signal;

/** file scope variables **/
static conn *listen_conn = NULL;
static int max_fds;
static struct event_base *main_base;

enum transmit_result {
    TRANSMIT_COMPLETE,   /** All done writing. */
    TRANSMIT_INCOMPLETE, /** More data remaining to write. */
    TRANSMIT_SOFT_ERROR, /** Can't write any more right now. */
    TRANSMIT_HARD_ERROR  /** Can't write (c->state is set to conn_closing) */
};

static enum transmit_result transmit(conn *c);

/* This reduces the latency without adding lots of extra wiring to be able to
 * notify the listener thread of when to listen again.
 * Also, the clock timer could be broken out into its own thread and we
 * can block the listener via a condition.
 */
static volatile bool allow_new_conns = true;
static struct event maxconnsevent;
//static void maxconns_handler(const int fd, const short which, void *arg) {
//    struct timeval t = {.tv_sec = 0, .tv_usec = 10000};
//
//    if (fd == -42 || allow_new_conns == false) {
//        /* reschedule in 10ms if we need to keep polling */
//        evtimer_set(&maxconnsevent, maxconns_handler, 0);
//        event_base_set(main_base, &maxconnsevent);
//        evtimer_add(&maxconnsevent, &t);
//    } else {
//        evtimer_del(&maxconnsevent);
//        accept_new_conns(true);
//    }
//}

#define REALTIME_MAXDELTA 60*60*24*30
static void out_string(conn *c, const char *str) {
    fprintf(stderr, "%s\n", str);
}

/*
 * given time value that's either unix time or delta from current unix time, return
 * unix time. Use the fact that delta can't exceed one month (and real time value can't
 * be that low).
 */
static rel_time_t realtime(const time_t exptime) {
    /* no. of seconds in 30 days - largest possible delta exptime */

    if (exptime == 0) return 0; /* 0 means never expire */

    if (exptime > REALTIME_MAXDELTA) {
        /* if item expiration is at/before the server started, give it an
           expiration time of 1 second after the server started.
           (because 0 means don't expire).  without this, we'd
           underflow and wrap around to some large value way in the
           future, effectively making items expiring in the past
           really expiring never */
        if (exptime <= process_started)
            return (rel_time_t)1;
        return (rel_time_t)(exptime - process_started);
    } else {
        return (rel_time_t)(exptime + current_time);
    }
}

static void stats_init(void) {
    memset(&stats, 0, sizeof(struct stats));
    memset(&stats_state, 0, sizeof(struct stats_state));
    stats_state.accepting_conns = true; /* assuming we start in this state. */

    /* make the time we started always be 2 seconds before we really
       did, so time(0) - time.started is never zero.  if so, things
       like 'settings.oldest_live' which act as booleans as well as
       values are now false in boolean context... */
    process_started = time(0) - ITEM_UPDATE_INTERVAL - 2;
    stats_prefix_init();
}

static void stats_reset(void) {
    STATS_LOCK();
    memset(&stats, 0, sizeof(struct stats));
    stats_prefix_clear();
    STATS_UNLOCK();
    threadlocal_stats_reset();
    item_stats_reset();
}

static void settings_init(void) {
    settings.use_cas = true;
    settings.access = 0700;
    settings.port = 11211;
    settings.udpport = 11211;
    /* By default this string should be NULL for getaddrinfo() */
    settings.inter = NULL;
    settings.maxbytes = 8 * 1024l * 1024l * 1024l; /* default is now 8 Gigabytes */
    settings.maxconns = 1024;         /* to limit connections-related memory to about 5MB */
    settings.verbose = 0;
    settings.oldest_live = 0;
    settings.oldest_cas = 0;          /* supplements accuracy of oldest_live */
    settings.evict_to_free = 1;       /* push old items out of cache when memory runs out */
    settings.socketpath = NULL;       /* by default, not using a unix socket */
    settings.factor = 1.25;
    settings.chunk_size = 1024*1024 - sizeof(item);         /* space for a modest key and value */
    settings.num_threads = 4;         /* N workers */
    settings.num_threads_per_udp = 0;
    settings.prefix_delimiter = ':';
    settings.detail_enabled = 0;
    settings.reqs_per_event = 20;
    settings.backlog = 1024;
    settings.binding_protocol = negotiating_prot;
    settings.item_size_max = 1024 * 1024 * 1024; /* The famous 1MB upper limit. */
    settings.slab_page_size = 1024 * 1024 * 1024; /* chunks are split from 1MB pages. */
    settings.slab_chunk_size_max = settings.slab_page_size;
    settings.sasl = false;
    settings.maxconns_fast = false;
    settings.lru_crawler = false;
    settings.lru_crawler_sleep = 100;
    settings.lru_crawler_tocrawl = 0;
    settings.lru_maintainer_thread = false;
    settings.lru_segmented = false;
    settings.hot_lru_pct = 32;
    settings.warm_lru_pct = 32;
    settings.hot_max_age = 3600;
    settings.warm_max_factor = 2.0;
    settings.inline_ascii_response = true;
    settings.temp_lru = false;
    settings.temporary_ttl = 61;
    settings.idle_timeout = 0; /* disabled */
    settings.hashpower_init = 0;
    settings.slab_reassign = false;
    settings.slab_automove = 0;
    settings.shutdown_command = false;
    settings.tail_repair_time = TAIL_REPAIR_TIME_DEFAULT;
    settings.flush_enabled = true;
    settings.dump_enabled = true;
    settings.crawls_persleep = 1000;
    settings.logger_watcher_buf_size = LOGGER_WATCHER_BUF_SIZE;
    settings.logger_buf_size = LOGGER_BUF_SIZE;
}


/* Connection timeout thread bits */
static pthread_t conn_timeout_tid;

#define CONNS_PER_SLICE 100
#define TIMEOUT_MSG_SIZE (1 + sizeof(int))
/**
 * Convert a state name to a human readable form.
 */
static const char *state_text(enum conn_states state) {
    const char* const statenames[] = { "conn_listening",
                                       "conn_new_cmd",
                                       "conn_waiting",
                                       "conn_read",
                                       "conn_parse_cmd",
                                       "conn_write",
                                       "conn_nread",
                                       "conn_swallow",
                                       "conn_closing",
                                       "conn_mwrite",
                                       "conn_closed",
                                       "conn_watch" };
    return statenames[state];
}

/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
static void conn_set_state(conn *c, enum conn_states state) {
    assert(c != NULL);
    assert(state >= conn_listening && state < conn_max_state);

    if (state != c->state) {
        if (settings.verbose > 2) {
            fprintf(stderr, "%d: going from %s to %s\n",
                    c->sfd, state_text(c->state),
                    state_text(state));
        }

        if (state == conn_write || state == conn_mwrite) {
            MEMCACHED_PROCESS_COMMAND_END(c->sfd, c->wbuf, c->wbytes);
        }
        c->state = state;
    }
}

/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int ensure_iov_space(conn *c) {
    assert(c != NULL);

    if (c->iovused >= c->iovsize) {
        int i, iovnum;
        struct iovec *new_iov = (struct iovec *)realloc(c->iov,
                                (c->iovsize * 2) * sizeof(struct iovec));
        if (! new_iov) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->iov = new_iov;
        c->iovsize *= 2;

        /* Point all the msghdr structures at the new list. */
        for (i = 0, iovnum = 0; i < c->msgused; i++) {
            c->msglist[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msglist[i].msg_iovlen;
        }
    }

    return 0;
}



static void append_bin_stats(const char *key, const uint16_t klen,
                             const char *val, const uint32_t vlen,
                             conn *c) {
    char *buf = c->stats.buffer + c->stats.offset;
    uint32_t bodylen = klen + vlen;
    protocol_binary_response_header header = {
        .response.magic = (uint8_t)PROTOCOL_BINARY_RES,
        .response.opcode = PROTOCOL_BINARY_CMD_STAT,
        .response.keylen = (uint16_t)htons(klen),
        .response.datatype = (uint8_t)PROTOCOL_BINARY_RAW_BYTES,
        .response.bodylen = htonl(bodylen),
        .response.opaque = c->opaque
    };

    memcpy(buf, header.bytes, sizeof(header.response));
    buf += sizeof(header.response);

    if (klen > 0) {
        memcpy(buf, key, klen);
        buf += klen;

        if (vlen > 0) {
            memcpy(buf, val, vlen);
        }
    }

    c->stats.offset += sizeof(header.response) + bodylen;
}

static void append_ascii_stats(const char *key, const uint16_t klen,
                               const char *val, const uint32_t vlen,
                               conn *c) {
    char *pos = c->stats.buffer + c->stats.offset;
    uint32_t nbytes = 0;
    int remaining = c->stats.size - c->stats.offset;
    int room = remaining - 1;

    if (klen == 0 && vlen == 0) {
        nbytes = snprintf(pos, room, "END\r\n");
    } else if (vlen == 0) {
        nbytes = snprintf(pos, room, "STAT %s\r\n", key);
    } else {
        nbytes = snprintf(pos, room, "STAT %s %s\r\n", key, val);
    }

    c->stats.offset += nbytes;
}

static bool grow_stats_buf(conn *c, size_t needed) {
    size_t nsize = c->stats.size;
    size_t available = nsize - c->stats.offset;
    bool rv = true;

    /* Special case: No buffer -- need to allocate fresh */
    if (c->stats.buffer == NULL) {
        nsize = 1024;
        available = c->stats.size = c->stats.offset = 0;
    }

    while (needed > available) {
        assert(nsize > 0);
        nsize = nsize << 1;
        available = nsize - c->stats.offset;
    }

    if (nsize != c->stats.size) {
        char *ptr = realloc(c->stats.buffer, nsize);
        if (ptr) {
            c->stats.buffer = ptr;
            c->stats.size = nsize;
        } else {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            rv = false;
        }
    }

    return rv;
}

static void append_stats(const char *key, const uint16_t klen,
                  const char *val, const uint32_t vlen,
                  const void *cookie)
{
    /* value without a key is invalid */
    if (klen == 0 && vlen > 0) {
        return ;
    }

    conn *c = (conn*)cookie;

    if (c->protocol == binary_prot) {
        size_t needed = vlen + klen + sizeof(protocol_binary_response_header);
        if (!grow_stats_buf(c, needed)) {
            return ;
        }
        append_bin_stats(key, klen, val, vlen, c);
    } else {
        size_t needed = vlen + klen + 10; // 10 == "STAT = \r\n"
        if (!grow_stats_buf(c, needed)) {
            return ;
        }
        append_ascii_stats(key, klen, val, vlen, c);
    }

    assert(c->stats.offset <= c->stats.size);
}


static void bin_read_key(conn *c, enum bin_substates next_substate, int extra) {
    assert(c);
    c->substate = next_substate;
    c->rlbytes = c->keylen + extra;

    /* Ok... do we have room for the extras and the key in the input buffer? */
    ptrdiff_t offset = c->rcurr + sizeof(protocol_binary_request_header) - c->rbuf;
    if (c->rlbytes > c->rsize - offset) {
        size_t nsize = c->rsize;
        size_t size = c->rlbytes + sizeof(protocol_binary_request_header);

        while (size > nsize) {
            nsize *= 2;
        }

        if (nsize != c->rsize) {
            if (settings.verbose > 1) {
                fprintf(stderr, "%d: Need to grow buffer from %lu to %lu\n",
                        c->sfd, (unsigned long)c->rsize, (unsigned long)nsize);
            }
            char *newm = realloc(c->rbuf, nsize);
            if (newm == NULL) {
                STATS_LOCK();
                stats.malloc_fails++;
                STATS_UNLOCK();
                if (settings.verbose) {
                    fprintf(stderr, "%d: Failed to grow buffer.. closing connection\n",
                            c->sfd);
                }
                conn_set_state(c, conn_closing);
                return;
            }

            c->rbuf= newm;
            /* rcurr should point to the same offset in the packet */
            c->rcurr = c->rbuf + offset - sizeof(protocol_binary_request_header);
            c->rsize = nsize;
        }
        if (c->rbuf != c->rcurr) {
            memmove(c->rbuf, c->rcurr, c->rbytes);
            c->rcurr = c->rbuf;
            if (settings.verbose > 1) {
                fprintf(stderr, "%d: Repack input buffer\n", c->sfd);
            }
        }
    }

    /* preserve the header in the buffer.. */
    c->ritem = c->rcurr + sizeof(protocol_binary_request_header);
    conn_set_state(c, conn_nread);
}


//static void reset_cmd_handler(conn *c) {
//    c->cmd = -1;
//    c->substate = bin_no_state;
//    if(c->item != NULL) {
//        item_remove(c->item);
//        c->item = NULL;
//    }
//    conn_shrink(c);
//    if (c->rbytes > 0) {
//        conn_set_state(c, conn_parse_cmd);
//    } else {
//        conn_set_state(c, conn_waiting);
//    }
//}

//static void complete_nread(conn *c) {
//    assert(c != NULL);
//    assert(c->protocol == ascii_prot
//           || c->protocol == binary_prot);
//
//    if (c->protocol == ascii_prot) {
//        complete_nread_ascii(c);
//    } else if (c->protocol == binary_prot) {
//        complete_nread_binary(c);
//    }
//}

/* Destination must always be chunked */
/* This should be part of item.c */
static int _store_item_copy_chunks(item *d_it, item *s_it, const int len) {
    item_chunk *dch = (item_chunk *) ITEM_data(d_it);
    /* Advance dch until we find free space */
    while (dch->size == dch->used) {
        if (dch->next) {
            dch = dch->next;
        } else {
            break;
        }
    }

    if (s_it->it_flags & ITEM_CHUNKED) {
        int remain = len;
        item_chunk *sch = (item_chunk *) ITEM_data(s_it);
        int copied = 0;
        /* Fills dch's to capacity, not straight copy sch in case data is
         * being added or removed (ie append/prepend)
         */
        while (sch && dch && remain) {
            assert(dch->used <= dch->size);
            int todo = (dch->size - dch->used < sch->used - copied)
                ? dch->size - dch->used : sch->used - copied;
            if (remain < todo)
                todo = remain;
            memcpy(dch->data + dch->used, sch->data + copied, todo);
            dch->used += todo;
            copied += todo;
            remain -= todo;
            assert(dch->used <= dch->size);
            if (dch->size == dch->used) {
                item_chunk *tch = do_item_alloc_chunk(dch, remain);
                if (tch) {
                    dch = tch;
                } else {
                    return -1;
                }
            }
            assert(copied <= sch->used);
            if (copied == sch->used) {
                copied = 0;
                sch = sch->next;
            }
        }
        /* assert that the destination had enough space for the source */
        assert(remain == 0);
    } else {
        int done = 0;
        /* Fill dch's via a non-chunked item. */
        while (len > done && dch) {
            int todo = (dch->size - dch->used < len - done)
                ? dch->size - dch->used : len - done;
            //assert(dch->size - dch->used != 0);
            memcpy(dch->data + dch->used, ITEM_data(s_it) + done, todo);
            done += todo;
            dch->used += todo;
            assert(dch->used <= dch->size);
            if (dch->size == dch->used) {
                item_chunk *tch = do_item_alloc_chunk(dch, len - done);
                if (tch) {
                    dch = tch;
                } else {
                    return -1;
                }
            }
        }
        assert(len == done);
    }
    return 0;
}

static int _store_item_copy_data(int comm, item *old_it, item *new_it, item *add_it) {
    if (comm == NREAD_APPEND) {
        if (new_it->it_flags & ITEM_CHUNKED) {
            if (_store_item_copy_chunks(new_it, old_it, old_it->nbytes - 2) == -1 ||
                _store_item_copy_chunks(new_it, add_it, add_it->nbytes) == -1) {
                return -1;
            }
        } else {
            memcpy(ITEM_data(new_it), ITEM_data(old_it), old_it->nbytes);
            memcpy(ITEM_data(new_it) + old_it->nbytes - 2 /* CRLF */, ITEM_data(add_it), add_it->nbytes);
        }
    } else {
        /* NREAD_PREPEND */
        if (new_it->it_flags & ITEM_CHUNKED) {
            if (_store_item_copy_chunks(new_it, add_it, add_it->nbytes - 2) == -1 ||
                _store_item_copy_chunks(new_it, old_it, old_it->nbytes) == -1) {
                return -1;
            }
        } else {
            memcpy(ITEM_data(new_it), ITEM_data(add_it), add_it->nbytes);
            memcpy(ITEM_data(new_it) + add_it->nbytes - 2 /* CRLF */, ITEM_data(old_it), old_it->nbytes);
        }
    }
    return 0;
}

/*
 * Stores an item in the cache according to the semantics of one of the set
 * commands. In threaded mode, this is protected by the cache lock.
 *
 * Returns the state of storage.
 */
enum store_item_type do_store_item(item *it, int comm, conn *c, const uint32_t hv) {
    char *key = ITEM_key(it);
    item *old_it = do_item_get(key, it->nkey, hv, c, DONT_UPDATE);
    enum store_item_type stored = NOT_STORED;

    item *new_it = NULL;
    uint32_t flags;

    if (old_it != NULL && comm == NREAD_ADD) {
        /* add only adds a nonexistent item, but promote to head of LRU */
        do_item_update(old_it);
    } else if (!old_it && (comm == NREAD_REPLACE
        || comm == NREAD_APPEND || comm == NREAD_PREPEND))
    {
        /* replace only replaces an existing value; don't store */
    } else if (comm == NREAD_CAS) {
        /* validate cas operation */
        if(old_it == NULL) {
            // LRU expired
            stored = NOT_FOUND;
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.cas_misses++;
            pthread_mutex_unlock(&c->thread->stats.mutex);
        }
        else if (ITEM_get_cas(it) == ITEM_get_cas(old_it)) {
            // cas validates
            // it and old_it may belong to different classes.
            // I'm updating the stats for the one that's getting pushed out
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.slab_stats[ITEM_clsid(old_it)].cas_hits++;
            pthread_mutex_unlock(&c->thread->stats.mutex);

            item_replace(old_it, it, hv);
            stored = STORED;
        } else {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.slab_stats[ITEM_clsid(old_it)].cas_badval++;
            pthread_mutex_unlock(&c->thread->stats.mutex);

            if(settings.verbose > 1) {
                fprintf(stderr, "CAS:  failure: expected %llu, got %llu\n",
                        (unsigned long long)ITEM_get_cas(old_it),
                        (unsigned long long)ITEM_get_cas(it));
            }
            stored = EXISTS;
        }
    } else {
        int failed_alloc = 0;
        /*
         * Append - combine new and old record into single one. Here it's
         * atomic and thread-safe.
         */
        if (comm == NREAD_APPEND || comm == NREAD_PREPEND) {
            /*
             * Validate CAS
             */
            if (ITEM_get_cas(it) != 0) {
                // CAS much be equal
                if (ITEM_get_cas(it) != ITEM_get_cas(old_it)) {
                    stored = EXISTS;
                }
            }

            if (stored == NOT_STORED) {
                /* we have it and old_it here - alloc memory to hold both */
                /* flags was already lost - so recover them from ITEM_suffix(it) */

                if (settings.inline_ascii_response) {
                    flags = (uint32_t) strtoul(ITEM_suffix(old_it), (char **) NULL, 10);
                } else {
                    flags = *((uint32_t *)ITEM_suffix(old_it));
                }

                new_it = do_item_alloc(key, it->nkey, flags, old_it->exptime, it->nbytes + old_it->nbytes - 2 /* CRLF */);

                /* copy data from it and old_it to new_it */
                if (new_it == NULL || _store_item_copy_data(comm, old_it, new_it, it) == -1) {
                    failed_alloc = 1;
                    stored = NOT_STORED;
                    // failed data copy, free up.
                    if (new_it != NULL)
                        item_remove(new_it);
                } else {
                    it = new_it;
                }
            }
        }

        if (stored == NOT_STORED && failed_alloc == 0) {
            if (old_it != NULL)
                item_replace(old_it, it, hv);
            else
                do_item_link(it, hv);

            c->cas = ITEM_get_cas(it);

            stored = STORED;
        }
    }

    if (old_it != NULL)
        do_item_remove(old_it);         /* release our reference */
    if (new_it != NULL)
        do_item_remove(new_it);

    if (stored == STORED) {
        c->cas = ITEM_get_cas(it);
    }
    LOGGER_LOG(c->thread->l, LOG_MUTATIONS, LOGGER_ITEM_STORE, NULL,
            stored, comm, ITEM_key(it), it->nkey, it->exptime, ITEM_clsid(it));

    return stored;
}

typedef struct token_s {
    char *value;
    size_t length;
} token_t;

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1
#define KEY_TOKEN 1

#define MAX_TOKENS 8

/*
 * Tokenize the command string by replacing whitespace with '\0' and update
 * the token array tokens with pointer to start of each token and length.
 * Returns total number of tokens.  The last valid token is the terminal
 * token (value points to the first unprocessed character of the string and
 * length zero).
 *
 * Usage example:
 *
 *  while(tokenize_command(command, ncommand, tokens, max_tokens) > 0) {
 *      for(int ix = 0; tokens[ix].length != 0; ix++) {
 *          ...
 *      }
 *      ncommand = tokens[ix].value - command;
 *      command  = tokens[ix].value;
 *   }
 */
static size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens) {
    char *s, *e;
    size_t ntokens = 0;
    size_t len = strlen(command);
    unsigned int i = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    s = e = command;
    for (i = 0; i < len; i++) {
        if (*e == ' ') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
                *e = '\0';
                if (ntokens == max_tokens - 1) {
                    e++;
                    s = e; /* so we don't add an extra token */
                    break;
                }
            }
            s = e + 1;
        }
        e++;
    }

    if (s != e) {
        tokens[ntokens].value = s;
        tokens[ntokens].length = e - s;
        ntokens++;
    }

    /*
     * If we scanned the whole string, the terminal value pointer is null,
     * otherwise it is the first unprocessed character.
     */
    tokens[ntokens].value =  *e == '\0' ? NULL : e;
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}

/* set up a connection to write a buffer then free it, used for stats */
//static void write_and_free(conn *c, char *buf, int bytes) {
//    if (buf) {
//        c->write_and_free = buf;
//        c->wcurr = buf;
//        c->wbytes = bytes;
//        conn_set_state(c, conn_write);
//        c->write_and_go = conn_new_cmd;
//    } else {
//        out_of_memory(c, "SERVER_ERROR out of memory writing stats");
//    }
//}

static inline bool set_noreply_maybe(conn *c, token_t *tokens, size_t ntokens)
{
    int noreply_index = ntokens - 2;

    /*
      NOTE: this function is not the first place where we are going to
      send the reply.  We could send it instead from process_command()
      if the request line has wrong number of tokens.  However parsing
      malformed line for "noreply" option is not reliable anyway, so
      it can't be helped.
    */
    if (tokens[noreply_index].value
        && strcmp(tokens[noreply_index].value, "noreply") == 0) {
        c->noreply = true;
    }
    return c->noreply;
}

void append_stat(const char *name, ADD_STAT add_stats, conn *c,
                 const char *fmt, ...) {
    char val_str[STAT_VAL_LEN];
    int vlen;
    va_list ap;

    assert(name);
    assert(add_stats);
    assert(c);
    assert(fmt);

    va_start(ap, fmt);
    vlen = vsnprintf(val_str, sizeof(val_str) - 1, fmt, ap);
    va_end(ap);

    add_stats(name, strlen(name), val_str, vlen, c);
}


/* return server specific stats only */
static void server_stats(ADD_STAT add_stats, conn *c) {
    pid_t pid = getpid();
    rel_time_t now = current_time;

    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats);
    struct slab_stats slab_stats;
    slab_stats_aggregate(&thread_stats, &slab_stats);

#ifndef WIN32
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
#endif /* !WIN32 */

    STATS_LOCK();

    APPEND_STAT("pid", "%lu", (long)pid);
    APPEND_STAT("uptime", "%u", now - ITEM_UPDATE_INTERVAL);
    APPEND_STAT("time", "%ld", now + (long)process_started);
    //APPEND_STAT("version", "%s", VERSION);
    //APPEND_STAT("libevent", "%s", event_get_version());
    APPEND_STAT("pointer_size", "%d", (int)(8 * sizeof(void *)));

#ifndef WIN32
    append_stat("rusage_user", add_stats, c, "%ld.%06ld",
                (long)usage.ru_utime.tv_sec,
                (long)usage.ru_utime.tv_usec);
    append_stat("rusage_system", add_stats, c, "%ld.%06ld",
                (long)usage.ru_stime.tv_sec,
                (long)usage.ru_stime.tv_usec);
#endif /* !WIN32 */

    APPEND_STAT("curr_connections", "%llu", (unsigned long long)stats_state.curr_conns - 1);
    APPEND_STAT("total_connections", "%llu", (unsigned long long)stats.total_conns);
    if (settings.maxconns_fast) {
        APPEND_STAT("rejected_connections", "%llu", (unsigned long long)stats.rejected_conns);
    }
    APPEND_STAT("connection_structures", "%u", stats_state.conn_structs);
    APPEND_STAT("reserved_fds", "%u", stats_state.reserved_fds);
    APPEND_STAT("cmd_get", "%llu", (unsigned long long)thread_stats.get_cmds);
    APPEND_STAT("cmd_set", "%llu", (unsigned long long)slab_stats.set_cmds);
    APPEND_STAT("cmd_flush", "%llu", (unsigned long long)thread_stats.flush_cmds);
    APPEND_STAT("cmd_touch", "%llu", (unsigned long long)thread_stats.touch_cmds);
    APPEND_STAT("get_hits", "%llu", (unsigned long long)slab_stats.get_hits);
    APPEND_STAT("get_misses", "%llu", (unsigned long long)thread_stats.get_misses);
    APPEND_STAT("get_expired", "%llu", (unsigned long long)thread_stats.get_expired);
    APPEND_STAT("get_flushed", "%llu", (unsigned long long)thread_stats.get_flushed);
    APPEND_STAT("delete_misses", "%llu", (unsigned long long)thread_stats.delete_misses);
    APPEND_STAT("delete_hits", "%llu", (unsigned long long)slab_stats.delete_hits);
    APPEND_STAT("incr_misses", "%llu", (unsigned long long)thread_stats.incr_misses);
    APPEND_STAT("incr_hits", "%llu", (unsigned long long)slab_stats.incr_hits);
    APPEND_STAT("decr_misses", "%llu", (unsigned long long)thread_stats.decr_misses);
    APPEND_STAT("decr_hits", "%llu", (unsigned long long)slab_stats.decr_hits);
    APPEND_STAT("cas_misses", "%llu", (unsigned long long)thread_stats.cas_misses);
    APPEND_STAT("cas_hits", "%llu", (unsigned long long)slab_stats.cas_hits);
    APPEND_STAT("cas_badval", "%llu", (unsigned long long)slab_stats.cas_badval);
    APPEND_STAT("touch_hits", "%llu", (unsigned long long)slab_stats.touch_hits);
    APPEND_STAT("touch_misses", "%llu", (unsigned long long)thread_stats.touch_misses);
    APPEND_STAT("auth_cmds", "%llu", (unsigned long long)thread_stats.auth_cmds);
    APPEND_STAT("auth_errors", "%llu", (unsigned long long)thread_stats.auth_errors);
    if (settings.idle_timeout) {
        APPEND_STAT("idle_kicks", "%llu", (unsigned long long)thread_stats.idle_kicks);
    }
    APPEND_STAT("bytes_read", "%llu", (unsigned long long)thread_stats.bytes_read);
    APPEND_STAT("bytes_written", "%llu", (unsigned long long)thread_stats.bytes_written);
    APPEND_STAT("limit_maxbytes", "%llu", (unsigned long long)settings.maxbytes);
    APPEND_STAT("accepting_conns", "%u", stats_state.accepting_conns);
    APPEND_STAT("listen_disabled_num", "%llu", (unsigned long long)stats.listen_disabled_num);
    APPEND_STAT("time_in_listen_disabled_us", "%llu", stats.time_in_listen_disabled_us);
    APPEND_STAT("threads", "%d", settings.num_threads);
    APPEND_STAT("conn_yields", "%llu", (unsigned long long)thread_stats.conn_yields);
    APPEND_STAT("hash_power_level", "%u", stats_state.hash_power_level);
    APPEND_STAT("hash_bytes", "%llu", (unsigned long long)stats_state.hash_bytes);
    APPEND_STAT("hash_is_expanding", "%u", stats_state.hash_is_expanding);
    if (settings.slab_reassign) {
        APPEND_STAT("slab_reassign_rescues", "%llu", stats.slab_reassign_rescues);
        APPEND_STAT("slab_reassign_chunk_rescues", "%llu", stats.slab_reassign_chunk_rescues);
        APPEND_STAT("slab_reassign_evictions_nomem", "%llu", stats.slab_reassign_evictions_nomem);
        APPEND_STAT("slab_reassign_inline_reclaim", "%llu", stats.slab_reassign_inline_reclaim);
        APPEND_STAT("slab_reassign_busy_items", "%llu", stats.slab_reassign_busy_items);
        APPEND_STAT("slab_reassign_running", "%u", stats_state.slab_reassign_running);
        APPEND_STAT("slabs_moved", "%llu", stats.slabs_moved);
    }
    if (settings.lru_crawler) {
        APPEND_STAT("lru_crawler_running", "%u", stats_state.lru_crawler_running);
        APPEND_STAT("lru_crawler_starts", "%u", stats.lru_crawler_starts);
    }
    if (settings.lru_maintainer_thread) {
        APPEND_STAT("lru_maintainer_juggles", "%llu", (unsigned long long)stats.lru_maintainer_juggles);
    }
    APPEND_STAT("malloc_fails", "%llu",
                (unsigned long long)stats.malloc_fails);
    APPEND_STAT("log_worker_dropped", "%llu", (unsigned long long)stats.log_worker_dropped);
    APPEND_STAT("log_worker_written", "%llu", (unsigned long long)stats.log_worker_written);
    APPEND_STAT("log_watcher_skipped", "%llu", (unsigned long long)stats.log_watcher_skipped);
    APPEND_STAT("log_watcher_sent", "%llu", (unsigned long long)stats.log_watcher_sent);
    STATS_UNLOCK();
}

static void process_stat_settings(ADD_STAT add_stats, void *c) {
    assert(add_stats);
    APPEND_STAT("maxbytes", "%llu", (unsigned long long)settings.maxbytes);
    APPEND_STAT("maxconns", "%d", settings.maxconns);
    APPEND_STAT("tcpport", "%d", settings.port);
    APPEND_STAT("udpport", "%d", settings.udpport);
    APPEND_STAT("inter", "%s", settings.inter ? settings.inter : "NULL");
    APPEND_STAT("verbosity", "%d", settings.verbose);
    APPEND_STAT("oldest", "%lu", (unsigned long)settings.oldest_live);
    APPEND_STAT("evictions", "%s", settings.evict_to_free ? "on" : "off");
    APPEND_STAT("domain_socket", "%s",
                settings.socketpath ? settings.socketpath : "NULL");
    APPEND_STAT("umask", "%o", settings.access);
    APPEND_STAT("growth_factor", "%.2f", settings.factor);
    APPEND_STAT("chunk_size", "%d", settings.chunk_size);
    APPEND_STAT("num_threads", "%d", settings.num_threads);
    APPEND_STAT("num_threads_per_udp", "%d", settings.num_threads_per_udp);
    APPEND_STAT("stat_key_prefix", "%c", settings.prefix_delimiter);
    APPEND_STAT("detail_enabled", "%s",
                settings.detail_enabled ? "yes" : "no");
    APPEND_STAT("reqs_per_event", "%d", settings.reqs_per_event);
    APPEND_STAT("cas_enabled", "%s", settings.use_cas ? "yes" : "no");
    APPEND_STAT("tcp_backlog", "%d", settings.backlog);
    //APPEND_STAT("binding_protocol", "%s",
    //            prot_text(settings.binding_protocol));
    APPEND_STAT("auth_enabled_sasl", "%s", settings.sasl ? "yes" : "no");
    APPEND_STAT("item_size_max", "%d", settings.item_size_max);
    APPEND_STAT("maxconns_fast", "%s", settings.maxconns_fast ? "yes" : "no");
    APPEND_STAT("hashpower_init", "%d", settings.hashpower_init);
    APPEND_STAT("slab_reassign", "%s", settings.slab_reassign ? "yes" : "no");
    APPEND_STAT("slab_automove", "%d", settings.slab_automove);
    APPEND_STAT("slab_chunk_max", "%d", settings.slab_chunk_size_max);
    APPEND_STAT("lru_crawler", "%s", settings.lru_crawler ? "yes" : "no");
    APPEND_STAT("lru_crawler_sleep", "%d", settings.lru_crawler_sleep);
    APPEND_STAT("lru_crawler_tocrawl", "%lu", (unsigned long)settings.lru_crawler_tocrawl);
    APPEND_STAT("tail_repair_time", "%d", settings.tail_repair_time);
    APPEND_STAT("flush_enabled", "%s", settings.flush_enabled ? "yes" : "no");
    APPEND_STAT("dump_enabled", "%s", settings.dump_enabled ? "yes" : "no");
    APPEND_STAT("hash_algorithm", "%s", settings.hash_algorithm);
    APPEND_STAT("lru_maintainer_thread", "%s", settings.lru_maintainer_thread ? "yes" : "no");
    APPEND_STAT("lru_segmented", "%s", settings.lru_segmented ? "yes" : "no");
    APPEND_STAT("hot_lru_pct", "%d", settings.hot_lru_pct);
    APPEND_STAT("warm_lru_pct", "%d", settings.warm_lru_pct);
    APPEND_STAT("hot_max_age", "%u", settings.hot_max_age);
    APPEND_STAT("warm_max_factor", "%.2f", settings.warm_max_factor);
    APPEND_STAT("temp_lru", "%s", settings.temp_lru ? "yes" : "no");
    APPEND_STAT("temporary_ttl", "%u", settings.temporary_ttl);
    APPEND_STAT("idle_timeout", "%d", settings.idle_timeout);
    APPEND_STAT("watcher_logbuf_size", "%u", settings.logger_watcher_buf_size);
    APPEND_STAT("worker_logbuf_size", "%u", settings.logger_buf_size);
    APPEND_STAT("track_sizes", "%s", item_stats_sizes_status() ? "yes" : "no");
    APPEND_STAT("inline_ascii_response", "%s", settings.inline_ascii_response ? "yes" : "no");
}

static void conn_to_str(const conn *c, char *buf) {
    char addr_text[MAXPATHLEN];

    if (!c) {
        strcpy(buf, "<null>");
    } else if (c->state == conn_closed) {
        strcpy(buf, "<closed>");
    } else {
        const char *protoname = "?";
        struct sockaddr_in6 local_addr;
        struct sockaddr *addr = (void *)&c->request_addr;
        int af;
        unsigned short port = 0;

        /* For listen ports and idle UDP ports, show listen address */
        if (c->state == conn_listening ||
                (IS_UDP(c->transport) &&
                 c->state == conn_read)) {
            socklen_t local_addr_len = sizeof(local_addr);

            if (getsockname(c->sfd,
                        (struct sockaddr *)&local_addr,
                        &local_addr_len) == 0) {
                addr = (struct sockaddr *)&local_addr;
            }
        }

        af = addr->sa_family;
        addr_text[0] = '\0';

        switch (af) {
            case AF_INET:
                (void) inet_ntop(af,
                        &((struct sockaddr_in *)addr)->sin_addr,
                        addr_text,
                        sizeof(addr_text) - 1);
                port = ntohs(((struct sockaddr_in *)addr)->sin_port);
                protoname = IS_UDP(c->transport) ? "udp" : "tcp";
                break;

            case AF_INET6:
                addr_text[0] = '[';
                addr_text[1] = '\0';
                if (inet_ntop(af,
                        &((struct sockaddr_in6 *)addr)->sin6_addr,
                        addr_text + 1,
                        sizeof(addr_text) - 2)) {
                    strcat(addr_text, "]");
                }
                port = ntohs(((struct sockaddr_in6 *)addr)->sin6_port);
                protoname = IS_UDP(c->transport) ? "udp6" : "tcp6";
                break;

            case AF_UNIX:
                strncpy(addr_text,
                        ((struct sockaddr_un *)addr)->sun_path,
                        sizeof(addr_text) - 1);
                addr_text[sizeof(addr_text)-1] = '\0';
                protoname = "unix";
                break;
        }

        if (strlen(addr_text) < 2) {
            /* Most likely this is a connected UNIX-domain client which
             * has no peer socket address, but there's no portable way
             * to tell for sure.
             */
            sprintf(addr_text, "<AF %d>", af);
        }

        if (port) {
            sprintf(buf, "%s:%s:%u", protoname, addr_text, port);
        } else {
            sprintf(buf, "%s:%s", protoname, addr_text);
        }
    }
}

static void process_stats_conns(ADD_STAT add_stats, void *c) {
    int i;
    char key_str[STAT_KEY_LEN];
    char val_str[STAT_VAL_LEN];
    char conn_name[MAXPATHLEN + sizeof("unix:")];
    int klen = 0, vlen = 0;

    assert(add_stats);

    for (i = 0; i < max_fds; i++) {
        if (conns[i]) {
            /* This is safe to do unlocked because conns are never freed; the
             * worst that'll happen will be a minor inconsistency in the
             * output -- not worth the complexity of the locking that'd be
             * required to prevent it.
             */
            if (conns[i]->state != conn_closed) {
                conn_to_str(conns[i], conn_name);

                APPEND_NUM_STAT(i, "addr", "%s", conn_name);
                APPEND_NUM_STAT(i, "state", "%s",
                        state_text(conns[i]->state));
                APPEND_NUM_STAT(i, "secs_since_last_cmd", "%d",
                        current_time - conns[i]->last_cmd_time);
            }
        }
    }
}


static inline int make_ascii_get_suffix(char *suffix, item *it, bool return_cas) {
    char *p;
    if (!settings.inline_ascii_response) {
        *suffix = ' ';
        p = itoa_u32(*((uint32_t *) ITEM_suffix(it)), suffix+1);
        *p = ' ';
        p = itoa_u32(it->nbytes-2, p+1);
    } else {
        p = suffix;
    }
    if (return_cas) {
        *p = ' ';
        p = itoa_u64(ITEM_get_cas(it), p+1);
    }
    *p = '\r';
    *(p+1) = '\n';
    *(p+2) = '\0';
    return (p - suffix) + 2;
}


static void process_touch_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    int32_t exptime_int = 0;
    item *it;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (!safe_strtol(tokens[2].value, &exptime_int)) {
        out_string(c, "CLIENT_ERROR invalid exptime argument");
        return;
    }

    it = item_touch(key, nkey, realtime(exptime_int), c);
    if (it) {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.touch_cmds++;
        c->thread->stats.slab_stats[ITEM_clsid(it)].touch_hits++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "TOUCHED");
        item_remove(it);
    } else {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.touch_cmds++;
        c->thread->stats.touch_misses++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "NOT_FOUND");
    }
}


/*
 * adds a delta value to a numeric item.
 *
 * c     connection requesting the operation
 * it    item to adjust
 * incr  true to increment value, false to decrement
 * delta amount to adjust value by
 * buf   buffer for response string
 *
 * returns a response string to send back to the client.
 */
enum delta_result_type do_add_delta(conn *c, const char *key, const size_t nkey,
                                    const bool incr, const int64_t delta,
                                    char *buf, uint64_t *cas,
                                    const uint32_t hv) {
    char *ptr;
    uint64_t value;
    int res;
    item *it;

    it = do_item_get(key, nkey, hv, c, DONT_UPDATE);
    if (!it) {
        return DELTA_ITEM_NOT_FOUND;
    }

    /* Can't delta zero byte values. 2-byte are the "\r\n" */
    /* Also can't delta for chunked items. Too large to be a number */
    if (it->nbytes <= 2 || (it->it_flags & ITEM_CHUNKED) != 0) {
        return NON_NUMERIC;
    }

    if (cas != NULL && *cas != 0 && ITEM_get_cas(it) != *cas) {
        do_item_remove(it);
        return DELTA_ITEM_CAS_MISMATCH;
    }

    ptr = ITEM_data(it);

    if (!safe_strtoull(ptr, &value)) {
        do_item_remove(it);
        return NON_NUMERIC;
    }

    if (incr) {
        value += delta;
        MEMCACHED_COMMAND_INCR(c->sfd, ITEM_key(it), it->nkey, value);
    } else {
        if(delta > value) {
            value = 0;
        } else {
            value -= delta;
        }
        MEMCACHED_COMMAND_DECR(c->sfd, ITEM_key(it), it->nkey, value);
    }

    pthread_mutex_lock(&c->thread->stats.mutex);
    if (incr) {
        c->thread->stats.slab_stats[ITEM_clsid(it)].incr_hits++;
    } else {
        c->thread->stats.slab_stats[ITEM_clsid(it)].decr_hits++;
    }
    pthread_mutex_unlock(&c->thread->stats.mutex);

    snprintf(buf, INCR_MAX_STORAGE_LEN, "%llu", (unsigned long long)value);
    res = strlen(buf);
    /* refcount == 2 means we are the only ones holding the item, and it is
     * linked. We hold the item's lock in this function, so refcount cannot
     * increase. */
    if (res + 2 <= it->nbytes && it->refcount == 2) { /* replace in-place */
        /* When changing the value without replacing the item, we
           need to update the CAS on the existing item. */
        /* We also need to fiddle it in the sizes tracker in case the tracking
         * was enabled at runtime, since it relies on the CAS value to know
         * whether to remove an item or not. */
        item_stats_sizes_remove(it);
        ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);
        item_stats_sizes_add(it);
        memcpy(ITEM_data(it), buf, res);
        memset(ITEM_data(it) + res, ' ', it->nbytes - res - 2);
        do_item_update(it);
    } else if (it->refcount > 1) {
        item *new_it;
        uint32_t flags;
        if (settings.inline_ascii_response) {
            flags = (uint32_t) strtoul(ITEM_suffix(it)+1, (char **) NULL, 10);
        } else {
            flags = *((uint32_t *)ITEM_suffix(it));
        }
        new_it = do_item_alloc(ITEM_key(it), it->nkey, flags, it->exptime, res + 2);
        if (new_it == 0) {
            do_item_remove(it);
            return EOM;
        }
        memcpy(ITEM_data(new_it), buf, res);
        memcpy(ITEM_data(new_it) + res, "\r\n", 2);
        item_replace(it, new_it, hv);
        // Overwrite the older item's CAS with our new CAS since we're
        // returning the CAS of the old item below.
        ITEM_set_cas(it, (settings.use_cas) ? ITEM_get_cas(new_it) : 0);
        do_item_remove(new_it);       /* release our reference */
    } else {
        /* Should never get here. This means we somehow fetched an unlinked
         * item. TODO: Add a counter? */
        if (settings.verbose) {
            fprintf(stderr, "Tried to do incr/decr on invalid item\n");
        }
        if (it->refcount == 1)
            do_item_remove(it);
        return DELTA_ITEM_NOT_FOUND;
    }

    if (cas) {
        *cas = ITEM_get_cas(it);    /* swap the incoming CAS value */
    }
    do_item_remove(it);         /* release our reference */
    return OK;
}

static void process_delete_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    item *it;

    assert(c != NULL);

    if (ntokens > 3) {
        bool hold_is_zero = strcmp(tokens[KEY_TOKEN+1].value, "0") == 0;
        bool sets_noreply = set_noreply_maybe(c, tokens, ntokens);
        bool valid = (ntokens == 4 && (hold_is_zero || sets_noreply))
            || (ntokens == 5 && hold_is_zero && sets_noreply);
        if (!valid) {
            out_string(c, "CLIENT_ERROR bad command line format.  "
                       "Usage: delete <key> [noreply]");
            return;
        }
    }


    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if(nkey > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    if (settings.detail_enabled) {
        stats_prefix_record_delete(key, nkey);
    }

    it = item_get(key, nkey, c, DONT_UPDATE);
    if (it) {
        MEMCACHED_COMMAND_DELETE(c->sfd, ITEM_key(it), it->nkey);

        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.slab_stats[ITEM_clsid(it)].delete_hits++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        item_unlink(it);
        item_remove(it);      /* release our reference */
        out_string(c, "DELETED");
    } else {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.delete_misses++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "NOT_FOUND");
    }
}

static void process_verbosity_command(conn *c, token_t *tokens, const size_t ntokens) {
    unsigned int level;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    level = strtoul(tokens[1].value, NULL, 10);
    settings.verbose = level > MAX_VERBOSITY_LEVEL ? MAX_VERBOSITY_LEVEL : level;
    out_string(c, "OK");
    return;
}

static void process_slabs_automove_command(conn *c, token_t *tokens, const size_t ntokens) {
    unsigned int level;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    level = strtoul(tokens[2].value, NULL, 10);
    if (level == 0) {
        settings.slab_automove = 0;
    } else if (level == 1 || level == 2) {
        settings.slab_automove = level;
    } else {
        out_string(c, "ERROR");
        return;
    }
    out_string(c, "OK");
    return;
}

/* TODO: decide on syntax for sampling? */

static void process_memlimit_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint32_t memlimit;
    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (!safe_strtoul(tokens[1].value, &memlimit)) {
        out_string(c, "ERROR");
    } else {
        if (memlimit < 8) {
            out_string(c, "MEMLIMIT_TOO_SMALL cannot set maxbytes to less than 8m");
        } else {
            if (slabs_adjust_mem_limit((size_t) memlimit * 1024 * 1024)) {
                if (settings.verbose > 0) {
                    fprintf(stderr, "maxbytes adjusted to %llum\n", (unsigned long long)memlimit);
                }

                out_string(c, "OK");
            } else {
                out_string(c, "MEMLIMIT_ADJUST_FAILED out of bounds or unable to adjust");
            }
        }
    }
}

static void process_lru_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint32_t pct_hot;
    uint32_t pct_warm;
    uint32_t hot_age;
    int32_t ttl;
    double factor;

    set_noreply_maybe(c, tokens, ntokens);

    if (strcmp(tokens[1].value, "tune") == 0 && ntokens >= 7) {
        if (!safe_strtoul(tokens[2].value, &pct_hot) ||
            !safe_strtoul(tokens[3].value, &pct_warm) ||
            !safe_strtoul(tokens[4].value, &hot_age) ||
            !safe_strtod(tokens[5].value, &factor)) {
            out_string(c, "ERROR");
        } else {
            if (pct_hot + pct_warm > 80) {
                out_string(c, "ERROR hot and warm pcts must not exceed 80");
            } else if (factor <= 0) {
                out_string(c, "ERROR cold age factor must be greater than 0");
            } else {
                settings.hot_lru_pct = pct_hot;
                settings.warm_lru_pct = pct_warm;
                settings.hot_max_age = hot_age;
                settings.warm_max_factor = factor;
                out_string(c, "OK");
            }
        }
    } else if (strcmp(tokens[1].value, "mode") == 0 && ntokens >= 3 &&
               settings.lru_maintainer_thread) {
        if (strcmp(tokens[2].value, "flat") == 0) {
            settings.lru_segmented = false;
            out_string(c, "OK");
        } else if (strcmp(tokens[2].value, "segmented") == 0) {
            settings.lru_segmented = true;
            out_string(c, "OK");
        } else {
            out_string(c, "ERROR");
        }
    } else if (strcmp(tokens[1].value, "temp_ttl") == 0 && ntokens >= 3 &&
               settings.lru_maintainer_thread) {
        if (!safe_strtol(tokens[2].value, &ttl)) {
            out_string(c, "ERROR");
        } else {
            if (ttl < 0) {
                settings.temp_lru = false;
            } else {
                settings.temp_lru = true;
                settings.temporary_ttl = ttl;
            }
            out_string(c, "OK");
        }
    } else {
        out_string(c, "ERROR");
    }
}



/*
 * read a UDP request.
 */
static enum try_read_result try_read_udp(conn *c) {
    int res;

    assert(c != NULL);

    c->request_addr_size = sizeof(c->request_addr);
    res = recvfrom(c->sfd, c->rbuf, c->rsize,
                   0, (struct sockaddr *)&c->request_addr,
                   &c->request_addr_size);
    if (res > 8) {
        unsigned char *buf = (unsigned char *)c->rbuf;
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.bytes_read += res;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        /* Beginning of UDP packet is the request ID; save it. */
        c->request_id = buf[0] * 256 + buf[1];

        /* If this is a multi-packet request, drop it. */
        if (buf[4] != 0 || buf[5] != 1) {
            out_string(c, "SERVER_ERROR multi-packet request not supported");
            return READ_NO_DATA_RECEIVED;
        }

        /* Don't care about any of the rest of the header. */
        res -= 8;
        memmove(c->rbuf, c->rbuf + 8, res);

        c->rbytes = res;
        c->rcurr = c->rbuf;
        return READ_DATA_RECEIVED;
    }
    return READ_NO_DATA_RECEIVED;
}


/* Does a looped read to fill data chunks */
/* TODO: restrict number of times this can loop.
 * Also, benchmark using readv's.
 */
static int read_into_chunked_item(conn *c) {
    int total = 0;
    int res;
    assert(c->rcurr != c->ritem);

    while (c->rlbytes > 0) {
        item_chunk *ch = (item_chunk *)c->ritem;
        assert(ch->used <= ch->size);
        if (ch->size == ch->used) {
            // FIXME: ch->next is currently always 0. remove this?
            if (ch->next) {
                c->ritem = (char *) ch->next;
            } else {
                /* Allocate next chunk. Binary protocol needs 2b for \r\n */
                c->ritem = (char *) do_item_alloc_chunk(ch, c->rlbytes +
                       ((c->protocol == binary_prot) ? 2 : 0));
                if (!c->ritem) {
                    // We failed an allocation. Let caller handle cleanup.
                    total = -2;
                    break;
                }
                // ritem has new chunk, restart the loop.
                continue;
                //assert(c->rlbytes == 0);
            }
        }

        int unused = ch->size - ch->used;
        /* first check if we have leftovers in the conn_read buffer */
        if (c->rbytes > 0) {
            total = 0;
            int tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
            tocopy = tocopy > unused ? unused : tocopy;
            if (c->ritem != c->rcurr) {
                memmove(ch->data + ch->used, c->rcurr, tocopy);
            }
            total += tocopy;
            c->rlbytes -= tocopy;
            c->rcurr += tocopy;
            c->rbytes -= tocopy;
            ch->used += tocopy;
            if (c->rlbytes == 0) {
                break;
            }
        } else {
            /*  now try reading from the socket */
            res = read(c->sfd, ch->data + ch->used,
                    (unused > c->rlbytes ? c->rlbytes : unused));
            if (res > 0) {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.bytes_read += res;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                ch->used += res;
                total += res;
                c->rlbytes -= res;
            } else {
                /* Reset total to the latest result so caller can handle it */
                total = res;
                break;
            }
        }
    }

    /* At some point I will be able to ditch the \r\n from item storage and
       remove all of these kludges.
       The above binprot check ensures inline space for \r\n, but if we do
       exactly enough allocs there will be no additional chunk for \r\n.
     */
    if (c->rlbytes == 0 && c->protocol == binary_prot && total >= 0) {
        item_chunk *ch = (item_chunk *)c->ritem;
        if (ch->size - ch->used < 2) {
            c->ritem = (char *) do_item_alloc_chunk(ch, 2);
            if (!c->ritem) {
                total = -2;
            }
        }
    }
    return total;
}



/*
 * Sets a socket's send buffer size to the maximum allowed by the system.
 */
static void maximize_sndbuf(const int sfd) {
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;

    /* Start with the default size. */
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &old_size, &intsize) != 0) {
        if (settings.verbose > 0)
            perror("getsockopt(SO_SNDBUF)");
        return;
    }

    /* Binary-search for the real maximum. */
    min = old_size;
    max = MAX_SENDBUF_SIZE;

    while (min <= max) {
        avg = ((unsigned int)(min + max)) / 2;
        if (setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&avg, intsize) == 0) {
            last_good = avg;
            min = avg + 1;
        } else {
            max = avg - 1;
        }
    }

    if (settings.verbose > 1)
        fprintf(stderr, "<%d send buffer was %d, now %d\n", sfd, old_size, last_good);
}


/*
 * We keep the current time of day in a global variable that's updated by a
 * timer event. This saves us a bunch of time() system calls (we really only
 * need to get the time once a second, whereas there can be tens of thousands
 * of requests a second) and allows us to use server-start-relative timestamps
 * rather than absolute UNIX timestamps, a space savings on systems where
 * sizeof(time_t) > sizeof(unsigned int).
 */
volatile rel_time_t current_time;
static struct event clockevent;

/* libevent uses a monotonic clock when available for event scheduling. Aside
 * from jitter, simply ticking our internal timer here is accurate enough.
 * Note that users who are setting explicit dates for expiration times *must*
 * ensure their clocks are correct before starting memcached. */
//static void clock_handler(const int fd, const short which, void *arg) {
//    struct timeval t = {.tv_sec = 1, .tv_usec = 0};
//    static bool initialized = false;
//#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
//    static bool monotonic = false;
//    static time_t monotonic_start;
//#endif
//
//    if (initialized) {
//        /* only delete the event if it's actually there. */
//        evtimer_del(&clockevent);
//    } else {
//        initialized = true;
//        /* process_started is initialized to time() - 2. We initialize to 1 so
//         * flush_all won't underflow during tests. */
//#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
//        struct timespec ts;
//        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
//            monotonic = true;
//            monotonic_start = ts.tv_sec - ITEM_UPDATE_INTERVAL - 2;
//        }
//#endif
//    }
//
//    evtimer_set(&clockevent, clock_handler, 0);
//    event_base_set(main_base, &clockevent);
//    evtimer_add(&clockevent, &t);
//
//#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
//    if (monotonic) {
//        struct timespec ts;
//        if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
//            return;
//        current_time = (rel_time_t) (ts.tv_sec - monotonic_start);
//        return;
//    }
//#endif
//    {
//        struct timeval tv;
//        gettimeofday(&tv, NULL);
//        current_time = (rel_time_t) (tv.tv_sec - process_started);
//    }
//}

static void usage(void) {
    //printf(PACKAGE " " VERSION "\n");
    printf("-p <num>      TCP port number to listen on (default: 11211)\n"
           "-U <num>      UDP port number to listen on (default: 11211, 0 is off)\n"
           "-s <file>     UNIX socket path to listen on (disables network support)\n"
           "-A            enable ascii \"shutdown\" command\n"
           "-a <mask>     access mask for UNIX socket, in octal (default: 0700)\n"
           "-l <addr>     interface to listen on (default: INADDR_ANY, all addresses)\n"
           "              <addr> may be specified as host:port. If you don't specify\n"
           "              a port number, the value you specified with -p or -U is\n"
           "              used. You may specify multiple addresses separated by comma\n"
           "              or by using -l multiple times\n"

           "-d            run as a daemon\n"
           "-r            maximize core file limit\n"
           "-u <username> assume identity of <username> (only when run as root)\n"
           "-m <num>      max memory to use for items in megabytes (default: 64 MB)\n"
           "-M            return error on memory exhausted (rather than removing items)\n"
           "-c <num>      max simultaneous connections (default: 1024)\n"
           "-k            lock down all paged memory.  Note that there is a\n"
           "              limit on how much memory you may lock.  Trying to\n"
           "              allocate more than that would fail, so be sure you\n"
           "              set the limit correctly for the user you started\n"
           "              the daemon with (not for -u <username> user;\n"
           "              under sh this is done with 'ulimit -S -l NUM_KB').\n"
           "-v            verbose (print errors/warnings while in event loop)\n"
           "-vv           very verbose (also print client commands/reponses)\n"
           "-vvv          extremely verbose (also print internal state transitions)\n"
           "-h            print this help and exit\n"
           "-i            print memcached and libevent license\n"
           "-V            print version and exit\n"
           "-P <file>     save PID in <file>, only used with -d option\n"
           "-f <factor>   chunk size growth factor (default: 1.25)\n"
           "-n <bytes>    minimum space allocated for key+value+flags (default: 48)\n");
    printf("-L            Try to use large memory pages (if available). Increasing\n"
           "              the memory page size could reduce the number of TLB misses\n"
           "              and improve the performance. In order to get large pages\n"
           "              from the OS, memcached will allocate the total item-cache\n"
           "              in one large chunk.\n");
    printf("-D <char>     Use <char> as the delimiter between key prefixes and IDs.\n"
           "              This is used for per-prefix stats reporting. The default is\n"
           "              \":\" (colon). If this option is specified, stats collection\n"
           "              is turned on automatically; if not, then it may be turned on\n"
           "              by sending the \"stats detail on\" command to the server.\n");
    printf("-t <num>      number of threads to use (default: 4)\n");
    printf("-R            Maximum number of requests per event, limits the number of\n"
           "              requests process for a given connection to prevent \n"
           "              starvation (default: 20)\n");
    printf("-C            Disable use of CAS\n");
    printf("-b <num>      Set the backlog queue limit (default: 1024)\n");
    printf("-B            Binding protocol - one of ascii, binary, or auto (default)\n");
    printf("-I            Override the size of each slab page. Adjusts max item size\n"
           "              (default: 1mb, min: 1k, max: 128m)\n");
#ifdef ENABLE_SASL
    printf("-S            Turn on Sasl authentication\n");
#endif
    printf("-F            Disable flush_all command\n");
    printf("-X            Disable stats cachedump and lru_crawler metadump commands\n");
    printf("-o            Comma separated list of extended or experimental options\n"
           "              - maxconns_fast: immediately close new\n"
           "                connections if over maxconns limit\n"
           "              - hashpower: An integer multiplier for how large the hash\n"
           "                table should be. Can be grown at runtime if not big enough.\n"
           "                Set this based on \"STAT hash_power_level\" before a \n"
           "                restart.\n"
           "              - tail_repair_time: Time in seconds that indicates how long to wait before\n"
           "                forcefully taking over the LRU tail item whose refcount has leaked.\n"
           "                Disabled by default; dangerous option.\n"
           "              - hash_algorithm: The hash table algorithm\n"
           "                default is jenkins hash. options: jenkins, murmur3\n"
           "              - lru_crawler: Enable LRU Crawler background thread\n"
           "              - lru_crawler_sleep: Microseconds to sleep between items\n"
           "                default is 100.\n"
           "              - lru_crawler_tocrawl: Max items to crawl per slab per run\n"
           "                default is 0 (unlimited)\n"
           "              - lru_maintainer: Enable new LRU system + background thread\n"
           "              - hot_lru_pct: Pct of slab memory to reserve for hot lru.\n"
           "                (requires lru_maintainer)\n"
           "              - warm_lru_pct: Pct of slab memory to reserve for warm lru.\n"
           "                (requires lru_maintainer)\n"
           "              - hot_max_age: Items idle longer than this drop from hot lru.\n"
           "              - cold_max_factor: Items idle longer than cold lru age * this drop from warm.\n"
           "              - temporary_ttl: TTL's below this use separate LRU, cannot be evicted.\n"
           "                (requires lru_maintainer)\n"
           "              - idle_timeout: Timeout for idle connections\n"
           "              - (EXPERIMENTAL) slab_chunk_max: Maximum slab size. Do not change without extreme care.\n"
           "              - watcher_logbuf_size: Size in kilobytes of per-watcher write buffer.\n"
           "              - worker_logbuf_Size: Size in kilobytes of per-worker-thread buffer\n"
           "                read by background thread. Which is then written to watchers.\n"
           "              - track_sizes: Enable dynamic reports for 'stats sizes' command.\n"
           "              - no_inline_ascii_resp: Save up to 24 bytes per item. Small perf hit in ASCII,\n"
           "                no perf difference in binary protocol. Speeds up sets.\n"
           "              - modern: Enables 'modern' defaults. Options that will be default in future.\n"
           "                enables: slab_chunk_max:512k,slab_reassign,slab_automove=1,maxconns_fast,\n"
           "                         hash_algorithm=murmur3,lru_crawler,lru_maintainer,no_inline_ascii_resp\n"
           );
    return;
}

static void usage_license(void) {
    //printf(PACKAGE " " VERSION "\n\n");
    printf(
    "Copyright (c) 2003, Danga Interactive, Inc. <http://www.danga.com/>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions are\n"
    "met:\n"
    "\n"
    "    * Redistributions of source code must retain the above copyright\n"
    "notice, this list of conditions and the following disclaimer.\n"
    "\n"
    "    * Redistributions in binary form must reproduce the above\n"
    "copyright notice, this list of conditions and the following disclaimer\n"
    "in the documentation and/or other materials provided with the\n"
    "distribution.\n"
    "\n"
    "    * Neither the name of the Danga Interactive nor the names of its\n"
    "contributors may be used to endorse or promote products derived from\n"
    "this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
    "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
    "LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\n"
    "A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
    "OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\n"
    "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n"
    "LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
    "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    "\n"
    "\n"
    "This product includes software developed by Niels Provos.\n"
    "\n"
    "[ libevent ]\n"
    "\n"
    "Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions\n"
    "are met:\n"
    "1. Redistributions of source code must retain the above copyright\n"
    "   notice, this list of conditions and the following disclaimer.\n"
    "2. Redistributions in binary form must reproduce the above copyright\n"
    "   notice, this list of conditions and the following disclaimer in the\n"
    "   documentation and/or other materials provided with the distribution.\n"
    "3. All advertising materials mentioning features or use of this software\n"
    "   must display the following acknowledgement:\n"
    "      This product includes software developed by Niels Provos.\n"
    "4. The name of the author may not be used to endorse or promote products\n"
    "   derived from this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR\n"
    "IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES\n"
    "OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.\n"
    "IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,\n"
    "INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT\n"
    "NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF\n"
    "THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    );

    return;
}


static void sig_handler(const int sig) {
    printf("Signal handled: %s.\n", strsignal(sig));
    exit(EXIT_SUCCESS);
}

#ifndef HAVE_SIGIGNORE
//static int sigignore(int sig) {
//    struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = 0 };
//
//    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(sig, &sa, 0) == -1) {
//        return -1;
//    }
//    return 0;
//}
#endif


/*
 * On systems that supports multiple page sizes we may reduce the
 * number of TLB-misses by using the biggest available page size
 */
static int enable_large_pages(void) {
#if defined(HAVE_GETPAGESIZES) && defined(HAVE_MEMCNTL)
    int ret = -1;
    size_t sizes[32];
    int avail = getpagesizes(sizes, 32);
    if (avail != -1) {
        size_t max = sizes[0];
        struct memcntl_mha arg = {0};
        int ii;

        for (ii = 1; ii < avail; ++ii) {
            if (max < sizes[ii]) {
                max = sizes[ii];
            }
        }

        arg.mha_flags   = 0;
        arg.mha_pagesize = max;
        arg.mha_cmd = MHA_MAPSIZE_BSSBRK;

        if (memcntl(0, 0, MC_HAT_ADVISE, (caddr_t)&arg, 0, 0) == -1) {
            fprintf(stderr, "Failed to set large pages: %s\n",
                    strerror(errno));
            fprintf(stderr, "Will use default page size\n");
        } else {
            ret = 0;
        }
    } else {
        fprintf(stderr, "Failed to get supported pagesizes: %s\n",
                strerror(errno));
        fprintf(stderr, "Will use default page size\n");
    }

    return ret;
#else
    return -1;
#endif
}


static bool _parse_slab_sizes(char *s, uint32_t *slab_sizes) {
    char *b = NULL;
    uint32_t size = 0;
    int i = 0;
    uint32_t last_size = 0;

    if (strlen(s) < 1)
        return false;

    char* p;
    for (p = strtok_r(s, "-", &b);
         p != NULL;
         p = strtok_r(NULL, "-", &b)) {
        if (!safe_strtoul(p, &size) || size < settings.chunk_size
             || size > settings.slab_chunk_size_max) {
            fprintf(stderr, "slab size %u is out of valid range\n", size);
            return false;
        }
        if (last_size >= size) {
            fprintf(stderr, "slab size %u cannot be lower than or equal to a previous class size\n", size);
            return false;
        }
        if (size <= last_size + CHUNK_ALIGN_BYTES) {
            fprintf(stderr, "slab size %u must be at least %d bytes larger than previous class\n",
                    size, CHUNK_ALIGN_BYTES);
            return false;
        }
        slab_sizes[i++] = size;
        last_size = size;
        if (i >= MAX_NUMBER_OF_SLAB_CLASSES-1) {
            fprintf(stderr, "too many slab classes specified\n");
            return false;
        }
    }

    slab_sizes[i] = 0;
    return true;
}


int init_memcached(int argc, char **argv) {
    int c;
    bool lock_memory = false;
    bool do_daemonize = false;
    bool preallocate = false;
    int maxcore = 0;
    char *username = NULL;
    char *pid_file = NULL;
    struct passwd *pw;
    struct rlimit rlim;
    char *buf;
    char unit = '\0';
    int size_max = 0;
    int retval = EXIT_SUCCESS;
    /* listening sockets */
    static int *l_socket = NULL;

    /* udp socket */
    static int *u_socket = NULL;
    bool protocol_specified = false;
    bool tcp_specified = false;
    bool udp_specified = false;
    bool start_lru_maintainer = false;
    bool start_lru_crawler = false;
    enum hashfunc_type hash_type = JENKINS_HASH;
    uint32_t tocrawl;
    uint32_t slab_sizes[MAX_NUMBER_OF_SLAB_CLASSES];
    bool use_slab_sizes = false;
    char *slab_sizes_unparsed = NULL;
    bool slab_chunk_size_changed = false;

    char *subopts, *subopts_orig;
    char *subopts_value;
    enum {
        MAXCONNS_FAST = 0,
        HASHPOWER_INIT,
        SLAB_REASSIGN,
        SLAB_AUTOMOVE,
        TAIL_REPAIR_TIME,
        HASH_ALGORITHM,
        LRU_CRAWLER,
        LRU_CRAWLER_SLEEP,
        LRU_CRAWLER_TOCRAWL,
        LRU_MAINTAINER,
        HOT_LRU_PCT,
        WARM_LRU_PCT,
        HOT_MAX_AGE,
        WARM_MAX_FACTOR,
        TEMPORARY_TTL,
        IDLE_TIMEOUT,
        WATCHER_LOGBUF_SIZE,
        WORKER_LOGBUF_SIZE,
        SLAB_SIZES,
        SLAB_CHUNK_MAX,
        TRACK_SIZES,
        NO_INLINE_ASCII_RESP,
        MODERN
    };
    char *const subopts_tokens[] = {
        [MAXCONNS_FAST] = "maxconns_fast",
        [HASHPOWER_INIT] = "hashpower",
        [SLAB_REASSIGN] = "slab_reassign",
        [SLAB_AUTOMOVE] = "slab_automove",
        [TAIL_REPAIR_TIME] = "tail_repair_time",
        [HASH_ALGORITHM] = "hash_algorithm",
        [LRU_CRAWLER] = "lru_crawler",
        [LRU_CRAWLER_SLEEP] = "lru_crawler_sleep",
        [LRU_CRAWLER_TOCRAWL] = "lru_crawler_tocrawl",
        [LRU_MAINTAINER] = "lru_maintainer",
        [HOT_LRU_PCT] = "hot_lru_pct",
        [WARM_LRU_PCT] = "warm_lru_pct",
        [HOT_MAX_AGE] = "hot_max_age",
        [WARM_MAX_FACTOR] = "warm_max_factor",
        [TEMPORARY_TTL] = "temporary_ttl",
        [IDLE_TIMEOUT] = "idle_timeout",
        [WATCHER_LOGBUF_SIZE] = "watcher_logbuf_size",
        [WORKER_LOGBUF_SIZE] = "worker_logbuf_size",
        [SLAB_SIZES] = "slab_sizes",
        [SLAB_CHUNK_MAX] = "slab_chunk_max",
        [TRACK_SIZES] = "track_sizes",
        [NO_INLINE_ASCII_RESP] = "no_inline_ascii_resp",
        [MODERN] = "modern",
        NULL
    };

//    if (!sanitycheck()) {
//        return EX_OSERR;
//    }

    /* handle SIGINT and SIGTERM */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* init settings */
    settings_init();

    /* Run regardless of initializing it later */
    init_lru_crawler();
    init_lru_maintainer();

    /* set stderr non-buffering (for running under, say, daemontools) */
    setbuf(stderr, NULL);

    printf("argc=%d, argv:\n", argc);
    int i = 0;
    for (; i < argc; i++)
        printf("%s\n", argv[i]);
    /* process arguments */
    while (-1 != (c = getopt(argc, argv,
          "a:"  /* access mask for unix socket */
          "A"  /* enable admin shutdown commannd */
          "p:"  /* TCP port number to listen on */
          "s:"  /* unix socket path to listen on */
          "U:"  /* UDP port number to listen on */
          "m:"  /* max memory to use for items in megabytes */
          "M"   /* return error on memory exhausted */
          "c:"  /* max simultaneous connections */
          "k"   /* lock down all paged memory */
          "hiV" /* help, licence info, version */
          "r"   /* maximize core file limit */
          "v"   /* verbose */
          "d"   /* daemon mode */
          "l:"  /* interface to listen on */
          "u:"  /* user identity to run as */
          "P:"  /* save PID in file */
          "f:"  /* factor? */
          "n:"  /* minimum space allocated for key+value+flags */
          "t:"  /* threads */
          "D:"  /* prefix delimiter? */
          "L"   /* Large memory pages */
          "R:"  /* max requests per event */
          "C"   /* Disable use of CAS */
          "b:"  /* backlog queue limit */
          "B:"  /* Binding protocol */
          "I:"  /* Max item size */
          "S"   /* Sasl ON */
          "F"   /* Disable flush_all */
          "X"   /* Disable dump commands */
          "o:"  /* Extended generic options */
        ))) {
        switch (c) {
        case 'A':
            /* enables "shutdown" command */
            settings.shutdown_command = true;
            break;

        case 'a':
            /* access for unix domain socket, as octal mask (like chmod)*/
            settings.access= strtol(optarg,NULL,8);
            break;

        case 'U':
            settings.udpport = atoi(optarg);
            udp_specified = true;
            break;
        case 'p':
            settings.port = atoi(optarg);
            tcp_specified = true;
            break;
        case 's':
            settings.socketpath = optarg;
            break;
        case 'm':
            settings.maxbytes = ((size_t)atoi(optarg)) * 1024 * 1024 * 1024; 
	    printf("memory budget for memcached: %ld\n", settings.maxbytes);
	    break;
        case 'M':
	    printf("evict_to_free\n");
            settings.evict_to_free = 0;
            break;
        case 'c':
            settings.maxconns = atoi(optarg);
            if (settings.maxconns <= 0) {
                fprintf(stderr, "Maximum connections must be greater than 0\n");
                return 1;
            }
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        case 'i':
            usage_license();
            exit(EXIT_SUCCESS);
        case 'V':
            //printf(PACKAGE " " VERSION "\n");
            exit(EXIT_SUCCESS);
        case 'k':
            lock_memory = true;
            break;
        case 'v':
            settings.verbose++;
            break;
        case 'l':
            if (settings.inter != NULL) {
                if (strstr(settings.inter, optarg) != NULL) {
                    break;
                }
                size_t len = strlen(settings.inter) + strlen(optarg) + 2;
                char *p = malloc(len);
                if (p == NULL) {
                    fprintf(stderr, "Failed to allocate memory\n");
                    return 1;
                }
                snprintf(p, len, "%s,%s", settings.inter, optarg);
                free(settings.inter);
                settings.inter = p;
            } else {
                settings.inter= strdup(optarg);
            }
            break;
        case 'd':
            do_daemonize = true;
            break;
        case 'r':
            maxcore = 1;
            break;
        case 'R':
            settings.reqs_per_event = atoi(optarg);
            if (settings.reqs_per_event == 0) {
                fprintf(stderr, "Number of requests per event must be greater than 0\n");
                return 1;
            }
            break;
        case 'u':
            username = optarg;
            break;
        case 'P':
            pid_file = optarg;
            break;
        case 'f':
            settings.factor = atof(optarg);
            if (settings.factor <= 1.0) {
                fprintf(stderr, "Factor must be greater than 1\n");
                return 1;
            }
            break;
        case 'n':
            settings.chunk_size = atoi(optarg);
            if (settings.chunk_size == 0) {
                fprintf(stderr, "Chunk size must be greater than 0\n");
                return 1;
            }
            break;
        case 't':
            settings.num_threads = atoi(optarg);
            if (settings.num_threads <= 0) {
                fprintf(stderr, "Number of threads must be greater than 0\n");
                return 1;
            }
            /* There're other problems when you get above 64 threads.
             * In the future we should portably detect # of cores for the
             * default.
             */
            if (settings.num_threads > 64) {
                fprintf(stderr, "WARNING: Setting a high number of worker"
                                "threads is not recommended.\n"
                                " Set this value to the number of cores in"
                                " your machine or less.\n");
            }
            break;
        case 'D':
            if (! optarg || ! optarg[0]) {
                fprintf(stderr, "No delimiter specified\n");
                return 1;
            }
            settings.prefix_delimiter = optarg[0];
            settings.detail_enabled = 1;
            break;
        case 'L' :
            if (enable_large_pages() == 0) {
                preallocate = true;
            } else {
                fprintf(stderr, "Cannot enable large pages on this system\n"
                    "(There is no Linux support as of this version)\n");
                return 1;
            }
            break;
        case 'C' :
            settings.use_cas = false;
            break;
        case 'b' :
            settings.backlog = atoi(optarg);
            break;
        case 'B':
            protocol_specified = true;
            if (strcmp(optarg, "auto") == 0) {
                settings.binding_protocol = negotiating_prot;
            } else if (strcmp(optarg, "binary") == 0) {
                settings.binding_protocol = binary_prot;
            } else if (strcmp(optarg, "ascii") == 0) {
                settings.binding_protocol = ascii_prot;
            } else {
                fprintf(stderr, "Invalid value for binding protocol: %s\n"
                        " -- should be one of auto, binary, or ascii\n", optarg);
                exit(EX_USAGE);
            }
            break;
        case 'I':
            buf = strdup(optarg);
            unit = buf[strlen(buf)-1];
            if (unit == 'k' || unit == 'm' ||
                unit == 'K' || unit == 'M') {
                buf[strlen(buf)-1] = '\0';
                size_max = atoi(buf);
                if (unit == 'k' || unit == 'K')
                    size_max *= 1024;
                if (unit == 'm' || unit == 'M')
                    size_max *= 1024 * 1024;
                settings.item_size_max = size_max;
            } else {
                settings.item_size_max = atoi(buf);
            }
            free(buf);
            if (settings.item_size_max < 1024) {
                fprintf(stderr, "Item max size cannot be less than 1024 bytes.\n");
                return 1;
            }
            if (settings.item_size_max > (settings.maxbytes / 4)) {
                fprintf(stderr, "Cannot set item size limit higher than 1/4 of memory max.\n");
                return 1;
            }
            if (settings.item_size_max > (1024 * 1024 * 1024)) {
                fprintf(stderr, "Cannot set item size limit higher than a gigabyte.\n");
                return 1;
            }
            if (settings.item_size_max > 1024 * 1024) {
                if (!slab_chunk_size_changed) {
                    // Ideal new default is 16k, but needs stitching.
                    settings.slab_chunk_size_max = 524288;
                }
            }
            break;
        case 'S': /* set Sasl authentication to true. Default is false */
#ifndef ENABLE_SASL
            fprintf(stderr, "This server is not built with SASL support.\n");
            exit(EX_USAGE);
#endif
            settings.sasl = true;
            break;
       case 'F' :
            settings.flush_enabled = false;
            break;
       case 'X' :
            settings.dump_enabled = false;
            break;
        case 'o': /* It's sub-opts time! */
            subopts_orig = subopts = strdup(optarg); /* getsubopt() changes the original args */

            while (*subopts != '\0') {

            switch (getsubopt(&subopts, subopts_tokens, &subopts_value)) {
            case MAXCONNS_FAST:
                settings.maxconns_fast = true;
                break;
            case HASHPOWER_INIT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing numeric argument for hashpower\n");
                    return 1;
                }
                settings.hashpower_init = atoi(subopts_value);
                if (settings.hashpower_init < 12) {
                    fprintf(stderr, "Initial hashtable multiplier of %d is too low\n",
                        settings.hashpower_init);
                    return 1;
                } else if (settings.hashpower_init > 64) {
                    fprintf(stderr, "Initial hashtable multiplier of %d is too high\n"
                        "Choose a value based on \"STAT hash_power_level\" from a running instance\n",
                        settings.hashpower_init);
                    return 1;
                }
                break;
            case SLAB_REASSIGN:
                settings.slab_reassign = true;
                break;
            case SLAB_AUTOMOVE:
                if (subopts_value == NULL) {
                    settings.slab_automove = 1;
                    break;
                }
                settings.slab_automove = atoi(subopts_value);
                if (settings.slab_automove < 0 || settings.slab_automove > 2) {
                    fprintf(stderr, "slab_automove must be between 0 and 2\n");
                    return 1;
                }
                break;
            case TAIL_REPAIR_TIME:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing numeric argument for tail_repair_time\n");
                    return 1;
                }
                settings.tail_repair_time = atoi(subopts_value);
                if (settings.tail_repair_time < 10) {
                    fprintf(stderr, "Cannot set tail_repair_time to less than 10 seconds\n");
                    return 1;
                }
                break;
            case HASH_ALGORITHM:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing hash_algorithm argument\n");
                    return 1;
                };
                if (strcmp(subopts_value, "jenkins") == 0) {
                    hash_type = JENKINS_HASH;
                } else if (strcmp(subopts_value, "murmur3") == 0) {
                    hash_type = MURMUR3_HASH;
                } else {
                    fprintf(stderr, "Unknown hash_algorithm option (jenkins, murmur3)\n");
                    return 1;
                }
                break;
            case LRU_CRAWLER:
                start_lru_crawler = true;
                break;
            case LRU_CRAWLER_SLEEP:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing lru_crawler_sleep value\n");
                    return 1;
                }
                settings.lru_crawler_sleep = atoi(subopts_value);
                if (settings.lru_crawler_sleep > 1000000 || settings.lru_crawler_sleep < 0) {
                    fprintf(stderr, "LRU crawler sleep must be between 0 and 1 second\n");
                    return 1;
                }
                break;
            case LRU_CRAWLER_TOCRAWL:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing lru_crawler_tocrawl value\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &tocrawl)) {
                    fprintf(stderr, "lru_crawler_tocrawl takes a numeric 32bit value\n");
                    return 1;
                }
                settings.lru_crawler_tocrawl = tocrawl;
                break;
            case LRU_MAINTAINER:
                start_lru_maintainer = true;
                settings.lru_segmented = true;
                break;
            case HOT_LRU_PCT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing hot_lru_pct argument\n");
                    return 1;
                }
                settings.hot_lru_pct = atoi(subopts_value);
                if (settings.hot_lru_pct < 1 || settings.hot_lru_pct >= 80) {
                    fprintf(stderr, "hot_lru_pct must be > 1 and < 80\n");
                    return 1;
                }
                break;
            case WARM_LRU_PCT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing warm_lru_pct argument\n");
                    return 1;
                }
                settings.warm_lru_pct = atoi(subopts_value);
                if (settings.warm_lru_pct < 1 || settings.warm_lru_pct >= 80) {
                    fprintf(stderr, "warm_lru_pct must be > 1 and < 80\n");
                    return 1;
                }
                break;
            case HOT_MAX_AGE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing hot_max_age argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.hot_max_age)) {
                    fprintf(stderr, "invalid argument to hot_max_age\n");
                    return 1;
                }
                break;
            case WARM_MAX_FACTOR:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing warm_max_factor argument\n");
                    return 1;
                }
                settings.warm_max_factor = atof(subopts_value);
                if (settings.warm_max_factor <= 0) {
                    fprintf(stderr, "warm_max_factor must be > 0\n");
                    return 1;
                }
                break;
            case TEMPORARY_TTL:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing temporary_ttl argument\n");
                    return 1;
                }
                settings.temp_lru = true;
                settings.temporary_ttl = atoi(subopts_value);
                break;
            case IDLE_TIMEOUT:
                settings.idle_timeout = atoi(subopts_value);
                break;
            case WATCHER_LOGBUF_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing watcher_logbuf_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.logger_watcher_buf_size)) {
                    fprintf(stderr, "could not parse argument to watcher_logbuf_size\n");
                    return 1;
                }
                settings.logger_watcher_buf_size *= 1024; /* kilobytes */
                break;
            case WORKER_LOGBUF_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing worker_logbuf_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.logger_buf_size)) {
                    fprintf(stderr, "could not parse argument to worker_logbuf_size\n");
                    return 1;
                }
                settings.logger_buf_size *= 1024; /* kilobytes */
            case SLAB_SIZES:
                slab_sizes_unparsed = subopts_value;
                break;
            case SLAB_CHUNK_MAX:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing slab_chunk_max argument\n");
                }
                if (!safe_strtoll(subopts_value, &settings.slab_chunk_size_max)) {
                    fprintf(stderr, "could not parse argument to slab_chunk_max\n");
                }
                slab_chunk_size_changed = true;
                break;
            case TRACK_SIZES:
                item_stats_sizes_init();
                break;
            case NO_INLINE_ASCII_RESP:
                settings.inline_ascii_response = false;
                break;
            case MODERN:
                /* Modernized defaults. Need to add equivalent no_* flags
                 * before making truly default. */
                // chunk default should come after stitching is fixed.
                //settings.slab_chunk_size_max = 16384;

                // With slab_ressign, pages are always 1MB, so anything larger
                // than .5m ends up using 1m anyway. With this we at least
                // avoid having several slab classes that use 1m.
                printf("MODERN\n");
		if (!slab_chunk_size_changed) {
                    settings.slab_chunk_size_max = 524288;
                }
                settings.slab_reassign = true;
                settings.slab_automove = 1;
                settings.maxconns_fast = true;
                settings.inline_ascii_response = false;
                settings.lru_segmented = true;
                hash_type = MURMUR3_HASH;
                start_lru_crawler = true;
                start_lru_maintainer = true;
                break;
            default:
                printf("Illegal suboption \"%s\"\n", subopts_value);
                return 1;
            }

            }
            free(subopts_orig);
            break;
        default:
            fprintf(stderr, "Illegal argument \"%c\"\n", c);
            return 1;
        }
    }

    if (settings.slab_chunk_size_max > settings.item_size_max) {
        fprintf(stderr, "slab_chunk_max (bytes: %d) cannot be larger than -I (item_size_max %d)\n",
                settings.slab_chunk_size_max, settings.item_size_max);
        exit(EX_USAGE);
    }

    if (settings.item_size_max % settings.slab_chunk_size_max != 0) {
        fprintf(stderr, "-I (item_size_max: %d) must be evenly divisible by slab_chunk_max (bytes: %d)\n",
                settings.item_size_max, settings.slab_chunk_size_max);
        exit(EX_USAGE);
    }

    if (settings.slab_page_size % settings.slab_chunk_size_max != 0) {
        fprintf(stderr, "slab_chunk_max (bytes: %d) must divide evenly into %d (slab_page_size)\n",
                settings.slab_chunk_size_max, settings.slab_page_size);
        exit(EX_USAGE);
    }

    // Reserve this for the new default. If factor size hasn't changed, use
    // new default.
    /*if (settings.slab_chunk_size_max == 16384 && settings.factor == 1.25) {
        settings.factor = 1.08;
    }*/

    if (slab_sizes_unparsed != NULL) {
        if (_parse_slab_sizes(slab_sizes_unparsed, slab_sizes)) {
            use_slab_sizes = true;
        } else {
            exit(EX_USAGE);
        }
    }

    if (settings.hot_lru_pct + settings.warm_lru_pct > 80) {
        fprintf(stderr, "hot_lru_pct + warm_lru_pct cannot be more than 80%% combined\n");
        exit(EX_USAGE);
    }

    if (settings.temp_lru && !start_lru_maintainer) {
        fprintf(stderr, "temporary_ttl requires lru_maintainer to be enabled\n");
        exit(EX_USAGE);
    }

    if (hash_init(hash_type) != 0) {
        fprintf(stderr, "Failed to initialize hash_algorithm!\n");
        exit(EX_USAGE);
    }

    printf("settings.temp_lru: %d\n", settings.temp_lru);
    printf("settings.lru_segmented: %d\n", settings.lru_segmented);
    /*
     * Use one workerthread to serve each UDP port if the user specified
     * multiple ports
     */
    if (settings.inter != NULL && strchr(settings.inter, ',')) {
        settings.num_threads_per_udp = 1;
    } else {
        settings.num_threads_per_udp = settings.num_threads;
    }

    if (settings.sasl) {
        if (!protocol_specified) {
            settings.binding_protocol = binary_prot;
        } else {
            if (settings.binding_protocol != binary_prot) {
                fprintf(stderr, "ERROR: You cannot allow the ASCII protocol while using SASL.\n");
                exit(EX_USAGE);
            }
        }
    }

    if (tcp_specified && !udp_specified) {
        settings.udpport = settings.port;
    } else if (udp_specified && !tcp_specified) {
        settings.port = settings.udpport;
    }

    if (maxcore != 0) {
        struct rlimit rlim_new;
        /*
         * First try raising to infinity; if that fails, try bringing
         * the soft limit to the hard.
         */
        if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
            rlim_new.rlim_cur = rlim_new.rlim_max = RLIM_INFINITY;
            if (setrlimit(RLIMIT_CORE, &rlim_new)!= 0) {
                /* failed. try raising just to the old max */
                rlim_new.rlim_cur = rlim_new.rlim_max = rlim.rlim_max;
                (void)setrlimit(RLIMIT_CORE, &rlim_new);
            }
        }
        /*
         * getrlimit again to see what we ended up with. Only fail if
         * the soft limit ends up 0, because then no core files will be
         * created at all.
         */

        if ((getrlimit(RLIMIT_CORE, &rlim) != 0) || rlim.rlim_cur == 0) {
            fprintf(stderr, "failed to ensure corefile creation\n");
            exit(EX_OSERR);
        }
    }

    /*
     * If needed, increase rlimits to allow as many connections
     * as needed.
     */

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        fprintf(stderr, "failed to getrlimit number of files\n");
        exit(EX_OSERR);
    } else {
        rlim.rlim_cur = settings.maxconns;
        rlim.rlim_max = settings.maxconns;
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
            fprintf(stderr, "failed to set rlimit for open files. Try starting as root or requesting smaller maxconns value.\n");
            exit(EX_OSERR);
        }
    }


    /* Initialize Sasl if -S was specified */
    if (settings.sasl) {
        init_sasl();
    }

    /* daemonize if requested */
    /* if we want to ensure our ability to dump core, don't chdir to / */
    if (do_daemonize) {
        if (sigignore(SIGHUP) == -1) {
            perror("Failed to ignore SIGHUP");
        }
        if (daemonize(maxcore, settings.verbose) == -1) {
            fprintf(stderr, "failed to daemon() in order to daemonize\n");
            exit(EXIT_FAILURE);
        }
    }

    /* lock paged memory if needed */
    if (lock_memory) {
#ifdef HAVE_MLOCKALL
        int res = mlockall(MCL_CURRENT | MCL_FUTURE);
        if (res != 0) {
            fprintf(stderr, "warning: -k invalid, mlockall() failed: %s\n",
                    strerror(errno));
        }
#else
        fprintf(stderr, "warning: -k invalid, mlockall() not supported on this platform.  proceeding without.\n");
#endif
    }

    /* initialize other stuff */
    logger_init();
    stats_init();
    assoc_init(settings.hashpower_init);
    slabs_init(settings.maxbytes, settings.factor, preallocate,
            use_slab_sizes ? slab_sizes : NULL);

    /*
     * ignore SIGPIPE signals; we can use errno == EPIPE if we
     * need that information
     */
    if (sigignore(SIGPIPE) == -1) {
        perror("failed to ignore SIGPIPE; sigaction");
        exit(EX_OSERR);
    }
    /* start up worker threads if MT mode */
    memcached_thread_init(settings.num_threads);

    if (start_assoc_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    if (start_lru_crawler && start_item_crawler_thread() != 0) {
        fprintf(stderr, "Failed to enable LRU crawler thread\n");
        exit(EXIT_FAILURE);
    }

    if (start_lru_maintainer && start_lru_maintainer_thread() != 0) {
        fprintf(stderr, "Failed to enable LRU maintainer thread\n");
        return 1;
    }

    if (settings.slab_reassign &&
        start_slab_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }

//    if (settings.idle_timeout && start_conn_timeout_thread() == -1) {
//        exit(EXIT_FAILURE);
//    }

    /* initialise clock event */
    //clock_handler(0, 0, 0);

    /* create unix mode sockets after dropping privileges */
    //if (settings.socketpath != NULL) {
    //    errno = 0;
    //    if (server_socket_unix(settings.socketpath,settings.access)) {
    //        vperror("failed to listen on UNIX socket: %s", settings.socketpath);
    //        exit(EX_OSERR);
    //    }
    //}

    /* create the listening socket, bind it, and init */
//    if (settings.socketpath == NULL) {
//        const char *portnumber_filename = getenv("MEMCACHED_PORT_FILENAME");
//        char *temp_portnumber_filename = NULL;
//        size_t len;
//        FILE *portnumber_file = NULL;
//
//        if (portnumber_filename != NULL) {
//            len = strlen(portnumber_filename)+4+1;
//            temp_portnumber_filename = malloc(len);
//            snprintf(temp_portnumber_filename,
//                     len,
//                     "%s.lck", portnumber_filename);
//
//            portnumber_file = fopen(temp_portnumber_filename, "a");
//            if (portnumber_file == NULL) {
//                fprintf(stderr, "Failed to open \"%s\": %s\n",
//                        temp_portnumber_filename, strerror(errno));
//            }
//        }

    //    errno = 0;
        //if (settings.port && server_sockets(settings.port, tcp_transport,
        //                                   portnumber_file)) {
        //    vperror("failed to listen on TCP port %d", settings.port);
        //    exit(EX_OSERR);
        //}

        /*
         * initialization order: first create the listening sockets
         * (may need root on low ports), then drop root if needed,
         * then daemonise if needed, then init libevent (in some cases
         * descriptors created by libevent wouldn't survive forking).
         */

        /* create the UDP listening socket and bind it */
  //      errno = 0;
  //      if (settings.udpport && server_sockets(settings.udpport, udp_transport,
  //                                            portnumber_file)) {
  //          vperror("failed to listen on UDP port %d", settings.udpport);
  //          exit(EX_OSERR);
  //      }

  //      if (portnumber_file) {
  //          fclose(portnumber_file);
  //          rename(temp_portnumber_filename, portnumber_filename);
  //          free(temp_portnumber_filename);
  //      }
  //  }

    /* Give the sockets a moment to open. I know this is dumb, but the error
     * is only an advisory.
     */
   // usleep(1000);
   // if (stats_state.curr_conns + stats_state.reserved_fds >= settings.maxconns - 1) {
   //     fprintf(stderr, "Maxconns setting is too low, use -c to increase.\n");
   //     exit(EXIT_FAILURE);
   // }

  //  if (pid_file != NULL) {
  //      save_pid(pid_file);
  //  }

    /* Drop privileges no longer needed */
 //   drop_privileges();

    /* Initialize the uriencode lookup table. */
    uriencode_init();

    /* enter the event loop */
//    if (event_base_loop(main_base, 0) != 0) {
//        retval = EXIT_FAILURE;
//    }
//
//    stop_assoc_maintenance_thread();

    /* remove the PID file if we're a daemon */
//    if (do_daemonize)
//        remove_pidfile(pid_file);
    /* Clean up strdup() call for bind() address */
//    if (settings.inter)
//      free(settings.inter);
//    if (l_socket)
//      free(l_socket);
//    if (u_socket)
//      free(u_socket);

    return retval;
}
