/*
 * Copyright (c) 2017, Carnegie Mellon University.
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
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include "fake_nexus.h"

#define NEXUS_DEBUG

/*
 * msg_abort: abort with a message
 */
static inline void msg_abort(const char* msg)
{
    if (errno != 0) {
        fprintf(stderr, "Error: %s (%s)\n", msg, strerror(errno));   
    } else {
        fprintf(stderr, "Error: %s\n", msg);
    }

    abort();
}

static void print_hg_addr(hg_class_t *hgcl, char *str, hg_addr_t hgaddr)
{
    char *addr_str = NULL;
    hg_size_t addr_size = 0;
    hg_return_t hret;

    hret = HG_Addr_to_string(hgcl, NULL, &addr_size, hgaddr);
    if (hgaddr == NULL)
        msg_abort("HG_Addr_to_string failed");

    addr_str = (char *)malloc(addr_size);
    if (addr_str == NULL)
        msg_abort("malloc failed");

    hret = HG_Addr_to_string(hgcl, addr_str, &addr_size, hgaddr);
    if (hret != HG_SUCCESS)
        msg_abort("HG_Addr_to_string failed");

    fprintf(stdout, "Mercury address: %s => %s\n", str, addr_str);
}

static void init_local_comm(nexus_ctx_t *nctx)
{
    int ret;

#if MPI_VERSION >= 3
    ret = MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0,
                              MPI_INFO_NULL, &nctx->localcomm);
    if (ret != MPI_SUCCESS)
        msg_abort("MPI_Comm_split_type failed");
#else
    /* XXX: Need to find a way to deal with MPI_VERSION < 3 */
    msg_abort("Nexus needs MPI version 3 or higher");
#endif
}

typedef struct {
    hg_context_t *hgctx;
    int bgdone;
} bgthread_dat_t;

/*
 * Network support pthread. Need to call progress to push the network and then
 * trigger to run the callback.
 */
static void *nexus_bgthread(void *arg)
{
    bgthread_dat_t *bgdat = (bgthread_dat_t *)arg;
    hg_return_t hret;

#ifdef NEXUS_DEBUG
    fprintf(stdout, "Network thread running\n");
#endif

    /* while (not done sending or not done recving */
    while (!bgdat->bgdone) {
        unsigned int count = 0;

        do {
            hret = HG_Trigger(bgdat->hgctx, 0, 1, &count);
        } while (hret == HG_SUCCESS && count);

        if (hret != HG_SUCCESS && hret != HG_TIMEOUT)
            msg_abort("nexus_bgthread: HG_Trigger failed");

        hret = HG_Progress(bgdat->hgctx, 100);
        if (hret != HG_SUCCESS && hret != HG_TIMEOUT)
            msg_abort("nexus_bgthread: HG_Progress failed");
    }

#ifdef NEXUS_DEBUG
    fprintf(stdout, "Network thread exiting\n");
#endif

    return NULL;
}

typedef struct hg_lookup_out {
    hg_return_t hret;
    hg_addr_t addr;
    pthread_mutex_t cb_mutex;
    pthread_cond_t cb_cv;
} hg_lookup_out_t;

static hg_return_t hg_lookup_cb(const struct hg_cb_info *info)
{
    hg_lookup_out_t *out = (hg_lookup_out_t *)info->arg;
    out->hret = info->ret;
    if (out->hret != HG_SUCCESS)
        out->addr = HG_ADDR_NULL;
    else
        out->addr = info->info.lookup.addr;

    pthread_mutex_lock(&out->cb_mutex);
    pthread_cond_signal(&out->cb_cv);
    pthread_mutex_unlock(&out->cb_mutex);

    return HG_SUCCESS;
}

static hg_return_t hg_lookup(nexus_ctx_t *nctx, hg_context_t *hgctx,
                             char *hgaddr, hg_addr_t *addr)
{
    hg_lookup_out_t *out = NULL;
    hg_return_t hret;

    /* Init addr metadata */
    out = (hg_lookup_out_t *)malloc(sizeof(*out));
    if (out == NULL)
        return HG_NOMEM_ERROR;

    /* rank is set, perform lookup */
    pthread_mutex_init(&out->cb_mutex, NULL);
    pthread_cond_init(&out->cb_cv, NULL);
    pthread_mutex_lock(&out->cb_mutex);
    hret = HG_Addr_lookup(hgctx, &hg_lookup_cb, out, hgaddr, HG_OP_ID_IGNORE);
    if (hret != HG_SUCCESS)
        goto err;

    /* Lookup posted, wait until finished */
    pthread_cond_wait(&out->cb_cv, &out->cb_mutex);
    pthread_mutex_unlock(&out->cb_mutex);

    if (out->hret != HG_SUCCESS) {
        hret = out->hret;
    } else {
        hret = HG_SUCCESS;
        *addr = out->addr;
    }

err:
    pthread_cond_destroy(&out->cb_cv);
    pthread_mutex_destroy(&out->cb_mutex);
    free(out);
    return hret;
}

typedef struct {
    int pid;
    int hgid;
    int grank;
    int lrank;
} ldata_t;

