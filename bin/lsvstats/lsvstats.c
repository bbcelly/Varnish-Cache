/*-
 * Copyright (c) 2015 Livesport s.r.o [http://www.livesport.eu]
 * All rights reserved.
 *
 * Author: Richard Kuchar <richard.kuchar@livesport.eu>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Log collectr for Livesport Varnish Stats
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <regex.h>
#include <math.h>
#include <limits.h>

#include "compat/daemon.h"

#include "vsb.h"
#include "vpf.h"
#include "libvarnish.h"
#include "vsl.h"
#include "varnishapi.h"

#include "lsvstats.h"

/* Globals -------------------------------------------------------------------*/
static struct VSM_data *vd;

static volatile sig_atomic_t showtime;
static volatile sig_atomic_t sigexit;

int         a_flag = 0;
const char    *w_arg = NULL;

regexp        *conf;
unsigned    *conft[CONFTN];

vsl    ob[SOCKETS_MAX];
vsd    *lsvs_d_hit;
vsd    *lsvs_d_miss;

static uint64_t    bitmap[SOCKETS_MAX];

/* Data Output Handler -------------------------------------------------------*/
static FILE *
log_open()
{
    FILE *of;

    if (w_arg) {
        if ((of = fopen(w_arg, a_flag ? "a" : "w")) == NULL) {
            fprintf(stderr, "Error: Couldn't open output file!\n");
            perror(w_arg);
            exit(1);
        }
    } else {
        of = stdout;
    }

    return (of);
}

/* Config --------------------------------------------------------------------*/

// strip first n character from string
static char*
strip(char *s, int n)
{
    char *os = (char *)malloc((strlen(s)-n+1)*sizeof(char));
    int i;
    int c=0;

    for (i=0 ; *s; s++) {
        if (i >= n) {
            os[c++] = *s;
        }
        i++;
    }

    os[c] = '\0';

    return(os);

}

static FILE*
config_open(const char *f_arg)
{
    FILE* file;

    file = fopen(f_arg, "r");
    if (!file) {
        fprintf(stderr, "Error: Couldn't open config file!\n");
        perror(f_arg);
        exit(1);
    }
    return (file);
}

static unsigned config_size() {
    unsigned i;
    unsigned n=0;

    for (i=0; i < CONFTN; i++) {
        n+=(conft[i][0]-1);
    }
    return (n);
}

