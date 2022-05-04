AM_CFLAGS = \
        $(WARNING_CFLAGS) \
        $(CODE_COVERAGE_CFLAGS)

AM_LDFLAGS = \
        $(CODE_COVERAGE_LIBS)

AM_CPPFLAGS = \
        -I$(top_srcdir) \
        -I$(top_srcdir)/src/include \
        -I$(top_builddir)/src/common/libflux \
        -I$(top_srcdir)/src/common/libccan \
        $(JANSSON_CFLAGS)

fluxmod_LTLIBRARIES = \
	derp.la

derp_la_LDFLAGS = \
        $(fluxlib_ldflags) \
        -avoid-version \
        -export-symbols-regex \
        '^(mod_name|mod_service|mod_main)$$' \
        -module
derp_la_LIBADD = \
        $(top_builddir)/src/common/libjob/libjob.la \
        $(top_builddir)/src/common/libflux-internal.la \
        $(top_builddir)/src/common/libflux-core.la \
        $(JANSSON_LIBS)


derp_la_SOURCES = \
	peer.c \
	peer.h \
	job.c \
	job.h \
	hello.c \
	hello.h \
	derp.c \
	derp.h \
	ping.c \
	exec.c \
	exec/barrier.c \
	exec/barrier.h
