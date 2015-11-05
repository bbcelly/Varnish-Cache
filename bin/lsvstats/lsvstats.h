#define SOCKETS_MAX	65536
#define CONFT_ERR	0
#define	CONFT_ALL	1
#define CONFT_ONE	2
#define CONFT_INV   3
#define CONFTN		4

typedef enum { false, true } bool;
typedef struct regexp_s {
	char	*key;
	regex_t	reg;
} regexp;

enum httpcodes {
	GET=1,
	POST,
	HEAD,
	INVALID
};

enum vcachestatus {
	hit=1,
	miss,
	pass
};

typedef struct vslf_s {
	bool	error;				//error ocured on cellecting?
	int		status;				//HTTP status code
	char	*sstatus;			//HTTP status code in string
	char	*url;				//URL request
	double	ttfb;				//time to first byte
	double	ttlb;				//time to last byte
	enum httpcodes		req;	//HTTP request methode
	char				*sreq;	//HTTP request methode in string
	enum vcachestatus	handling;	//Varnish Cache status code
} vslf;							//Varnish Statistics Log Format

typedef struct vsdf_s {
	unsigned long	c;		//counter
	double			ttfb;	//total time
	double			*dttfb;	//array of times
	double			ttlb;	//total time
	double			*dttlb;	//array of times
	unsigned long	dm;		//max size of array
} vsdf;						//Varnish Statistics Data Field

static void cleanup(int signal);
static void output(int signal);

static char*  strip(char *s, int n);
static FILE * log_open();

static void lsvs_init();
static void lsvs_clear();
static void lsvs_cleanup();
static void lsvs_add(enum vcachestatus handl, unsigned i, double ttfb, double ttlb);
static int  lsvs_qsort_cmp (const void * a, const void * b);
static void lsvs_compute();

static FILE*    config_open(const char *f_arg);
static unsigned config_size();
static void     config_read(const char *f_arg);
static void     config_cleanup();

static bool vslf_status(vslf *ptr);
static void vslf_cleanup(vslf *ptr);
static void vslf_clear(vslf *ptr);
static void vslf_init();

static void collect_analyze(int fd, const struct VSM_data *vd);
static void collect_init();
static void collect_cleanup();
static int  collect(void *priv, enum VSL_tag_e tag, unsigned fd, unsigned len, unsigned spec, const char *ptr, uint64_t bm);

static void usage(void);