static void
config_read(const char *f_arg)
{
    FILE    *file;
    int     linen = 0;
    int     ret = 0;
    int        i,j, confs;
    char    *line = NULL;
    char     *key;
    char    *reg;
    char    *delim;
    char    msgbuf[100];
    size_t  len = 0;
    ssize_t cn;
    regex_t reconf;
    regex_t recomm;

    file = config_open(f_arg);
    XXXAN(file >= 0);

    /* Compile regular expression for config test*/
    if (regcomp(&reconf, "^[[:space:]]*[[:alnum:]_-]+[[:space:]]+[:][[:space:]]+.*$", REG_EXTENDED | REG_ICASE | REG_NOSUB)
        || regcomp(&recomm, "^[[:space:]]*[#;].*$", REG_EXTENDED | REG_ICASE | REG_NOSUB)
       ) {
        fprintf(stderr, "Internal Error - Regexp compilation failed!\n");
        cleanup(SIGKILL);
    }

    confs = 0;
    delim = (char *)malloc(10*sizeof(char));

    while ((cn = getline(&line, &len, file)) != -1) {
        linen++;
    }
    rewind(file);

    conf=(regexp *)malloc(sizeof(regexp)*linen);
    for (i=0; i < CONFTN; i++) {
        conft[i]=(unsigned *)malloc((linen + 1) * sizeof(unsigned));
        conft[i][0]=1;
    }

    linen=0;

    while ((cn = getline(&line, &len, file)) != -1) {
        linen++;
        key = (char *)malloc((strlen(line)+1)*sizeof(char));
        reg = (char *)malloc((strlen(line)+1)*sizeof(char));

        if ((regexec(&reconf, line, 0, NULL, 0) == 0)
            && (regexec(&recomm, line, 0, NULL, 0) != 0)
           ) {
            if (sscanf(line, "%s : %s", key, reg) == 2) {

                ret = regcomp(&(conf[confs].reg), reg, REG_EXTENDED | REG_ICASE | REG_NOSUB);
                if (ret != 0) {
                    regerror(ret, &(conf[confs].reg), msgbuf, sizeof(msgbuf));
                    fprintf(stderr, "Warning [%i]: Regex compilation failed: %s\n", linen, msgbuf);
                    regfree(&(conf[confs].reg));
                } else {
                    /* which type of key we have */
                    conf[confs].key = (char **)malloc(sizeof(char *));
                    if (strncmp(key, "x_", 2) == 0) {
                        i=CONFT_ALL;
                        *(conf[confs].key) = strip(key, 2);
                    } else if (strncmp(key, "xx_", 3) == 0) {
                        i=CONFT_HEAD;
                        *(conf[confs].key) = strip(key, 3);
                    } else if (strncmp(key, "xm_", 3) == 0) {
                        i=CONFT_MOB;
                        *(conf[confs].key) = strip(key, 3);
                    } else if (strncmp(key, "error_", 6) == 0) {
                        i=CONFT_ERR;
                        *(conf[confs].key) = strdup(key);
                    } else {
                        i=CONFT_ONE;
                        *(conf[confs].key) = strdup(key);
                    }

                    /* get index of free position in array */
                    j=conft[i][0];
                    /* save the index of array to latest free position */
                    conft[i][j]=confs;
                    /* increment the index */
                    conft[i][0]++;

                    confs++;
                }
            } else {
                fprintf(stderr, "Error [%i]: Parser error!\n", linen);
                cleanup(SIGKILL);
            }
        } else {
            fprintf(stderr, "Warning [%i]: Syntax error!\n", linen);
        }

        free(key);
        key=NULL;
        free(reg);
        reg=NULL;

    }

    free(delim);
    free(line);
    regfree(&reconf);
    regfree(&recomm);

    AZ(fclose(file));
}

static void
config_cleanup()
{
    int i;
    unsigned confs=config_size();

    for (i=0; i < confs ; i++) {
        free(*(conf[i].key));
        free(conf[i].key);
        regfree(&conf[i].reg);
    }

    free(conf);

    for (i=0; i < CONFTN; i++) {
        free(conft[i]);
    }
}

/* Data structure handling ---------------------------------------------------*/

static void
lsvs_init()
{
    unsigned i;
    unsigned confs=config_size();

    lsvs_d_miss=(vsd *)malloc(confs*sizeof(vsd));

    for (i=0; i < confs; i++) {
        lsvs_d_miss[i].c=0;
        lsvs_d_miss[i].ttfb=0.0;
        lsvs_d_miss[i].ttlb=0.0;
        lsvs_d_miss[i].dm=SOCKETS_MAX;
        lsvs_d_miss[i].dttfb=(double **)malloc(sizeof(double *));
        lsvs_d_miss[i].dttlb=(double **)malloc(sizeof(double *));
        *(lsvs_d_miss[i].dttfb)=(double *)malloc(lsvs_d_miss[i].dm*sizeof(double));
        *(lsvs_d_miss[i].dttlb)=(double *)malloc(lsvs_d_miss[i].dm*sizeof(double));
    }

    lsvs_d_hit=(vsd *)malloc(confs*sizeof(vsd));

    for (i=0; i < confs; i++) {
        lsvs_d_hit[i].c=0;
        lsvs_d_hit[i].ttfb=0.0;
        lsvs_d_hit[i].ttlb=0.0;
        lsvs_d_hit[i].dm=SOCKETS_MAX;
        lsvs_d_hit[i].dttfb=(double **)malloc(sizeof(double *));
        lsvs_d_hit[i].dttlb=(double **)malloc(sizeof(double *));
        *(lsvs_d_hit[i].dttfb)=(double *)malloc(lsvs_d_hit[i].dm*sizeof(double));
        *(lsvs_d_hit[i].dttlb)=(double *)malloc(lsvs_d_hit[i].dm*sizeof(double));
    }
}

