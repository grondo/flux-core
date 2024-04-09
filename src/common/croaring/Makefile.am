AM_CFLAGS = \
	$(WARNING_CFLAGS) \
	$(CODE_COVERAGE_CFLAGS)

AM_LDFLAGS = \
	$(CODE_COVERAGE_LIBS)

AM_CPPFLAGS = \
	-I$(srcdir)

noinst_LTLIBRARIES = \
	libcroaring.la
libcroaring_la_SOURCES = \
	roaring/array_util.h \
	roaring/roaring_version.h \
	roaring/memory.h \
	roaring/roaring.h \
	roaring/roaring_array.h \
	roaring/portability.h \
	roaring/utilasm.h \
	roaring/misc/configreport.h \
	roaring/bitset/bitset.h \
	roaring/containers/mixed_union.h \
	roaring/containers/bitset.h \
	roaring/containers/mixed_andnot.h \
	roaring/containers/perfparameters.h \
	roaring/containers/container_defs.h \
	roaring/containers/containers.h \
	roaring/containers/mixed_negation.h \
	roaring/containers/run.h \
	roaring/containers/mixed_subset.h \
	roaring/containers/mixed_equal.h \
	roaring/containers/array.h \
	roaring/containers/mixed_intersection.h \
	roaring/containers/mixed_xor.h \
	roaring/containers/convert.h \
	roaring/art/art.h \
	roaring/bitset_util.h \
	roaring/roaring_types.h \
	roaring/isadetection.h \
	roaring/roaring64.h \
	src/license-comment.h \
	src/isadetection.c \
	src/bitset.c \
	src/bitset_util.c \
	src/roaring_priority_queue.c \
	src/memory.c \
	src/roaring_array.c \
	src/containers/run.c \
	src/containers/convert.c \
	src/containers/mixed_negation.c \
	src/containers/bitset.c \
	src/containers/mixed_subset.c \
	src/containers/containers.c \
	src/containers/array.c \
	src/containers/mixed_equal.c \
	src/containers/mixed_xor.c \
	src/containers/mixed_intersection.c \
	src/containers/mixed_union.c \
	src/containers/mixed_andnot.c \
	src/art/art.c \
	src/roaring64.c \
	src/array_util.c \
	src/roaring.c

