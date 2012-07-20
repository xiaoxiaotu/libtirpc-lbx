/*
 * Copyright (c) 2012 Linux Box Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <sys/types.h>
#include <netinet/in.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/types.h>
#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <rpc/auth.h>
#include <rpc/svc_auth.h>
#include <rpc/svc.h>
#include <rpc/clnt.h>
#include <stddef.h>
#include <assert.h>

#include "intrinsic.h"
#include "rpc_com.h"

#include <rpc/xdr_vrec.h>

static bool xdr_vrec_getlong(XDR *, long *);
static bool xdr_vrec_putlong(XDR *, const long *);
static bool xdr_vrec_getbytes(XDR *, char *, u_int);
static bool xdr_vrec_putbytes(XDR *, const char *, u_int);
static bool xdr_vrec_getbufs(XDR *, xdr_uio *, u_int, u_int);
static bool xdr_vrec_putbufs(XDR *, xdr_uio *, u_int, u_int);
static u_int xdr_vrec_getpos(XDR *);
static bool xdr_vrec_setpos(XDR *, u_int);
static int32_t *xdr_vrec_inline(XDR *, u_int);
static void xdr_vrec_destroy(XDR *);
static bool xdr_vrec_noop(void);

typedef bool (* dummyfunc3)(XDR *, int, void *);
typedef bool (* dummyfunc4)(XDR *, const char *, u_int, u_int);

static const struct  xdr_ops xdr_vrec_ops = {
    xdr_vrec_getlong,
    xdr_vrec_putlong,
    xdr_vrec_getbytes,
    xdr_vrec_putbytes,
    xdr_vrec_getpos,
    xdr_vrec_setpos,
    xdr_vrec_inline,
    xdr_vrec_destroy,
    (dummyfunc3) xdr_vrec_noop, /* x_control */
    xdr_vrec_getbufs,
    xdr_vrec_putbufs
};

#define LAST_FRAG ((u_int32_t)(1 << 31))

static u_int fix_buf_size(u_int);
static bool flush_out(V_RECSTREAM *, bool);
static bool fill_input_buf(V_RECSTREAM *);
static bool vrec_get_input_bytes(V_RECSTREAM *, char *, int);
static bool vrec_get_input_fragment_bytes(V_RECSTREAM *, char **, int);
static bool vrec_set_input_fragment(V_RECSTREAM *);
static bool vrec_skip_input_bytes(V_RECSTREAM *, long);

/* XXX might not be faster than jemalloc, &c? */
static inline void
init_prealloc_queues(V_RECSTREAM *vstrm)
{
    int ix;
    struct v_rec *vrec;

    opr_queue_Init(&vstrm->prealloc.v_req.q);
    vstrm->prealloc.v_req.size = 0;

    opr_queue_Init(&vstrm->prealloc.v_req_buf.q);
    vstrm->prealloc.v_req_buf.size = 0;

    for (ix = 0; ix < VQSIZE; ++ix) {
        vrec = mem_alloc(sizeof(struct v_rec));
        opr_queue_Append(&vstrm->prealloc.v_req.q, &vrec->ioq);
        (vstrm->prealloc.v_req.size)++;
    }
}

static inline void
vrec_init_queue(struct v_rec_queue *q)
{
    opr_queue_Init(&q->q);
    q->size = 0;
}

static inline struct v_rec *
vrec_get_vrec(V_RECSTREAM *vstrm)
{
    struct v_rec *vrec;
    if (unlikely(vstrm->prealloc.v_req.size == 0)) {
        vrec = mem_alloc(sizeof(struct v_rec));
    } else {
        vrec = opr_queue_First(&vstrm->prealloc.v_req.q, struct v_rec, ioq);
        opr_queue_Remove(&vrec->ioq);
        (vstrm->prealloc.v_req.size)--;
    }
    return (vrec);
}

static inline void
vrec_put_vrec(V_RECSTREAM *vstrm, struct v_rec *vrec)
{
    if (unlikely(vstrm->prealloc.v_req.size > VQSIZE))
        mem_free(vrec, sizeof(struct v_rec));
    else {
        opr_queue_Append(&vstrm->prealloc.v_req.q, &vrec->ioq);
        (vstrm->prealloc.v_req.size)++;
    }
}

