#

INCLUDES = -I$(top_srcdir)/include

bin_PROGRAMS = varnishadm

dist_man_MANS = varnishadm.1

varnishadm_SOURCES = \
	varnishadm.c \
	$(top_builddir)/lib/libvarnish/assert.c \
	$(top_builddir)/lib/libvarnish/tcp.c \
	$(top_builddir)/lib/libvarnish/vss.c

varnishadm_CFLAGS = @LIBEDIT_CFLAGS@

varnishadm_LDADD = \
	$(top_builddir)/lib/libvarnishapi/libvarnishapi.la \
	$(top_builddir)/lib/libvarnishcompat/libvarnishcompat.la \
	${PTHREAD_LIBS} ${NET_LIBS} @LIBEDIT_LIBS@ ${LIBM}

varnishadm.1: $(top_srcdir)/doc/sphinx/reference/varnishadm.rst
if HAVE_RST2MAN
	${RST2MAN} $? $@
else
	@echo "========================================"
	@echo "You need rst2man installed to make dist"
	@echo "========================================"
	@false
endif