static void
lsvs_clear()
{
    unsigned i;
    unsigned confs=config_size();

    for (i=0; i < confs; i++) {
        lsvs_d_miss[i].c=0;
        lsvs_d_miss[i].ttfb=0.0;
        lsvs_d_miss[i].ttlb=0.0;

        lsvs_d_hit[i].c=0;
        lsvs_d_hit[i].ttfb=0.0;
        lsvs_d_hit[i].ttlb=0.0;
    }
}

static void
lsvs_cleanup()
{
    unsigned i;
    unsigned confs=config_size();

    for (i=0; i < confs; i++) {
        free(*(lsvs_d_miss[i].dttfb));
        free(*(lsvs_d_miss[i].dttlb));
        free(lsvs_d_miss[i].dttfb);
        free(lsvs_d_miss[i].dttlb);
    }
    free(lsvs_d_miss);

    for (i=0; i < confs; i++) {
        free(*(lsvs_d_hit[i].dttfb));
        free(*(lsvs_d_hit[i].dttlb));
        free(lsvs_d_hit[i].dttfb);
        free(lsvs_d_hit[i].dttlb);
    }

    free(lsvs_d_hit);
}

static void
lsvs_add(enum vcachestatus handl, unsigned i, double ttfb, double ttlb)
{
    double *dttfb;
    double *dttlb;

    switch (handl) {
        case hit:
            lsvs_d_hit[i].ttfb+=(long double) ttfb;
            lsvs_d_hit[i].ttlb+=(long double) ttlb;

            *(*(lsvs_d_hit[i].dttfb)+lsvs_d_hit[i].c)=ttfb;
            *(*(lsvs_d_hit[i].dttlb)+lsvs_d_hit[i].c)=ttlb;

            lsvs_d_hit[i].c++;

            if (lsvs_d_hit[i].c < ULONG_MAX) {
                if (lsvs_d_hit[i].c >= lsvs_d_hit[i].dm) {
                    if ((lsvs_d_hit[i].dm*2) < ULONG_MAX) {
                        lsvs_d_hit[i].dm=lsvs_d_hit[i].dm*2;
                    } else if (lsvs_d_hit[i].dm == ULONG_MAX) {
                        return;
                    } else {
                        lsvs_d_hit[i].dm=ULONG_MAX;
                    }

                    if(!(dttfb=(double *)realloc(*(lsvs_d_hit[i].dttfb), (sizeof(double)*lsvs_d_hit[i].dm)))) {
                        raise(SIGINT);
                    } else {
                        *(lsvs_d_hit[i].dttfb)=dttfb;
                        dttfb=NULL;
                    }
                    if(!(dttlb=(double *)realloc(*(lsvs_d_hit[i].dttlb), (sizeof(double)*lsvs_d_hit[i].dm)))) {
                        raise(SIGINT);
                    } else {
                        *(lsvs_d_hit[i].dttlb)=dttlb;
                        dttlb=NULL;
                    }
                }
            } else {
                showtime=1;
            }
            break;
        case miss:
        case pass:
            lsvs_d_miss[i].ttfb+=(long double) ttfb;
            lsvs_d_miss[i].ttlb+=(long double) ttlb;

            *(*(lsvs_d_miss[i].dttfb)+lsvs_d_miss[i].c)=ttfb;
            *(*(lsvs_d_miss[i].dttlb)+lsvs_d_miss[i].c)=ttlb;

            lsvs_d_miss[i].c++;

            if (lsvs_d_miss[i].c < ULONG_MAX) {
                if (lsvs_d_miss[i].c >= lsvs_d_miss[i].dm) {
                    if ((lsvs_d_miss[i].dm*2) < ULONG_MAX) {
                        lsvs_d_miss[i].dm=lsvs_d_miss[i].dm*2;
                    } else if (lsvs_d_miss[i].dm == ULONG_MAX) {
                        return;
                    } else {
                        lsvs_d_miss[i].dm=ULONG_MAX;
                    }

                    if(!(dttfb=(double *)realloc(*(lsvs_d_miss[i].dttfb), (sizeof(double)*lsvs_d_miss[i].dm)))) {
                        raise(SIGINT);
                    } else {
                        *(lsvs_d_miss[i].dttfb)=dttfb;
                        dttfb=NULL;
                    }
                    if(!(dttlb=(double *)realloc(*(lsvs_d_miss[i].dttlb), (sizeof(double)*lsvs_d_miss[i].dm)))) {
                        raise(SIGINT);
                    } else {
                        *(lsvs_d_miss[i].dttlb)=dttlb;
                        dttlb=NULL;
                    }
                }
            } else {
                showtime=1;
            }
            break;
    }
}