#if 0 /* jemalloc docs warn about reclaim */
#define vrec_alloc_buffer(size) mem_alloc_aligned(0x8, (size))
#else
#define vrec_alloc_buffer(size) mem_alloc((size))
#endif /* 0 */
#define vrec_free_buffer(addr) mem_free((addr), 0)

static inline void
init_discard_buffers(V_RECSTREAM *vstrm)
{
    int ix;
    struct iovec *iov;

    for (ix = 0; ix < 4; ix++) {
        iov = &(vstrm->st_u.in.iovsink[ix]);
        iov->iov_base = vrec_alloc_buffer(vstrm->def_bsize);
        iov->iov_len = 0;
    }
}

static inline void
free_discard_buffers(V_RECSTREAM *vstrm)
{
    int ix;
    struct iovec *iov;

    for (ix = 0; ix < 4; ix++) {
        iov = &(vstrm->st_u.in.iovsink[ix]);
        vrec_free_buffer(iov->iov_base);
    }
}

#define vrec_qlen(q) ((q)->size)
#define vrec_fpos(vstrm) (&vstrm->ioq.fpos)
#define vrec_lpos(vstrm) (&vstrm->ioq.lpos)

static inline void vrec_append_rec(struct v_rec_queue *q, struct v_rec *vrec)
{
    opr_queue_Append(&q->q, &vrec->ioq);
    (q->size)++;
}

static inline void
vrec_init_ioq(V_RECSTREAM *vstrm)
{
    struct v_rec *vrec;
    struct v_rec_pos_t *pos;

    vrec = vrec_get_vrec(vstrm);
    vrec->off = 0;
    vrec->len = 0;
    vrec->size = vstrm->def_bsize;
    vrec->base = vrec_alloc_buffer(vrec->size);
    vrec->flags = VREC_FLAG_RECLAIM;
    vrec_append_rec(&vstrm->ioq, vrec);
    (vstrm->ioq.size)++;
}

static inline void
vrec_rele(V_RECSTREAM *vstrm, struct v_rec *vrec)
{
    (vrec->refcnt)--;
    if (unlikely(vrec->refcnt == 0)) {
        if (vrec->flags & VREC_FLAG_RECLAIM) {
            vrec_free_buffer(vrec->base);
        }
        /* return to freelist */
        vrec_put_vrec(vstrm, vrec);
    }
}

static inline size_t
vrec_readahead_bytes(V_RECSTREAM *vstrm, int len, u_int flags)
{
    struct v_rec_pos_t *pos;
    struct iovec iov[1];
    uint32_t nbytes;

    pos = vrec_fpos(vstrm);
    iov->iov_base = pos->vrec->base + pos->vrec->off;
    iov->iov_len = len;
    nbytes = vstrm->ops.readv(vstrm->vp_handle, iov, 1, flags);
    vstrm->st_u.in.rbtbc += nbytes;
    pos->vrec->len += nbytes;

    return (nbytes);
}

#define vrec_nb_readahead(vstrm) \
    vrec_readahead_bytes((vstrm), (vstrm)->st_u.in.readahead_bytes, \
                         VREC_FLAG_NONBLOCK);

enum vrec_cursor
{
    VREC_FPOS,
    VREC_LPOS,
    VREC_RESET_POS
};

/*
 * Set initial read/insert or fill position.
 */
static inline void
vrec_stream_reset(V_RECSTREAM *vstrm, enum vrec_cursor wh_pos)
{
    struct v_rec_pos_t *pos;
    struct v_rec *vrec;

    vrec = opr_queue_First(&vstrm->ioq.q, struct v_rec, ioq);

    switch (wh_pos) {
    case VREC_FPOS:
        pos = vrec_fpos(vstrm);
        break;
    case VREC_LPOS:
        pos = vrec_lpos(vstrm);
        break;
    case VREC_RESET_POS:
        vrec_stream_reset(vstrm, VREC_FPOS);
        vrec_stream_reset(vstrm, VREC_LPOS);
        return;
        break;
    default:
        abort();
        break;
    }

    pos->vrec = vrec;
    pos->loff = 0;
    pos->bpos = 1; /* first position */
    pos->boff = 0;
}

/*
 * Advance read/insert or fill position.
 */
static inline void
vrec_next(V_RECSTREAM *vstrm, enum vrec_cursor wh_pos, u_int flags)
{
    switch (wh_pos) {
    case VREC_FPOS:
        break;
    case VREC_LPOS:
        break;
    default:
        abort();
        break;
    }
}

