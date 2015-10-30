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

typedef enum { false, true } bool;

/* Globals ------------------------------------------------------------*/
#define SOCKETS_MAX	65536

static volatile sig_atomic_t showtime;

typedef struct {
	char	*key;
	regex_t	reg;
} RegExp;

RegExp	conf[100];
int		confs = 0;

enum HttpCodes {
	GET=1,
	POST,
	HEAD,
	INVALID
};

enum VCacheStatus {
	hit=1,
	miss,
	pass
};

typedef struct {
	bool	error;				//error ocured on cellecting?
	int		status;				//HTTP status code
	char	*url;				//URL request
	double	ttfb;				//time to first byte
	double	ttlb;				//time to last byte
	enum HttpCodes		req;	//HTTP request methode
	enum VCacheStatus	handling;	//Varnish Cache status code
} vslf;							//Varnish Statistics Log Format

vslf			ob[SOCKETS_MAX];
static uint64_t	bitmap[SOCKETS_MAX];

typedef struct {
	unsigned long	c;		//counter
	double			ttfb;	//total time
	double			*dttfb;	//array of times
	double			ttlb;	//total time
	double			*dttlb;	//array of times
	unsigned long	dm;		//max size of array
} vsdf;						//Varnish Statistics Data Field

vsdf	*lsvs_d_hit;
vsdf	*lsvs_d_miss;

vsdf	*(lsvs_d[4]);

static void cleanup(int signal);
static void output(int signal);

/* Data structure handling --------------------------------------------*/