static int
lsvs_qsort_cmp (const void * a, const void * b)
{
    if (*(const double*)a > *(const double*)b) {
        return(-1);
    } else if (*(const double*)a < *(const double*)b) {
        return(1);
    } else {
        return(0);
    }
}

static void
lsvs_compute()
{
    unsigned long int i,j,jm;
    long double wttfb,wttlb;
    unsigned confs=config_size();

    FILE *f = log_open();

    for (i = 0; i < confs; i++) {

        if (lsvs_d_miss[i].c > 0) {
            qsort(*(lsvs_d_miss[i].dttfb), lsvs_d_miss[i].c, sizeof(double), lsvs_qsort_cmp);
            qsort(*(lsvs_d_miss[i].dttlb), lsvs_d_miss[i].c, sizeof(double), lsvs_qsort_cmp);

            wttfb=0.0;
            wttlb=0.0;
            jm=(unsigned long int)lround((double)lsvs_d_miss[i].c*0.1);

            if (jm > 0) {
                for (j = 0; j < jm; j++) {
                    wttfb= wttfb + *(*(lsvs_d_miss[i].dttfb)+j);
                    wttlb= wttlb + *(*(lsvs_d_miss[i].dttlb)+j);
                }

                wttfb=wttfb/jm;
                wttlb=wttlb/jm;
            }
            fprintf(f, "%s ", *(conf[i].key));
            fprintf(f, "count_miss:%lu ", lsvs_d_miss[i].c);
            fprintf(f, "avarage_miss:%lli ",llroundl((lsvs_d_miss[i].ttfb*1000)/lsvs_d_miss[i].c));
            fprintf(f, "10wa_miss:%lli ",llroundl(wttfb*1000));
        } else {
            fprintf(f, "%s count_miss:%lu avarage_miss:0 10wa_miss:0 ", *(conf[i].key), lsvs_d_miss[i].c);
        }

        if (lsvs_d_hit[i].c > 0) {
            qsort(*(lsvs_d_hit[i].dttfb), lsvs_d_hit[i].c, sizeof(double), lsvs_qsort_cmp);
            qsort(*(lsvs_d_hit[i].dttlb), lsvs_d_hit[i].c, sizeof(double), lsvs_qsort_cmp);

            wttfb=0.0;
            wttlb=0.0;
            jm=(unsigned long int)lround((double)lsvs_d_hit[i].c*0.1);

            if (jm > 0) {
                for (j = 0; j < jm; j++) {
                    wttfb= wttfb + *(*(lsvs_d_hit[i].dttfb)+j);
                    wttlb= wttlb + *(*(lsvs_d_hit[i].dttlb)+j);
                }

                wttfb=wttfb/jm;
                wttlb=wttlb/jm;
            }
            fprintf(f, "count_hit:%lu ", lsvs_d_hit[i].c);
            fprintf(f, "avarage_hit:%lli ", llroundl((lsvs_d_hit[i].ttfb*1000)/lsvs_d_hit[i].c));
            fprintf(f, "10wa_hit:%lli\n", llroundl(wttfb*1000));
        } else {
            fprintf(f, "count_hit:%lu avarage_hit:0 10wa_hit:0\n", lsvs_d_hit[i].c);
        }
    }
    fflush(f);
    if (f != stdout) {
        AZ(fclose(f));
    }
}

/* Collecting & Analyzing ----------------------------------------------------*/

/*
 * Returns a copy of the entire string with leading and trailing spaces
 * trimmed.
 */
static char *
trimline(const char *str, const char *end)
{
    size_t len;
    char *p;

    /* skip leading space */
    while (str < end && *str && *str == ' ')
        ++str;

    /* seek to end of string */
    for (len = 0; &str[len] < end && str[len]; ++len)
         /* nothing */ ;

    /* trim trailing space */
    while (len && str[len - 1] == ' ')
        --len;

    /* copy and return */
    p = malloc(len + 1);
    assert(p != NULL);
    memcpy(p, str, len);
    p[len] = '\0';
    return (p);
}

