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

/*
 * nx-try.cc  run deltafs-nexus init and then stop
 * 14-Jun-2017  chuck@ece.cmu.edu
 */

/*
 * this program tests deltafs-nexus.  we use MPI to managing the 
 * processes in the test.
 *
 * to use this program you need to launch it as an MPI application.
 * the launch process will determine the number of nodes allocated and
 * the number of processes per node.  nexus uses MPI_Comm_split_type
 * to determine the node-level configuration.  thus nx-try
 * itself does not have any topology configuration command line flags,
 * it uses whatever it gets from the MPI launcher.
 *
 * usage: nx-try [options] mercury-protocol subnet
 *
 * options:
 *  -p baseport  base port number
 *  -t secs      timeout (alarm)
 *
 * examples:
 *
 *   mpirun -n 3 ./nx-try -c 1 bmi+tcp 10
 */

#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>

#include <mercury.h>
#include <mercury_macros.h>

#include <mpi.h>   /* XXX: nexus requires this */

#include <deltafs-nexus/deltafs-nexus_api.h>

/*
 * helper/utility functions, included inline here so we are self-contained
 * in one single source file...
 */
char *argv0;                     /* argv[0], program name */
int myrank = 0;

/*
 * vcomplain/complain about something.  if ret is non-zero we exit(ret)
 * after complaining.  if r0only is set, we only print if myrank == 0.
 */
void vcomplain(int ret, int r0only, const char *format, va_list ap) {
    if (!r0only || myrank == 0) {
        fprintf(stderr, "%s: ", argv0);
        vfprintf(stderr, format, ap);
        fprintf(stderr, "\n");
    }
    if (ret) {
        MPI_Finalize();
        exit(ret);
    }
}

void complain(int ret, int r0only, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vcomplain(ret, r0only, format, ap);
    va_end(ap);
}

/*
 * end of helper/utility functions.
 */

/*
 * default values for port and count
 */
#define DEF_BASEPORT 19900 /* starting TCP port we listen on (instance 0) */
#define DEF_TIMEOUT 120    /* alarm timeout */

/*
 * gs: shared global data (e.g. from the command line)
 */
struct gs {
    int ninst;               /* currently locked at 1 */
    /* note: MPI rank stored in global "myrank" */
    int size;                /* world size (from MPI) */
    char *hgproto;           /* hg protocol to use */
    char *hgsubnet;          /* subnet to use (XXX: assumes IP) */
    int baseport;            /* base port number */
    int timeout;             /* alarm timeout */
} g;

/*
 * is: per-instance state structure.   currently we only allow
 * one instance per proc (but we keep this broken out in case
 * we want to change it...).
 */
struct is {
    int n;                   /* our instance number (0 .. n-1) */
    nexus_ctx_t *nxp;        /* nexus context */
};
struct is *isa;    /* an array of state */

/*
 * alarm signal handler
 */
void sigalarm(int foo) {
    int lcv;
    fprintf(stderr, "SIGALRM detected (%d)\n", myrank);
    for (lcv = 0 ; lcv < g.ninst ; lcv++) {
        fprintf(stderr, "%d: %d: @alarm: ", myrank, lcv);
    }
    fprintf(stderr, "Alarm clock\n");
    MPI_Finalize();
    exit(1);
}

/*
 * usage
 */
static void usage(const char *msg) {

    /* only have rank 0 print usage error message */
    if (myrank) goto skip_prints;

    if (msg) fprintf(stderr, "%s: %s\n", argv0, msg);
    fprintf(stderr, "usage: %s [options] mercury-protocol subnet\n", argv0);
    fprintf(stderr, "\noptions:\n");
    fprintf(stderr, "\t-p port     base port number\n");
    fprintf(stderr, "\t-t sec      timeout (alarm), in seconds\n");

skip_prints:
    MPI_Finalize();
    exit(1);
}

/*
 * forward prototype decls.
 */
static void *run_instance(void *arg);   /* run one instance */

/*
 * main program.  usage:
 *
 * ./nexus-runner [options] mercury-protocol subnet
 */