/*
 * Create an xdr handle
 */
void
xdr_vrec_create(XDR *xdrs,
                enum xdr_vrec_direction direction, void *xhandle,
                size_t (*xreadv)(void *, struct iovec *, int, u_int),
                size_t (*xwritev)(void *, struct iovec *, int, u_int),
                u_int def_bsize, u_int flags)
{
    V_RECSTREAM *vstrm = mem_alloc(sizeof(V_RECSTREAM));

    if (vstrm == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "xdr_vrec_create: out of memory");
        return;
    }

    xdrs->x_ops = &xdr_vrec_ops;
    xdrs->x_private = vstrm;

    vstrm->vp_handle = xhandle;

    /* init queues */
    vrec_init_queue(&vstrm->ioq);
    vrec_init_queue(&vstrm->relq);

    /* buffer tuning */
    vstrm->def_bsize = def_bsize;

    switch (direction) {
    case XDR_VREC_INREC:
        vstrm->ops.readv = xreadv;
        vstrm->st_u.in.readahead_bytes = 1200; /* XXX PMTU? */
        vstrm->st_u.in.fbtbc = 0;
        vstrm->st_u.in.rbtbc = 0;
        vstrm->st_u.in.haveheader = FALSE;
        vstrm->st_u.in.last_frag = TRUE;
        break;
    case XDR_VREC_OUTREC:
        vstrm->ops.writev = xwritev;
        vrec_init_ioq(vstrm);
        /* XXX finish */
        vstrm->st_u.out.frag_sent = FALSE;
        break;
    default:
        abort();
        break;
    }

    init_prealloc_queues(vstrm);
    init_discard_buffers(vstrm);

    return;
}

/*
 * The routines defined below are the xdr ops which will go into the
 * xdr handle filled in by xdr_vrec_create.
 */
static bool
xdr_vrec_getlong(XDR *xdrs,  long *lp)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    int32_t *buf = NULL;

    /* first try the inline, fast case */
    if (vstrm->st_u.in.fbtbc >= sizeof(int32_t)) {
        struct v_rec_pos_t *pos = vrec_lpos(vstrm);
        buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
        *lp = (long)ntohl(*buf);
    } else {
        if (! xdr_vrec_getbytes(
                xdrs, (char *)(void *)buf, sizeof(int32_t)))
            return (FALSE);
        *lp = (long)ntohl(*buf);
    }
    return (TRUE);
}

static bool
xdr_vrec_putlong(XDR *xdrs, const long *lp)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    int32_t *dest_lp = ((int32_t *)(void *)(rstrm->out_finger));

    if ((rstrm->out_finger += sizeof(int32_t)) > rstrm->out_boundry) {
        /*
         * this case should almost never happen so the code is
         * inefficient
         */
        rstrm->out_finger -= sizeof(int32_t);
        rstrm->frag_sent = TRUE;
        if (! flush_out(rstrm, FALSE))
            return (FALSE);
        dest_lp = ((int32_t *)(void *)(rstrm->out_finger));
        rstrm->out_finger += sizeof(int32_t);
    }
    *dest_lp = (int32_t)htonl((u_int32_t)(*lp));
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool
xdr_vrec_getbytes(XDR *xdrs, char *addr, u_int len)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    int current;

    while (len > 0) {
        current = (int)vstrm->st_u.in.fbtbc;
        if (current == 0) {
            if (vstrm->st_u.in.last_frag)
                return (FALSE);
            if (! vrec_set_input_fragment(vstrm))
                return (FALSE);
            continue;
        }
        current = (len < current) ? len : current;
        if (! vrec_get_input_bytes(vstrm, addr, current))
            return (FALSE);
        addr += current;
        vstrm->st_u.in.fbtbc -= current;
        len -= current;
    }
    return (TRUE);
}