//get status of vsl object
static bool
vsl_status(vsl *ptr)
{
    if ((ptr->error == true)
        || (ptr->status== 0)
        || (*(ptr->url) == NULL)
        || (ptr->ttfb != ptr->ttfb)
        || (ptr->ttlb != ptr->ttlb)
        || (ptr->req == 0)
        || (ptr->handling == 0)
        || (ptr->mobile == 0)) {
        return(false);
    } else {
        return(true);
    }
}

//init vsl object
static void
vsl_init(vsl *ptr)
{
    ptr->error=false;
    ptr->status=0;
    ptr->sstatus=(char **)malloc(sizeof(char *));
    *(ptr->sstatus)=NULL;
    ptr->url=(char **)malloc(sizeof(char *));
    *(ptr->url)=NULL;
    ptr->ttfb=0.0;
    ptr->ttlb=0.0;
    ptr->req=0;
    ptr->sreq=(char **)malloc(sizeof(char *));
    *(ptr->sreq)=NULL;
    ptr->handling=0;
    ptr->mobile=1;
}

//clear vsl object
static void
vsl_cleanup(vsl *ptr)
{
    if (*(ptr->sstatus) != NULL) {
        free(*(ptr->sstatus));
    }
    free(ptr->sstatus);
    if (*(ptr->url) != NULL) {
        free(*(ptr->url));
    }
    free(ptr->url);
    if (*(ptr->sreq) != NULL) {
        free(*(ptr->sreq));
    }
    free(ptr->sreq);
    ptr->ttfb=0.0;
    ptr->ttlb=0.0;
}

//clear vsl object
static void
vsl_clear(vsl *ptr)
{
    vsl_cleanup(ptr);
    vsl_init(ptr);
}

//init collect buffers
static void
collect_init()
{
    unsigned i;

    for (i = 0; i < SOCKETS_MAX; i++) {
        vsl_init(&(ob[i]));
    }
}

//analyze collected data
static void
collect_analyze(int fd)
{
    unsigned i;
    unsigned idx=0;

    if (vsl_status(&(ob[fd])) && VSL_Matched(vd, bitmap[fd])) {
        if ((ob[fd].status > 399) && (ob[fd].status < 600 ) && (ob[fd].status != 501)) {
            /* Error part */
            for (i=1; i < conft[CONFT_ERR][0]; i++) {
                idx=conft[CONFT_ERR][i];
                if (regexec(&(conf[idx].reg), *(ob[fd].sstatus), 0, NULL, 0) == 0) {
                    lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
                    break;
                }
            }
        } else if ((ob[fd].req == GET) || (ob[fd].req == POST) || (ob[fd].req == HEAD)) {
            for (i=1; i < conft[CONFT_ALL][0]; i++) {
                /* All part */
                idx=conft[CONFT_ALL][i];
                if (regexec(&(conf[idx].reg), *(ob[fd].url), 0, NULL, 0) == 0) {
                    lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
                }
            }

            for (i=1; i < conft[CONFT_ONE][0]; i++) {
                /* Only first occurence */
                idx=conft[CONFT_ONE][i];
                if (regexec(&(conf[idx].reg), *(ob[fd].url), 0, NULL, 0) == 0) {
                    lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
                    break;
                }
            }
        }

        /* Bye Headers */
        for (i=1; i < conft[CONFT_HEAD][0]; i++) {
            /* Do it for All keys */
            idx=conft[CONFT_HEAD][i];
            if (regexec(&(conf[idx].reg), *(ob[fd].sreq), 0, NULL, 0) == 0) {
                lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
            }
        }

        /* Count_mobile */
        for (i=1; i < conft[CONFT_MOB][0]; i++) {
            idx=conft[CONFT_MOB][i];
            /* Is there something to analyze? */
            if (ob[fd].mobile == non) {
                break;
                /* Do it for All keys */
            } else if ((strcmp(*(conf[idx].key),"ios") == 0) && (ob[fd].mobile == ios)) {
                lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
            } else if ((strcmp(*(conf[idx].key),"android") == 0) && (ob[fd].mobile == android)) {
                lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
            } else if ((strcmp(*(conf[idx].key),"mobiles") == 0)) {
                lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
            }
        }
    }
    bitmap[fd] = 0;
    vsl_clear(&(ob[fd]));
}

