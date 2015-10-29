#

INCLUDES = -I$(top_srcdir)/include

bin_PROGRAMS = lsvstats

dist_man_MANS = lsvstats.1

lsvstats_SOURCES = \
	lsvstats.c \
	$(top_builddir)/lib/libvarnish/assert.c \
	$(top_builddir)/lib/libvarnish/flopen.c \
	$(top_builddir)/lib/libvarnish/version.c \
	$(top_builddir)/lib/libvarnish/vsb.c \
	$(top_builddir)/lib/libvarnish/vpf.c

lsvstats_LDADD = \
	$(top_builddir)/lib/libvarnishcompat/libvarnishcompat.la \
	$(top_builddir)/lib/libvarnishapi/libvarnishapi.la \
	${PTHREAD_LIBS}

lsvstats_LDFLAGS = -lm

lsvstats.1: $(top_srcdir)/doc/sphinx/reference/lsvstats.rst
if HAVE_RST2MAN
	${RST2MAN} $? $@
else
	@echo "========================================"
	@echo "You need rst2man installed to make dist"
	@echo "========================================"
	@false
endif