static bool
xdr_vrec_putbytes(XDR *xdrs, const char *addr, u_int len)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    int current;

    while (len > 0) {
        current = (int)rstrm->fbtbc;
        if (current == 0) {
            if (rstrm->last_frag)
                return (FALSE);
            if (! set_input_fragment(rstrm))
                return (FALSE);
            continue;
        }
        current = (len < current) ? len : current;
        if (! get_input_bytes(rstrm, addr, current))
            return (FALSE);
        addr += current;
        rstrm->fbtbc -= current;
        len -= current;
    }
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool
xdr_vrec_getbufs(XDR *xdrs, xdr_uio *uio, u_int len, u_int flags)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    size_t current;

    while (len > 0) {
        current = (size_t)((u_long)rstrm->out_boundry -
                           (u_long)rstrm->out_finger);
        current = (len < current) ? len : current;
        memmove(rstrm->out_finger, addr, current);
        rstrm->out_finger += current;
        addr += current;
        len -= current;
        if (rstrm->out_finger == rstrm->out_boundry) {
            rstrm->frag_sent = TRUE;
            if (! flush_out(rstrm, FALSE))
                return (FALSE);
        }
    }
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool
xdr_vrec_putbufs(XDR *xdrs, xdr_uio *uio, u_int len, u_int flags)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    size_t current;

    while (len > 0) {
        current = (size_t)((u_long)rstrm->out_boundry -
                           (u_long)rstrm->out_finger);
        current = (len < current) ? len : current;
        memmove(rstrm->out_finger, addr, current);
        rstrm->out_finger += current;
        addr += current;
        len -= current;
        if (rstrm->out_finger == rstrm->out_boundry) {
            rstrm->frag_sent = TRUE;
            if (! flush_out(rstrm, FALSE))
                return (FALSE);
        }
    }
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static u_int
xdr_vrec_getpos(XDR *xdrs)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);
    struct v_rec_pos_t *lpos = vrec_lpos(vstrm);

    return (lpos->loff);
}

static bool
xdr_vrec_setpos(XDR *xdrs, u_int pos)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);
    struct v_rec_pos_t *lpos = vrec_lpos(vstrm);
    struct opr_queue *qiter;
    struct v_rec *vrec;
    u_int resid = 0;
    int ix;

    ix = 0;
    for (opr_queue_Scan(&(vstrm->ioq.q), qiter)) {
        vrec = opr_queue_Entry(&vstrm->ioq.q, struct v_rec, ioq);
        if ((vrec->len + resid) > pos) {
            lpos->vrec = vrec;
            lpos->bpos = ix;
            lpos->loff = pos;
            lpos->boff = (pos-resid);
            return (TRUE);
        }
        resid += vrec->len;
        ++ix;
    }
    return (FALSE);
}

static int32_t *
xdr_vrec_inline(XDR *xdrs, u_int len)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos;
    int32_t *buf = NULL;

    /* we keep xdrrec's inline concept intact--encode or decode
     * bytes in the stream buffer */

    switch (vstrm->direction) {
    case XDR_VREC_INREC:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            abort();
            break;
        case XDR_DECODE:
            if (vstrm->st_u.in.fbtbc >= len) {
                pos = vrec_lpos(vstrm);
                buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
                vstrm->st_u.in.fbtbc -= len;
                pos->boff += len;
            }
            break;
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUTREC:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            /* TODO: implement */
            break;
        case XDR_DECODE:
        default:
            abort();
            break;
        }
        break;
    default:
        abort();
        break;
    }

    return (buf);
}

static void
xdr_vrec_destroy(XDR *xdrs)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec *vrec;

    /* release queued buffers */
    while (vstrm->ioq.size > 0) {
        vrec = opr_queue_First(&vstrm->ioq.q, struct v_rec, ioq);
        vrec_rele(vstrm, vrec);
        opr_queue_Remove(&vrec->ioq);
        (vstrm->ioq.size)--;
    }
    free_discard_buffers(vstrm);
    mem_free(vstrm, sizeof(V_RECSTREAM));
}


/*
 * Exported routines to manage xdr records
 */
