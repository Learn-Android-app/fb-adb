#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/file.h>
#include <ctype.h>
#include "dbg.h"
#include "util.h"
#include "ringbuf.h"
#include "proto.h"
#include "channel.h"

#ifndef NDEBUG

static FILE* dbgout = NULL;
static int dbglock_fd = -1;
static int dbglock_level = 0;

void
dbg_init(void)
{
    const char* dv = getenv("ADBX_DEBUG");
    if (strcmp(dv, "1") == 0) {
        dbgout = fdopen(fileno(stderr), "w");
    } else if (dv[0] == '>') {
        dbgout = fopen(dv+1, "a");
        if (dbgout != NULL)
            dbglock_fd = fileno(dbgout);
    }
}

static bool
dbg_enabled_p(void)
{
    return dbgout != NULL;
}

void
dbg(const char* fmt, ...)
{
    if (dbg_enabled_p()) {
        SCOPED_RESLIST(rl_dbg);
        dbglock();
        va_list args;
        fprintf(dbgout, "%s(%04d): ", prgname, getpid());
        va_start(args, fmt);
        vfprintf(dbgout, fmt, args);
        va_end(args);
        fputc('\n', dbgout);
        fflush(dbgout);
    }
}


static void
cleanup_dbginit(void* arg)
{
    unlink((const char*) arg);
}

void
dbglock_init(void)
{
    if (dbglock_fd != -1)
        return;

    const char envvar[] = "ADBX_DBGLOCK_NAME";
    /* No, we can't just inherit the file descriptor.  Without a
     * separate file open, taking the lock won't block.  */
    
    const char* fn = getenv(envvar);
    if (fn == NULL) {
#ifdef __ANDROID__
        const char* pfx = "/data/local/tmp";
#else
        const char* pfx = "/tmp";
#endif

        char* tmpfname = xaprintf("%s/adbx-dbg-XXXXXX", pfx);
        struct cleanup* cl = cleanup_allocate();
        int tmpfd = mkostemp(tmpfname, O_CLOEXEC | O_RDWR);
        if (tmpfd != -1) {
            setenv(envvar, tmpfname, 1);
            cleanup_commit(cl, cleanup_dbginit, tmpfname);
            dbglock_fd = tmpfd;
        }

        return;
    }

    dbglock_fd = open(fn, O_CLOEXEC | O_RDWR);
}

static void
cleanup_dbglock(void* ignored)
{
    if (--dbglock_level == 0)
        flock(dbglock_fd, LOCK_UN);
}

void
dbglock(void)
{
    if (!dbg_enabled_p())
        return;

    if (dbglock_fd == -1)
        return;

    if (dbglock_level++ == 0)
        flock(dbglock_fd, LOCK_EX);

    cleanup_commit(cleanup_allocate(), cleanup_dbglock, 0);
}

void
iovec_dbg(const struct iovec* iov, unsigned nio)
{
    for (unsigned i = 0; i < nio; ++i) {
        char* b = (char*)iov[i].iov_base;
        size_t blen = iov[i].iov_len;
        for (unsigned j = 0; j < blen; ++j) {
            dbg("  iov[%u][%u] c 0x%02x %c", i, j, b[j], isprint(b[j]) ? b[j] : '.');
        }
    }

}

void
ringbuf_dbg(const struct ringbuf* rb)
{
    struct iovec iov[2];
    ringbuf_readable_iov(rb, iov, ringbuf_size(rb));
    iovec_dbg(iov, ARRAYSIZE(iov));
}

void
dbgmsg(struct msg* msg, const char* tag)
{
    switch (msg->type) {
        case MSG_CHANNEL_DATA: {
            struct msg_channel_data* m = (void*) msg;
            dbg("%s MSG_CHANNEL_DATA ch=%s sz=%u, payloadsz=%lu",
                tag, chname(m->channel), m->msg.size, m->msg.size - sizeof (*m));
            break;
        }
        case MSG_CHANNEL_WINDOW: {
            struct msg_channel_window* m = (void*) msg;
            dbg("%s MSG_CHANNEL_WINDOW ch=%s d=%u",
                tag, chname(m->channel), m->window_delta);
            break;
        }
        case MSG_CHANNEL_CLOSE: {
            dbg("%s MSG_CHANNEL_CLOSE ch=%s",
                tag, chname(((struct msg_channel_close*)msg)->channel));
            break;
        }
        default: {
            dbg("%s MSG_??? type=%d sz=%d", tag, msg->type, msg->size);
            break;
        }
    }
}

const char*
chname(int chno)
{
    static const char* chnames[] = {
        "FROM_PEER",
        "TO_PEER",
        "CHILD_STDIN",
        "CHILD_STDOUT",
        "CHILD_STDERR"
    };

    if (chno < ARRAYSIZE(chnames))
        return chnames[chno];

    return "?!?!?";
}

void
dbgch(const char* label, struct channel** ch, unsigned nrch)
{
    SCOPED_RESLIST(rl_dbgch);
    unsigned chno;

    dbglock();

    dbg("DBGCH[%s]", label);

    for (chno = 0; chno < nrch; ++chno) {
        struct channel* c = ch[chno];
        struct pollfd p = channel_request_poll(ch[chno]);
        const char* pev;
        switch (p.events) {
            case POLLIN | POLLOUT:
                pev = "POLLIN,POLLOUT";
                break;
            case POLLIN:
                pev = "POLLIN";
                break;
            case POLLOUT:
                pev = "POLLOUT";
                break;
            case 0:
                pev = "NONE";
                break;
            default:
                pev = xaprintf("%xd", p.events);
                break;
        }

        assert(p.fd == -1 || p.fd == c->fdh->fd);

        dbg("  %-18s size:%-4lu room:%-4lu window:%-4d %s%-2s %p %s",
            xaprintf("ch[%d=%s]", chno, chname(chno)),
            ringbuf_size(c->rb),
            ringbuf_room(c->rb),
            c->window,
            (c->dir == CHANNEL_FROM_FD ? "<" : ">"),
            ((c->fdh != NULL)
             ? xaprintf("%d", c->fdh->fd)
             : (c->sent_eof ? "!!" : "!?")),
            c,
            pev);
    }
}

#endif