static void discover_local_info(nexus_ctx_t *nctx)
{
    int ret;
    char hgaddr[128];
    ldata_t ldat;
    ldata_t *hginfo;
    hg_return_t hret;
    pthread_t bgthread; /* network background thread */
    bgthread_dat_t *bgarg;

    MPI_Comm_rank(nctx->localcomm, &(nctx->lrank));
    MPI_Comm_size(nctx->localcomm, &(nctx->lsize));

    /* Initialize local Mercury listening endpoints */
    snprintf(hgaddr, sizeof(hgaddr), "na+sm://%d/0", getpid());
#ifdef NEXUS_DEBUG
    fprintf(stderr, "Initializing for %s\n", hgaddr);
#endif

    nctx->local_hgcl = HG_Init(hgaddr, HG_TRUE);
    if (!nctx->local_hgcl)
        msg_abort("HG_init failed for local endpoint");

    nctx->local_hgctx = HG_Context_create(nctx->local_hgcl);
    if (!nctx->local_hgctx)
        msg_abort("HG_Context_create failed for local endpoint");

    /* Start the network thread */
    bgarg = (bgthread_dat_t *)malloc(sizeof(*bgarg));
    if (!bgarg)
        msg_abort("malloc failed");

    bgarg->hgctx = nctx->local_hgctx;
    bgarg->bgdone = 0;

    ret = pthread_create(&bgthread, NULL, nexus_bgthread, (void*)bgarg);
    if (ret != 0)
        msg_abort("pthread_create failed");

    /* Exchange PID, ID, global rank, local rank among local ranks */
    ldat.pid = getpid();
    ldat.hgid = 0;
    ldat.grank = nctx->grank;
    ldat.lrank = nctx->lrank;

    hginfo = (ldata_t *)malloc(sizeof(ldata_t) * (nctx->lsize));
    if (!hginfo)
        msg_abort("malloc failed");

    MPI_Allgather(&ldat, sizeof(ldata_t), MPI_BYTE, hginfo,
                  sizeof(ldata_t), MPI_BYTE, nctx->localcomm);

    /* Build local => global rank map */
    nctx->localranks = (int *)malloc(sizeof(int) * (nctx->lsize));
    if (!nctx->localranks)
        msg_abort("malloc failed");

    for (int i = 0; i < nctx->lsize; i++) {
        int eff_i = (nctx->lrank + i) % nctx->lsize;
        hg_addr_t localaddr;

        /* Find the local root */
        if (hginfo[eff_i].lrank == 0)
            nctx->lroot = hginfo[eff_i].grank;

        /* Update mapping */
        nctx->localranks[hginfo[eff_i].lrank] = hginfo[eff_i].grank;

#ifdef NEXUS_DEBUG
        fprintf(stdout, "[%d] Idx %d: pid %d, id %d, grank %d, lrank %d\n",
                nctx->grank, eff_i, hginfo[eff_i].pid, hginfo[eff_i].hgid,
                hginfo[eff_i].grank, hginfo[eff_i].lrank);
#endif

        snprintf(hgaddr, sizeof(hgaddr), "na+sm://%d/%d",
                 hginfo[eff_i].pid, hginfo[eff_i].hgid);

        if (hginfo[eff_i].grank == nctx->grank) {
            hret = HG_Addr_self(nctx->local_hgcl, &localaddr);
        } else {
            hret = hg_lookup(nctx, nctx->local_hgctx, hgaddr, &localaddr);
        }

        if (hret != HG_SUCCESS) {
            fprintf(stderr, "Tried to lookup %s\n", hgaddr);
            msg_abort("hg_lookup failed");
        }

        /* Add to local map */
        nctx->laddrs[hginfo[eff_i].grank] = localaddr;
#ifdef NEXUS_DEBUG
        print_hg_addr(nctx->local_hgcl, hgaddr, localaddr);
#endif
    }

    free(hginfo);

    /* Sync before terminating background threads */
    MPI_Barrier(nctx->localcomm);

    /* Terminate network thread */
    bgarg->bgdone = 1;
    pthread_join(bgthread, NULL);

    free(bgarg);
}

nexus_ret_t nexus_bootstrap(nexus_ctx_t *nctx, int minport, int maxport,
                            char *subnet, char *proto)
{
    char hgaddr[128];

    /* Grab MPI rank info */
    MPI_Comm_rank(MPI_COMM_WORLD, &(nctx->grank));
    MPI_Comm_size(MPI_COMM_WORLD, &(nctx->gsize));

    if (!nctx->grank)
        fprintf(stdout, "Nexus: started bootstrap\n");

    init_local_comm(nctx);
    discover_local_info(nctx);

    if (!nctx->grank)
        fprintf(stdout, "Nexus: done local info discovery\n");

#ifdef NEXUS_DEBUG
    fprintf(stdout, "[%d] grank = %d, lrank = %d, gsize = %d, lsize = %d\n",
            nctx->grank, nctx->grank, nctx->lrank, nctx->gsize, nctx->lsize);
#endif /* NEXUS_DEBUG */

    return NX_SUCCESS;
}

nexus_ret_t nexus_destroy(nexus_ctx_t *nctx)
{
    std::map<int, hg_addr_t>::iterator it;

    /* Free local Mercury addresses */
    for (it = nctx->laddrs.begin(); it != nctx->laddrs.end(); it++)
        if (it->second != HG_ADDR_NULL)
            HG_Addr_free(nctx->local_hgcl, it->second);

    /* Sync before tearing down local endpoints */
    MPI_Barrier(nctx->localcomm);
    MPI_Comm_free(&nctx->localcomm);

    /* Destroy Mercury local endpoints */
    HG_Context_destroy(nctx->local_hgctx);
    HG_Finalize(nctx->local_hgcl);

    if (!nctx->grank)
        fprintf(stdout, "Nexus: done local info cleanup\n");

    free(nctx->localranks);
    return NX_SUCCESS;
}