static inline void
vrec_truncate_input_q(V_RECSTREAM *vstrm, int max)
{
    struct v_rec *vrec;

    /* the ioq queue can contain shared and special segments (eg, mapped
     * buffers).  if present, these segements will also be threaded on
     * a release (sub) queue.
     *
     * we are truncating a recv queue.  if any special segments are
     * present, we must detach them.
     */
    while (unlikely(! opr_queue_IsEmpty(&vstrm->relq.q))) {
        vrec = opr_queue_First(&vstrm->relq.q, struct v_rec, relq);
        /* dequeue segment */
        opr_queue_Remove(&vrec->ioq);
        (vstrm->ioq.size)--;
        /* decref */
        (vrec->refcnt)--;
        /* vrec should be ref'd elsewhere (it was special) */
        if (unlikely(vrec->refcnt == 0)) {
            opr_queue_Remove(&vrec->relq);
            (vstrm->relq.size)--;
            if (unlikely(vrec->flags & VREC_FLAG_RECLAIM)) {
                vrec_free_buffer(vrec->base);
            }
            /* recycle it */
            vrec_put_vrec(vstrm, vrec);
        }
    }

    /* any segment left in ioq is a network buffer.  enforce upper
     * bound on ioq size.
     */
    while (unlikely(vstrm->ioq.size > max)) {
        vrec = opr_queue_Last(&vstrm->ioq.q, struct v_rec, ioq);
        opr_queue_Remove(&vrec->ioq);
        (vstrm->ioq.size)--;
        /* almost certainly recycles */
        vrec_rele(vstrm, vrec);
    }

    /* ideally, the first read on the stream */
    if (unlikely(vstrm->ioq.size == 0))
        vrec_init_ioq(vstrm);

    /* stream reset */
    vrec_stream_reset(vstrm, VREC_RESET_POS);

    return;
}

/*
 * Before reading (deserializing from the stream), one should always call
 * this procedure to guarantee proper record alignment.
 */
bool
xdr_vrec_skiprecord(XDR *xdrs)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);

    switch (vstrm->direction) {
    case XDR_VREC_INREC:
        switch (xdrs->x_op) {
        case XDR_DECODE:
            /* stream reset */
            while (vstrm->st_u.in.fbtbc > 0 ||
                   (! vstrm->st_u.in.last_frag)) {
                if (! vrec_skip_input_bytes(vstrm, vstrm->st_u.in.fbtbc))
                    return (FALSE);
                if ((! vstrm->st_u.in.last_frag) &&
                    (! vrec_set_input_fragment(vstrm)))
                    return (FALSE);
            }
            /* bound queue size and support future mapped reads */
            vrec_truncate_input_q(vstrm, 8);
            vstrm->st_u.in.fbtbc = 0;
            vstrm->st_u.in.rbtbc = 0;
            if (! vstrm->st_u.in.haveheader) {
                if (! vrec_set_input_fragment(vstrm))
                    return (FALSE);
            }
            /* reset logical stream position */
            *(vrec_lpos(vstrm)) = *(vrec_fpos(vstrm));
            break;
        case XDR_ENCODE:
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUTREC:
    default:
        abort();
        break;
    }

    return (TRUE);
}

/*
 * Look ahead function.
 * Returns TRUE iff there is no more input in the buffer
 * after consuming the rest of the current record.
 */
bool
xdr_vrec_eof(XDR *xdrs)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);

    switch (vstrm->direction) {
    case XDR_VREC_INREC:
        switch (xdrs->x_op) {
        case XDR_DECODE:
            /* stream reset */
            while (vstrm->st_u.in.fbtbc > 0 ||
                   (! vstrm->st_u.in.last_frag)) {
                if (! vrec_skip_input_bytes(vstrm, vstrm->st_u.in.fbtbc))
                    return (TRUE);
                if ((! vstrm->st_u.in.last_frag) &&
                    (! vrec_set_input_fragment(vstrm)))
                    return (TRUE);
                /* XXX missing boundary case? */
#if 0 /* corresponding logic in xdrrec: */
                if (rstrm->in_finger == rstrm->in_boundry)
                    return (TRUE);
#endif /* 0 */
            }
            break;
        case XDR_ENCODE:
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUTREC:
    default:
        abort();
        break;
    }
    return (FALSE);
}

/*
 * The client must tell the package when an end-of-record has occurred.
 * The second paramter tells whether the record should be flushed to the
 * (output) tcp stream.  (This let's the package support batched or
 * pipelined procedure calls.)  TRUE => immmediate flush to tcp connection.
 */