//cleanup data objects at the end
static void
collect_cleanup()
{
    unsigned i;

    for (i = 0; i < SOCKETS_MAX; i++) {
        vsl_cleanup(&(ob[i]));
        bitmap[i] = 0;
    }
}

static int
collect(void *priv, enum VSL_tag_e tag, unsigned fd, unsigned len,
        unsigned spec, const char *ptr, uint64_t bm)
{
    const char *end, *split;
    char *key;
    char *value;

    (void)priv;
    /* SIGINT was raised so we need to exit */
    if (sigexit > 0) {
        return (-1);
    }

    /* SIGUSR1 was raised so we need to output data */
    if (showtime > 0) {
        lsvs_compute();
        lsvs_clear();
        showtime=0;
    }

    end = ptr + len;

    /* Just ignore any fd not inside the bitmap */
    if (fd >= sizeof bitmap / sizeof bitmap[0])
        return (0);

    bitmap[fd] |= bm;

    if (!(spec & (VSL_S_CLIENT|VSL_S_BACKEND))) {
        return (0);
    }
    switch (tag) {
        case SLT_VCL_call:
            if (strncmp(ptr, "hit", len) == 0) {
                ob[fd].handling = hit;
            } else if (strncmp(ptr, "miss", len) == 0) {
                ob[fd].handling = miss;
            } else if (strncmp(ptr, "pass", len) == 0) {
                ob[fd].handling = pass;
            } //nothing else matters
            break;
        case SLT_TxRequest:
        case SLT_RxRequest:
            if (strncmp(ptr, "GET", len) == 0) {
                ob[fd].req = GET;
            } else if (strncmp(ptr, "POST", len) == 0) {
                ob[fd].req = POST;
            } else if (strncmp(ptr, "HEAD", len) == 0) {
                ob[fd].req = HEAD;
            } else {
                ob[fd].req = INVALID;
            }

            if (*(ob[fd].sreq) != NULL) {
                free(*(ob[fd].sreq));
                *(ob[fd].sreq)=NULL;
            }
            *(ob[fd].sreq) = strndup(ptr, len);
            break;

        case SLT_TxHeader:
        case SLT_RxHeader:
            split = strchr(ptr, ':');
            if (split == NULL)
                break;

            AN(split);
            key = trimline(ptr, split);
            value = trimline(split+1, end);
            //if key = X-Platform
            for (int i = 0; i < strlen(key); i++) {
                key[i] = tolower(key[i]);
            }
            if (strcmp(key,"x-platform") == 0) {
                //if value == ios || android
                for (int i = 0; i < strlen(value); i++) {
                    value[i] = tolower(value[i]);
                }
                if (strcmp(value,"ios") == 0) {
                    ob[fd].mobile = ios;
                } else {
                    ob[fd].mobile = android;
                }
            }
            break;
        case SLT_TxURL:
        case SLT_RxURL:
            if (*(ob[fd].url) != NULL) {
                free(*(ob[fd].url));
                *(ob[fd].url)=NULL;
            }
            *(ob[fd].url) = trimline(ptr, end);
            break;
        case SLT_RxStatus:
        case SLT_TxStatus:
            ob[fd].status = 0;

            if (*(ob[fd].sstatus) != NULL) {
                free(*(ob[fd].sstatus));
                *(ob[fd].sstatus)=NULL;
            }
            *(ob[fd].sstatus) = trimline(ptr, end);

            if (sscanf(*(ob[fd].sstatus), "%i", &(ob[fd].status)) != 1) {
                ob[fd].status = 0;
                ob[fd].error=true;
            }
            break;
        case SLT_ReqEnd:
            if (sscanf(ptr, "%*u %*u.%*u %*u.%*u %*u.%*u %lf %lf", &(ob[fd].ttfb), &(ob[fd].ttlb)) != 2) {
                ob[fd].ttfb = 0.0;
                ob[fd].ttlb = 0.0;
                ob[fd].error=true;
                errno=0;
            }
            if ((ob[fd].ttfb != ob[fd].ttfb) || (ob[fd].ttlb != ob[fd].ttlb)) {
                ob[fd].error=true;
            }
        case SLT_BackendClose:
        case SLT_BackendReuse:
            collect_analyze(fd);
            break;
        default:
            break;
    }
    free(key);
    key=NULL;
    free(value);
    value=NULL;
    free(split);
    split=NULL;

    return (0);
}

