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
static volatile sig_atomic_t showtime;

int 		a_flag = 0;
const char	*w_arg = NULL;

regexp		*conf;
unsigned	*conft[CONFTN];

vslf	ob[SOCKETS_MAX];
vsdf	*lsvs_d_hit;
vsdf	*lsvs_d_miss;

static uint64_t	bitmap[SOCKETS_MAX];

/* Data Output Handler -------------------------------------------------------*/
static FILE *
log_open()
{
	FILE *of;

	if (w_arg) {
		if ((of = fopen(w_arg, a_flag ? "a" : "w")) == NULL) {
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
	FILE	*file;
	int     linen = 0;
	int 	ret = 0;
	int		i,j, confs;
	char    *line = NULL;
	char 	*key;
	char	*reg;
	char	*delim;
	char	msgbuf[100];
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
		conft[i]=(unsigned *)malloc(linen * sizeof(unsigned));
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
					if (strncmp(key, "x_", 2) == 0) {
						i=CONFT_ALL;
						conf[confs].key = strip(key, 2);
					} else if (strncmp(key, "xx_", 3) == 0) {
						i=CONFT_INV;
						conf[confs].key = strip(key, 3);
					} else if (strncmp(key, "error_", 6) == 0) {
						i=CONFT_ERR;
						conf[confs].key = strdup(key);
					} else {
						i=CONFT_ONE;
						conf[confs].key = strdup(key);
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

	lsvs_d_miss=(vsdf *)malloc(confs*sizeof(vsdf));

	for (i=0; i < confs; i++) {
		lsvs_d_miss[i].c=0;
		lsvs_d_miss[i].ttfb=0.0;
		lsvs_d_miss[i].ttlb=0.0;
		lsvs_d_miss[i].dm=SOCKETS_MAX;
		lsvs_d_miss[i].dttfb=(double *)malloc(lsvs_d_miss[i].dm*sizeof(double));
		lsvs_d_miss[i].dttlb=(double *)malloc(lsvs_d_miss[i].dm*sizeof(double));

	}

	lsvs_d_hit=(vsdf *)malloc(confs*sizeof(vsdf));

	for (i=0; i < confs; i++) {
		lsvs_d_hit[i].c=0;
		lsvs_d_hit[i].ttfb=0.0;
		lsvs_d_hit[i].ttlb=0.0;
		lsvs_d_hit[i].dm=SOCKETS_MAX;
		lsvs_d_hit[i].dttfb=(double *)malloc(lsvs_d_hit[i].dm*sizeof(double));
		lsvs_d_hit[i].dttlb=(double *)malloc(lsvs_d_hit[i].dm*sizeof(double));

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
		free(lsvs_d_miss[i].dttfb);
		free(lsvs_d_miss[i].dttlb);
	}

	free(lsvs_d_miss);

	for (i=0; i < confs; i++) {
		free(lsvs_d_hit[i].dttfb);
		free(lsvs_d_hit[i].dttlb);
	}

	free(lsvs_d_hit);
}

static void
lsvs_add(enum vcachestatus handl, unsigned i, double ttfb, double ttlb)
{
	switch (handl) {
		case hit:
			lsvs_d_hit[i].ttfb+=ttfb;
			lsvs_d_hit[i].ttlb+=ttlb;
			lsvs_d_hit[i].dttfb[lsvs_d_hit[i].c]=ttfb;
			lsvs_d_hit[i].dttlb[lsvs_d_hit[i].c]=ttlb;
			lsvs_d_hit[i].c++;

			if (lsvs_d_hit[i].c >= (ULONG_MAX -1)) {
				showtime=1;
			} else if (lsvs_d_hit[i].c >= lsvs_d_hit[i].dm) {
				if ((lsvs_d_hit[i].dm*2) > ULONG_MAX) {
					lsvs_d_hit[i].dm=ULONG_MAX;
				} else {
					lsvs_d_hit[i].dm*=2;
				}
				lsvs_d_hit[i].dttfb=(double *)realloc(lsvs_d_hit[i].dttfb, (sizeof(double)*lsvs_d_miss[i].dm));
				lsvs_d_hit[i].dttlb=(double *)realloc(lsvs_d_hit[i].dttlb, (sizeof(double)*lsvs_d_miss[i].dm));
			}
			break;
		case miss:
		case pass:
			lsvs_d_miss[i].ttfb+=ttfb;
			lsvs_d_miss[i].ttlb+=ttlb;
			lsvs_d_miss[i].dttfb[lsvs_d_miss[i].c]=ttfb;
			lsvs_d_miss[i].dttlb[lsvs_d_miss[i].c]=ttlb;
			lsvs_d_miss[i].c++;

			if (lsvs_d_miss[i].c >= ULONG_MAX) {
				showtime=1;
			} else if (lsvs_d_miss[i].c >= lsvs_d_miss[i].dm) {
				if ((lsvs_d_miss[i].dm*2) > ULONG_MAX) {
					lsvs_d_miss[i].dm=ULONG_MAX;
				} else {
					lsvs_d_miss[i].dm*=2;
				}
				lsvs_d_miss[i].dttfb=(double *)realloc(lsvs_d_miss[i].dttfb, (sizeof(double)*lsvs_d_miss[i].dm));
				lsvs_d_miss[i].dttlb=(double *)realloc(lsvs_d_miss[i].dttlb, (sizeof(double)*lsvs_d_miss[i].dm));
			}
			break;
	}
}

static int
lsvs_qsort_cmp (const void * a, const void * b)
{
	if (*(const double*)a > *(const double*)b) {
		return -1;
	} else if (*(const double*)a < *(const double*)b) {
		return 1;
	} else {
		return 0;
	}
}

static void
lsvs_compute()
{
	int	i,j,jm;
	double wttfb,wttlb;
	unsigned confs=config_size();

	FILE *f = log_open();

	for (i = 0; i < confs; i++) {

		if (lsvs_d_miss[i].c > 0) {
			qsort(lsvs_d_miss[i].dttfb, lsvs_d_miss[i].c, sizeof(double), lsvs_qsort_cmp);
			qsort(lsvs_d_miss[i].dttfb, lsvs_d_miss[i].c, sizeof(double), lsvs_qsort_cmp);

			wttfb=0.0;
			wttlb=0.0;
			jm=(int)floor((double)(lsvs_d_miss[i].c*0.1));

			if (jm > 0) {
				for (j = 0; j < jm; j++) {
					wttfb+=lsvs_d_miss[i].dttfb[j];
					wttlb+=lsvs_d_miss[i].dttlb[j];
				}

				wttfb/=(double)jm;
				wttlb/=(double)jm;
			}
			fprintf(f, "%s ", conf[i].key);
			fprintf(f, "count_miss:%lu ", lsvs_d_miss[i].c);
			fprintf(f, "avarage_miss:%i ",(int)floor(lsvs_d_miss[i].ttfb*1000/lsvs_d_miss[i].c));
			fprintf(f, "10wa_miss:%i ",(int)floor(wttfb*1000));
		} else {
			fprintf(f, "%s count_miss:%lu avarage_miss:0 10wa_miss:0 ", conf[i].key, lsvs_d_miss[i].c);
		}

		if (lsvs_d_hit[i].c > 0) {
			qsort(lsvs_d_hit[i].dttfb, lsvs_d_hit[i].c, sizeof(double), lsvs_qsort_cmp);
			qsort(lsvs_d_hit[i].dttfb, lsvs_d_hit[i].c, sizeof(double), lsvs_qsort_cmp);

			wttfb=0.0;
			wttlb=0.0;
			jm=(int)floor((double)(lsvs_d_hit[i].c*0.1));

			if (jm > 0) {
				for (j = 0; j < jm; j++) {
					wttfb+=lsvs_d_hit[i].dttfb[j];
					wttlb+=lsvs_d_hit[i].dttlb[j];
				}

				wttfb/=(double)jm;
				wttlb/=(double)jm;
			}
			fprintf(f, "count_hit:%lu ", lsvs_d_hit[i].c);
			fprintf(f, "avarage_hit:%i ", (int)floor(lsvs_d_hit[i].ttfb*1000/lsvs_d_hit[i].c));
			fprintf(f, "10wa_hit:%i\n", (int)floor(wttfb*1000));
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

//get status of vslf object
static bool
vslf_status(vslf *ptr)
{
	if (ptr->error == true
		|| ptr->status== 0
		|| ptr->url == NULL
		|| ptr->ttfb == 0.0
		|| ptr->ttlb == 0.0
		|| ptr->req == 0
		|| ptr->handling == 0) {
		return(false);
	} else {
		return(true);
	}
}

//init vslf object
static void
vslf_init(vslf *ptr)
{
	ptr->error=false;
	ptr->status=0;
	ptr->sstatus=NULL;
	ptr->url=NULL;
	ptr->ttfb=0.0;
	ptr->ttlb=0.0;
	ptr->req=0;
	ptr->sreq=NULL;
	ptr->handling=0;
}

//clear vslf object
static void
vslf_cleanup(vslf *ptr)
{
	if (ptr->sstatus != NULL) {
		free(ptr->sstatus);
	}
	if (ptr->url != NULL) {
		free(ptr->url);
	}
	if (ptr->sreq != NULL) {
		free(ptr->sreq);
	}
}

//clear vslf object
static void
vslf_clear(vslf *ptr)
{
	vslf_cleanup(ptr);
	vslf_init(ptr);
}

//init collect buffers
static void
collect_init()
{
	unsigned i;

	for (i = 0; i < SOCKETS_MAX; i++) {
		vslf_init(&(ob[i]));
	}

}

//analyze collected data
static void
collect_analyze(int fd, const struct VSM_data *vd)
{
	unsigned i;
	unsigned idx=0;

	if (vslf_status(&(ob[fd])) && VSL_Matched(vd, bitmap[fd])) {
		if ((ob[fd].status > 399) && (ob[fd].status < 600 ) && (ob[fd].status != 501)) {
			/* Error part */
			for (i=1; i < conft[CONFT_ERR][0]; i++) {
				idx=conft[CONFT_ERR][i];
				if (regexec(&(conf[idx].reg), ob[fd].sstatus, 0, NULL, 0) == 0) {
					lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
					break;
				}
			}
		} else if ((ob[fd].req != GET) && (ob[fd].req != POST) && (ob[fd].req != HEAD)) {
			/* INVALID part */
			for (i=1; i < conft[CONFT_INV][0]; i++) {
				/* All part */
				idx=conft[CONFT_INV][i];
				if (regexec(&(conf[idx].reg), ob[fd].sreq, 0, NULL, 0) == 0) {
					lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
				}
			}
		} else {
			for (i=1; i < conft[CONFT_ALL][0]; i++) {
				/* All part */
				idx=conft[CONFT_ALL][i];
				if (regexec(&(conf[idx].reg), ob[fd].url, 0, NULL, 0) == 0) {
					lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
				}
			}

			for (i=1; i < conft[CONFT_ONE][0]; i++) {
				/* Only first occurence */
				idx=conft[CONFT_ONE][i];
				if (regexec(&(conf[idx].reg), ob[fd].url, 0, NULL, 0) == 0) {
					lsvs_add(ob[fd].handling , idx, ob[fd].ttfb, ob[fd].ttlb);
					break;
				}
			}
		}
	}
	//
	bitmap[fd] = 0;
	vslf_clear(&(ob[fd]));
}

//cleanup data objects at the end
static void
collect_cleanup()
{
	unsigned i;

	for (i = 0; i < SOCKETS_MAX; i++) {
		vslf_cleanup(&(ob[i]));
		bitmap[i] = 0;
	}
}

static int
collect(void *priv, enum VSL_tag_e tag, unsigned fd, unsigned len,
        unsigned spec, const char *ptr, uint64_t bm)
{
	struct VSM_data *vd = priv;

	/* SIGUSR1 was raised so we need to output data */
	if (showtime > 0) {
		lsvs_compute();
		lsvs_clear();
		showtime=0;
	}

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
			} else {
				//do nothing, it's ok here
			}
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

			ob[fd].sreq = strndup(ptr, len);
			break;
		case SLT_TxURL:
		case SLT_RxURL:
			ob[fd].url = strndup(ptr, len);
			break;
		case SLT_RxStatus:
		case SLT_TxStatus:
			ob[fd].status = 0;
			if (sscanf(ptr, "%i", &(ob[fd].status)) != 1) {
				ob[fd].status = 0;
				ob[fd].error=true;
			} else {
				ob[fd].sstatus = strndup(ptr, len);
			}
			break;
		case SLT_ReqEnd:
			ob[fd].ttfb = 0.0;
			ob[fd].ttlb = 0.0;
			if (sscanf(ptr, "%*u %*u.%*u %*u.%*u %*u.%*u %lf %lf", &(ob[fd].ttfb), &(ob[fd].ttlb)) != 2) {
				ob[fd].ttfb = 0.0;
				ob[fd].ttlb = 0.0;
				ob[fd].error=true;
				errno=0;
			}
		case SLT_BackendClose:
		case SLT_BackendReuse:
			collect_analyze(fd, vd);
			break;
		default:
			break;
	}

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
	struct VSM_data *vd;

	signal(SIGABRT, cleanup);
	signal(SIGTERM, cleanup);
	signal(SIGINT, cleanup);
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
		perror(f_arg);
		exit(1);
	}

	/* specify pidfile */
	if (P_arg && (pfh = VPF_Open(P_arg, 0644, NULL)) == NULL) {
		perror(P_arg);
		exit(1);
	}

	/* run as daemon? */
	if (D_flag && varnish_daemon(0, 0) == -1) {
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
	while (1) {
		i = VSL_Dispatch(vd, collect, vd);
		if (i == 0) {
			collect_cleanup();
			AZ(fflush(stdout));
		}
		else if (i < 0)
			break;
	}
	collect_cleanup();

	/* clear pidfile */
	if (pfh != NULL)
		VPF_Remove(pfh);

	exit(0);
}

/* Signal handling  ---------------------------------------------------*/

static void
cleanup(int signal)
{
	fprintf(stderr, "\nStopped by %i!\n\n", signal);

	lsvs_cleanup();
	collect_cleanup();
	config_cleanup();
	exit(1);
}

static void
output(int signal)
{
	(void)signal;
	showtime=1;
}