bool
xdr_vrec_endofrecord(XDR *xdrs, bool sendnow)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    u_long len;  /* fragment length */

    if (sendnow || rstrm->frag_sent ||
        ((u_long)rstrm->out_finger + sizeof(u_int32_t) >=
         (u_long)rstrm->out_boundry)) {
        rstrm->frag_sent = FALSE;
        return (flush_out(rstrm, TRUE));
    }
    len = (u_long)(rstrm->out_finger) - (u_long)(rstrm->frag_header) -
        sizeof(u_int32_t);
    *(rstrm->frag_header) = htonl((u_int32_t)len | LAST_FRAG);
    rstrm->frag_header = (u_int32_t *)(void *)rstrm->out_finger;
    rstrm->out_finger += sizeof(u_int32_t);
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

/*
 * Internal useful routines
 */
static bool
flush_out(V_RECSTREAM *rstrm, bool eor)
{
#if 0 /* XXX */
    u_int32_t eormask = (eor == TRUE) ? LAST_FRAG : 0;
    u_int32_t len = (u_int32_t)((u_long)(rstrm->out_finger) -
                                (u_long)(rstrm->frag_header) - sizeof(u_int32_t));

    *(rstrm->frag_header) = htonl(len | eormask);
    len = (u_int32_t)((u_long)(rstrm->out_finger) -
                      (u_long)(rstrm->out_base));
    if ((*(rstrm->writeit))(rstrm->tcp_handle, rstrm->out_base, (int)len)
        != (int)len)
        return (FALSE);
    rstrm->frag_header = (u_int32_t *)(void *)rstrm->out_base;
    rstrm->out_finger = (char *)rstrm->out_base + sizeof(u_int32_t);
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

/* Stream read operation intended for small, e.g., header-length
 * reads ONLY.  Note:  len is required to be less than the queue
 * default buffer size. */
static bool
vrec_get_input_fragment_bytes(V_RECSTREAM *vstrm, char **addr, int len)
{
    struct v_rec_pos_t *pos;

    if (! vstrm->st_u.in.rbtbc) {
        vrec_readahead_bytes(vstrm, vstrm->st_u.in.readahead_bytes,
                             VREC_FLAG_NONE);
        if (len <= vstrm->st_u.in.rbtbc) {
            pos = vrec_fpos(vstrm);
            *addr = (void *)(pos->vrec->base + pos->boff);
            pos->boff += len;
            return (TRUE);
        }
    }
    return (FALSE);
}

static bool  /* next two bytes of the input stream are treated as a header */
vrec_set_input_fragment(V_RECSTREAM *vstrm)
{
    u_int32_t header;
    char *addr;

    if (! vrec_get_input_fragment_bytes(vstrm, &addr, sizeof(header)))
        return (FALSE);
    header = ntohl(*((u_int32_t *)addr));
    vstrm->st_u.in.last_frag = ((header & LAST_FRAG) == 0) ? FALSE : TRUE;
    /*
     * Sanity check. Try not to accept wildly incorrect
     * record sizes. Unfortunately, the only record size
     * we can positively identify as being 'wildly incorrect'
     * is zero. Ridiculously large record sizes may look wrong,
     * but we don't have any way to be certain that they aren't
     * what the client actually intended to send us.
     */
    if (header == 0)
        return(FALSE);
    vstrm->st_u.in.fbtbc = header & (~LAST_FRAG);

    return (TRUE);
}

/* Read contguous bytes from stream. */
static bool
vrec_get_input_bytes(V_RECSTREAM *vstrm, char *addr, int len)
{
    /* TODO: implement */
    abort();

    return (FALSE);
}

/* Consume and discard cnt bytes from the input stream. */
static bool
vrec_skip_input_bytes(V_RECSTREAM *vstrm, long cnt)
{
    int ix;
    u_int32_t nbytes, resid;
    struct iovec *iov;

    while (cnt > 0) {
        for (ix = 0, resid = cnt; ((resid > 0) && (ix < 4)); ++ix) {
            iov = &(vstrm->st_u.in.iovsink[ix]);
            iov->iov_len = MIN(resid, vstrm->def_bsize);
            resid -= iov->iov_len;
        }
        nbytes = vstrm->ops.readv(vstrm->vp_handle,
                                  (struct iovec *) &(vstrm->st_u.in.iovsink),
                                  ix+1 /* iovcnt */,
                                  VREC_FLAG_NONE);
        cnt -= nbytes;
    }
    return (TRUE);
}

static u_int
fix_buf_size(u_int s)
{

    if (s < 100)
        s = 4000;
    return (RNDUP(s));
}

static bool
xdr_vrec_noop(void)
{
    return (FALSE);
}