/*--------------------------------------------------------------------*/

static void
usage(void)
{
    fprintf(stderr, "usage: lsvstats -f config_file [-D] [-n varnish_name] "
                    "[-w file] [-P file]\n");
    exit(1);
}

int
main(int argc, char * const *argv)
{
    int c, i;
    int D_flag = 0;
    const char *P_arg = NULL;
    const char *f_arg = NULL;
    struct vpf_fh *pfh = NULL;

    signal(SIGABRT, sigterm);
    signal(SIGTERM, sigterm);
    signal(SIGINT, sigterm);
    signal(SIGUSR1, output);

    vd = VSM_New();
    VSL_Setup(vd);

    while ((c = getopt(argc, argv, VSL_ARGS "DP:f:w:")) != -1) {
        switch (c) {
            case 'a':
                a_flag = 1;
                break;
            case 'b':
                fprintf(stderr, "-b is not valid for lsvstats\n");
                 exit(1);
                 break;
            case 'c':
                 /* XXX: Silently ignored: it's required anyway */
                 break;
            case 'D':
                D_flag = 1;
                break;
            case 'P':
                P_arg = optarg;
                break;
            case 'f':
                f_arg = optarg;
                break;
            case 'w':
                w_arg = optarg;
                break;

            /* FALLTHROUGH */
            default:
                if (VSL_Arg(vd, c, optarg) > 0)
                    break;
                usage();
        }
    }

    /* not implemented options */
    if ((argc - optind) > 0)
        usage();

    VSL_Arg(vd, 'c', optarg);

    /* VarnishShareLog open */
    if (VSL_Open(vd, 1))
        exit(1);

    /* read config file */
    if (f_arg) {
        config_read(f_arg);
    } else {
        fprintf(stderr, "Error: Config file is not specified!\n");
        perror(f_arg);
        exit(1);
    }

    /* specify pidfile */
    if (P_arg && (pfh = VPF_Open(P_arg, 0644, NULL)) == NULL) {
        fprintf(stderr, "Error: Couldn't create PID file!\n");
        perror(P_arg);
        exit(1);
    }

    /* run as daemon? */
    if (D_flag && varnish_daemon(0, 0) == -1) {
        fprintf(stderr, "Error: Daemonization doesn't work!\n");
        perror("daemon()");
        if (pfh != NULL)
            VPF_Remove(pfh);
        exit(1);
    }

    /* write pidfile */
    if (pfh != NULL)
        VPF_Write(pfh);

    /* unbuffered output */
    setbuf(stdout, NULL);

    /* init data structure */
    lsvs_init();
    collect_init();

    /* run */
    showtime=0;
    sigexit=0;
    while (!sigexit) {
        i = VSL_Dispatch(vd, collect, NULL);
        if (i == 0) {
            collect_cleanup();
            AZ(fflush(stdout));
        }
        else if (i < 0)
            break;
    }

    /* clear pidfile */
    if (pfh != NULL)
        VPF_Remove(pfh);

    cleanup(sigexit);
}

/* Signal handling  ---------------------------------------------------*/

static void
sigterm(int signal)
{
    fprintf(stderr, "\nSIGINT !\n\n");
    sigexit=signal;
}

static void
cleanup(int signal)
{
    fprintf(stderr, "\nStopped by %i!\n\n", signal);
    lsvs_compute();
    lsvs_cleanup();
    collect_cleanup();
    config_cleanup();
    VSM_Delete(vd);
    exit(0);
}

static void
output(int signal)
{
    (void)signal;
    showtime=1;
}
