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

#ifndef GSS_INTERNAL_H
#define GSS_INTERNAL_H

#include <config.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <rpc/rpc.h>
#include <misc/rbtree_x.h>

#ifdef HAVE_HEIMDAL
#include <gssapi.h>
#define gss_nt_service_name GSS_C_NT_HOSTBASED_SERVICE
#else
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_generic.h>
#endif
#include <rpc/auth_gss.h>

extern SVCAUTH svc_auth_none;

/*
 * from mit-krb5-1.2.1 mechglue/mglueP.h:
 * Array of context IDs typed by mechanism OID
 */
typedef struct gss_union_ctx_id_t
{
    gss_OID mech_type;
    gss_ctx_id_t internal_ctx_id;
} gss_union_ctx_id_desc, *gss_union_ctx_id_t;


#define _MSPAC_SUPPORT 1

#define URN_MSPAC "urn:mspac:"

struct mspac_buf
{
    size_t length;
    uint8_t *data; /* krb5_octet */
};    struct rbtree_x xt;

struct svc_rpc_gss_data
{
    struct opr_rbtree_node node_k;
    mutex_t lock;
    u_int refcnt;
    struct hk {
        uint64_t k;
    };
    bool established;
    gss_ctx_id_t ctx;  /* context id */
    struct rpc_gss_sec sec; /* security triple */
    gss_buffer_desc cname;  /* GSS client name */
    u_int seq;
    u_int win;
    u_int seqlast;
    uint32_t seqmask;
    gss_name_t client_name;
    gss_buffer_desc checksum;
#ifdef _MSPAC_SUPPORT
    struct mspac_buf pac;
#endif
};

static inline struct svc_rpc_gss_data *
alloc_svc_rpc_gss_data(void)
{
    struct svc_rpc_gss_data *gd = mem_zalloc(sizeof(struct svc_rpc_gss_data));
    mutex_init(&gd->lock, NULL);
    return (gd);
}

static inline void
free_svc_rpc_gss_data(struct svc_rpc_gss_data *gd)
{
    mutex_destroy(&gd->lock);
    mem_free(gd, 0);
}


#endif /* GSS_INTERNAL_H */