static void lsvs_init()
{
	unsigned i;

	//fprintf(stderr, "D----> init\n");
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

static void lsvs_clear()
{
	int i;
	//fprintf(stderr, "D----> clear\n");

	for (i=0; i < confs; i++) {
		lsvs_d_miss[i].c=0;
		lsvs_d_miss[i].ttfb=0.0;
		lsvs_d_miss[i].ttlb=0.0;

		lsvs_d_hit[i].c=0;
		lsvs_d_hit[i].ttfb=0.0;
		lsvs_d_hit[i].ttlb=0.0;
	}
}

static void lsvs_cleanup()
{
	//fprintf(stderr, "D----> cleanup\n");

	for (int i=0; i < confs; i++) {
		free(lsvs_d_miss[i].dttfb);
		free(lsvs_d_miss[i].dttlb);
	}

	free(lsvs_d_miss);

	for (int i=0; i < confs; i++) {
		free(lsvs_d_hit[i].dttfb);
		free(lsvs_d_hit[i].dttlb);
	}

	free(lsvs_d_hit);
}

static void lsvs_add(enum VCacheStatus handl, unsigned int i, double ttfb, double ttlb)
{
	switch (handl) {
		case hit:
			lsvs_d_hit[i].ttfb+=ttfb;
			lsvs_d_hit[i].ttlb+=ttlb;
			lsvs_d_hit[i].dttfb[lsvs_d_hit[i].c]=ttfb;
			lsvs_d_hit[i].dttlb[lsvs_d_hit[i].c]=ttlb;
			lsvs_d_hit[i].c++;

			if (lsvs_d_hit[i].c >= ULONG_MAX) {
				fprintf(stderr, "D----> Data object for %s_hit is full!!!\n", conf[i].key);
				showtime=1;
			} else if (lsvs_d_hit[i].c >= lsvs_d_hit[i].dm) {
				if ((lsvs_d_hit[i].dm*2) > ULONG_MAX) {
					lsvs_d_hit[i].dm=ULONG_MAX;
					fprintf(stderr, "D----> reallocing hit for %s with FUCKING MAX! ...", conf[i].key);

				} else {
					lsvs_d_hit[i].dm*=2;
					fprintf(stderr, "D----> reallocing hit for %s with new size %lu ...", conf[i].key, lsvs_d_hit[i].dm);
				}


				lsvs_d_hit[i].dttfb=(double *)realloc(lsvs_d_hit[i].dttfb, (sizeof(double)*lsvs_d_hit[i].dm));
				lsvs_d_hit[i].dttlb=(double *)realloc(lsvs_d_hit[i].dttlb, (sizeof(double)*lsvs_d_hit[i].dm));

				fprintf(stderr, "Done\n");
			}
			break;
		case miss:
		case pass:
			lsvs_d_miss[i].ttfb+=ttfb;
			lsvs_d_miss[i].ttlb+=ttlb;
			lsvs_d_miss[i].dttfb[lsvs_d_miss[i].c]=ttfb;
			lsvs_d_miss[i].dttlb[lsvs_d_miss[i].c]=ttlb;
			lsvs_d_miss[i].c++;

			if (lsvs_d_miss[i].c >= lsvs_d_miss[i].dm) {
				if ((lsvs_d_miss[i].dm*2) > ULONG_MAX) {
					lsvs_d_miss[i].dm=ULONG_MAX;
					fprintf(stderr, "D----> reallocing hit for %s with FUCKING MAX! ...", conf[i].key);

				} else {
					lsvs_d_miss[i].dm*=2;
					fprintf(stderr, "D----> reallocing hit for %s with new size %lu ...", conf[i].key, lsvs_d_miss[i].dm);
				}


				lsvs_d_miss[i].dttfb=(double *)realloc(lsvs_d_miss[i].dttfb, (sizeof(double)*lsvs_d_miss[i].dm));
				lsvs_d_miss[i].dttlb=(double *)realloc(lsvs_d_miss[i].dttlb, (sizeof(double)*lsvs_d_miss[i].dm));

				fprintf(stderr, "Done\n");
			} else if (lsvs_d_miss[i].c >= ULONG_MAX) {
				fprintf(stderr, "D----> Data object for %s_miss is full!!!\n", conf[i].key);
				showtime=1;
			}
			break;
	}

}

static int lsvs_qsort_cmp (const void * a, const void * b)
{
	if (*(const double*)a > *(const double*)b) {
		return -1;
	} else if (*(const double*)a < *(const double*)b) {
		return 1;
	} else {
		return 0;
	}
}

static void lsvs_compute()
{
	int	i,j,jm;
	double wttfb,wttlb;

	for (i = 0; i < confs; i++) {
		if (lsvs_d_miss[i].c > 0) {
			qsort(lsvs_d_miss[i].dttfb, lsvs_d_miss[i].c, sizeof(double), lsvs_qsort_cmp);
			qsort(lsvs_d_miss[i].dttfb, lsvs_d_miss[i].c, sizeof(double), lsvs_qsort_cmp);

			wttfb=0.0;
			wttlb=0.0;
			jm=(int)floor((double)(lsvs_d_miss[i].c*0.1));

			if (jm > 0) {
				for (j = 0; j < jm; j++) {
					//fprintf(stderr, "C----> %lf , %lf\n", lsvs_d_miss[i].dttfb[j], lsvs_d_miss[i].dttlb[j]);
					wttfb+=lsvs_d_miss[i].dttfb[j];
					wttlb+=lsvs_d_miss[i].dttlb[j];
				}

				wttfb/=(double)jm;
				wttlb/=(double)jm;
			}

			fprintf(stderr, "%s count_miss:%lu avarage_miss:%i 10wa_miss:%i ", conf[i].key, lsvs_d_miss[i].c, (int)floor(lsvs_d_miss[i].ttfb/lsvs_d_miss[i].c), (int)floor(wttfb));
		} else {
			fprintf(stderr, "%s count_miss:%lu avarage_miss:0 10wa_miss:0 ", conf[i].key, lsvs_d_miss[i].c);
		}

		if (lsvs_d_hit[i].c > 0) {
			qsort(lsvs_d_hit[i].dttfb, lsvs_d_hit[i].c, sizeof(double), lsvs_qsort_cmp);
			qsort(lsvs_d_hit[i].dttfb, lsvs_d_hit[i].c, sizeof(double), lsvs_qsort_cmp);

			wttfb=0.0;
			wttlb=0.0;
			jm=(int)floor((double)(lsvs_d_hit[i].c*0.1));

			if (jm > 0) {
				for (j = 0; j < jm; j++) {
					//fprintf(stderr, "C----> %lf , %lf\n", lsvs_d_hit[i].dttfb[j], lsvs_d_hit[i].dttlb[j]);
					wttfb+=lsvs_d_hit[i].dttfb[j];
					wttlb+=lsvs_d_hit[i].dttlb[j];
				}

				wttfb/=(double)jm;
				wttlb/=(double)jm;
			}

			fprintf(stderr, "count_hit:%lu avarage_hit:%i 10wa_hit:%i\n", lsvs_d_hit[i].c, (int)floor(lsvs_d_hit[i].ttfb/lsvs_d_hit[i].c), (int)floor(wttfb));
		} else {
			fprintf(stderr, "count_hit:%lu avarage_hit:0 10wa_hit:0\n", lsvs_d_hit[i].c);
		}
	}
}

/* Config -------------------------------------------------------------*/

static FILE* open_config(const char *f_arg)
{
	FILE* file;

	file = fopen(f_arg, "r");
	if (!file) {
		perror(f_arg);
		exit(1);
	}
	return (file);
}

static void read_config(const char *f_arg)
{
	FILE	*file;
	int     linen = 0;
	int 	ret = 0;
	char    *line = NULL;
	char 	*key;
	char	*reg;
	char	*delim;
	char	msgbuf[100];
	size_t  len = 0;
	ssize_t cn;
	regex_t reconf;
	regex_t recomm;

	file = open_config(f_arg);
	XXXAN(file >= 0);

	/* Compile regular expression for config test*/
	if (regcomp(&reconf, "^[[:space:]]*[[:alnum:]_-]+[[:space:]]+[:][[:space:]]+.*$", REG_EXTENDED | REG_ICASE | REG_NOSUB)
	    || regcomp(&recomm, "^[[:space:]]*[#;].*$", REG_EXTENDED | REG_ICASE | REG_NOSUB)
	   ) {
		fprintf(stderr, "Regular Expression compilation failed! Check-up regex.h C library.\n");
	    cleanup(SIGKILL);
	}

	confs = 0;
	delim = (char *)malloc(10*sizeof(char));

	while ((cn = getline(&line, &len, file)) != -1) {
		linen++;
		key = (char *)malloc((strlen(line)+1)*sizeof(char));
		reg = (char *)malloc((strlen(line)+1)*sizeof(char));

		if ((regexec(&reconf, line, 0, NULL, 0) == 0)
		    && (regexec(&recomm, line, 0, NULL, 0) != 0)
		   ) {
			if (sscanf(line, "%s : %s", key, reg) == 2) {
				conf[confs].key = malloc((strlen(key)+1)*sizeof(char));
				strcpy(conf[confs].key, key);

				ret = regcomp(&conf[confs].reg, reg, REG_EXTENDED | REG_ICASE | REG_NOSUB);
				if (ret != 0) {
					regerror(ret, &conf[confs].reg, msgbuf, sizeof(msgbuf));
    				fprintf(stderr, "Warning [%i]: Regex compilation failed: %s\n", linen, msgbuf);
				}

				confs++;
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

static void config_cleanup() {
	int i;

	for (i=0; i < confs ; i++) {
		free(conf[i].key);
		regfree(&conf[i].reg);
	}
}

/* Analyzing -----------------------------------------------------------*/

//get status of vslf object
static bool vslf_status(vslf *ptr)
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
//clear vslf object
static void vslf_clear(vslf *ptr)
{
	//fprintf(stderr, "CLEAR\n");
	ptr->error=false;
	ptr->status=0;
	if (ptr->url != NULL) {
		free(ptr->url);
		ptr->url=NULL;
	}
	ptr->ttfb=0.0;
	ptr->ttlb=0.0;
	ptr->req=0;
	ptr->handling=0;
}

//analyze collected data
static void analyze(int fd, const struct VSM_data *vd)
{
	int i=0;

	if (vslf_status(&(ob[fd])) && VSL_Matched(vd, bitmap[fd])) {
		//fprintf(stderr, "M----> Matched\n");
		//fprintf(stderr, "A----> %i %s %0.9lf %0.9lf %i %i\n", ob[fd].status, ob[fd].url, ob[fd].ttfb, ob[fd].ttlb, ob[fd].req, ob[fd].handling);
		for (i=0; i < confs; i++) {
			//fprintf(stderr, "V----> %s | %s\n", conf[i].key, ob[fd].url);
			if (regexec(&(conf[i].reg), ob[fd].url, 0, NULL, 0) == 0) {
				//fprintf(stderr, "D----> %s , %lf , %lf\n", conf[i].key, ob[fd].ttfb, ob[fd].ttlb);
				lsvs_add(ob[fd].handling , i, ob[fd].ttfb, ob[fd].ttlb);
			}
		}
	}
	//
	bitmap[fd] = 0;
	vslf_clear(&(ob[fd]));
}

//cleanup data objects at the end
static void collect_cleanup(const struct VSM_data *vd)
{
	unsigned u;

	for (u = 0; u < SOCKETS_MAX; u++) {
		if (vslf_status(&(ob[u]))) {
			if (VSL_Matched(vd, bitmap[u])) {
				//fprintf(stderr, "D----> %i %s %0.9lf %0.9lf %i %i", ob[u].status, ob[u].url, ob[u].ttfb, ob[u].ttlb, ob[u].req, ob[u].handling);
				vslf_clear(&(ob[u]));
			}
			bitmap[u] = 0;
		}
	}
}

static int collect(void *priv, enum VSL_tag_e tag, unsigned fd, unsigned len,
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
				ob[fd].error=true;
			} else {
				ob[fd].handling = 0;
				ob[fd].error=true;
			}
			//fprintf(stderr, "hit/miss by [%s]\n", VSL_tags[tag]);
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
			//fprintf(stderr, "Request by [%s]\n", VSL_tags[tag]);
			break;
		case SLT_TxURL:
		case SLT_RxURL:
			ob[fd].url = (char *)calloc((len+1), sizeof(char));
			strncpy(ob[fd].url, ptr , len);
			//fprintf(stderr, "URL by [%s] %s\n", VSL_tags[tag], ob[fd].url);
			break;
		case SLT_RxStatus:
		case SLT_TxStatus:
			ob[fd].status = 0;
			if (sscanf(ptr, "%i", &(ob[fd].status)) != 1) {
				ob[fd].status = 0;
				ob[fd].error=true;
				//fprintf(stderr, "D-----------> neeeeee%i\n", errno);
			}
			//fprintf(stderr, "Status by [%s]\n", VSL_tags[tag]);
			break;
		case SLT_ReqEnd:
			ob[fd].ttfb = 0;
			ob[fd].ttlb = 0;
			if (sscanf(ptr, "%*u %*u.%*u %*u.%*u %*u.%*u %lf %lf", &(ob[fd].ttfb), &(ob[fd].ttlb)) != 2) {
				ob[fd].ttfb = 0;
				ob[fd].ttlb = 0;
				ob[fd].error=true;
				errno=0;
			}
			//fprintf(stderr, "reqEnd\n");
		case SLT_BackendClose:
		case SLT_BackendReuse:
			//fprintf(stderr, "running analyze by [%s]\n", VSL_tags[tag]);
			analyze(fd, vd);
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
	fprintf(stderr, "usage: lsvstats -f config_file [-D] [-n varnish_name] [-P file]\n");
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

	while ((c = getopt(argc, argv, VSL_ARGS "DP:f:")) != -1) {
		switch (c) {
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
		read_config(f_arg);
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

	/* run */
	showtime=0;
	while (1) {
		i = VSL_Dispatch(vd, collect, vd);
		if (i == 0) {
			collect_cleanup(vd);
			AZ(fflush(stdout));
		}
		else if (i < 0)
			break;
	}
	collect_cleanup(vd);

	/* clear pidfile */
	if (pfh != NULL)
		VPF_Remove(pfh);

	exit(0);
}

/* Signal handling  ---------------------------------------------------*/

static void cleanup(int signal)
{
	fprintf(stderr, "\nStopped by %i!\n\n", signal);

	lsvs_cleanup();
	config_cleanup();
	exit(1);
}

static void output(int signal)
{
	signal=0;
	showtime=1;
}