int main(int argc, char **argv) {
    int ch, lcv, rv;
    pthread_t *tarr;

    argv0 = argv[0];

    /* mpich says we should call this early as possible */
    if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
        fprintf(stderr, "%s: MPI_Init failed.  MPI is required.\n", argv0);
        exit(1);
    }

    /* we want lines, even if we are writing to a pipe */
    setlinebuf(stdout);

    /* setup default to zero/null, except as noted below */
    memset(&g, 0, sizeof(g));
    if (MPI_Comm_rank(MPI_COMM_WORLD, &myrank) != MPI_SUCCESS)
        complain(1, 0, "unable to get MPI rank");
    if (MPI_Comm_size(MPI_COMM_WORLD, &g.size) != MPI_SUCCESS)
        complain(1, 0, "unable to get MPI size");
    g.baseport = DEF_BASEPORT;
    g.timeout = DEF_TIMEOUT;
    while ((ch = getopt(argc, argv, "p:t:")) != -1) {
        switch (ch) {
            case 'p':
                g.baseport = atoi(optarg);
                if (g.baseport < 1) usage("bad port");
                break;
            case 't':
                g.timeout = atoi(optarg);
                if (g.timeout < 0) usage("bad timeout");
                break;
            default:
                usage(NULL);
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 2)          /* hgproto and hgsubnet must be provided on cli */
      usage("bad args");
    g.ninst = 1;
    g.hgproto = argv[0];
    g.hgsubnet = argv[1];

    if (myrank == 0) {
        printf("\n%s options:\n", argv0);
        printf("\tMPI_rank   = %d\n", myrank);
        printf("\tMPI_size   = %d\n", g.size);
        printf("\thgproto    = %s\n", g.hgproto);
        printf("\thgsubnet   = %s\n", g.hgsubnet);
        printf("\tbaseport   = %d\n", g.baseport);
        printf("\ttimeout    = %d\n", g.timeout);
        printf("\n");
    }

    signal(SIGALRM, sigalarm);
    alarm(g.timeout);
    if (myrank == 0) printf("main: starting ...\n");

    tarr = (pthread_t *)malloc(g.ninst * sizeof(pthread_t));
    if (!tarr) complain(1, 0, "malloc tarr thread array failed");
    isa = (struct is *)malloc(g.ninst *sizeof(*isa));    /* array */
    if (!isa) complain(1, 0, "malloc 'isa' instance state failed");
    memset(isa, 0, g.ninst * sizeof(*isa));

    /* fork off a thread for each instance */
    for (lcv = 0 ; lcv < g.ninst ; lcv++) {
        isa[lcv].n = lcv;
        rv = pthread_create(&tarr[lcv], NULL, run_instance, (void*)&isa[lcv]);
        if (rv != 0)
            complain(1, 0, "pthread create failed %d", rv);
    }

    /* now wait for everything to finish */
    if (myrank == 0) printf("main: collecting\n");
    for (lcv = 0 ; lcv < g.ninst ; lcv++) {
        pthread_join(tarr[lcv], NULL);
    }

    if (myrank == 0) printf("main: collection done.\n");

    MPI_Barrier(MPI_COMM_WORLD);
    if (myrank == 0) printf("main exiting...\n");

    MPI_Finalize();
    exit(0);
}

/*
 * run_instance: the main routine for running one instance of mercury.
 * we pass the instance state struct in as the arg...
 */
void *run_instance(void *arg) {
    struct is *isp = (struct is *)arg;
    int n = isp->n;               /* recover n from isp */
    nexus_ret_t nrv;

    printf("%d: instance running\n", myrank);
    isa[n].n = n;    /* make it easy to map 'is' structure back to n */
    isa[n].nxp = new nexus_ctx_t;   /* XXXCDC: need ctor to run */

    /* XXXCDC: port stuff likely to go away */
    nrv = nexus_bootstrap(isp->nxp, g.baseport, g.baseport+1000 /*XXX*/,
                          g.hgsubnet, g.hgproto);
    if (nrv != NX_SUCCESS)
        complain(1, 0, "%d: nexus_bootstrap failed: %d", myrank, nrv);
    printf("%d: nexus powered up!\n", myrank);

    MPI_Barrier(MPI_COMM_WORLD);

    printf("%d: nexus powering down!\n", myrank);
    nrv = nexus_destroy(isa[n].nxp);
    if (nrv != NX_SUCCESS)
            fprintf(stderr, "nexus_destroy failed(%d)\n", nrv);
    delete isa[n].nxp;

    return(NULL);
}
