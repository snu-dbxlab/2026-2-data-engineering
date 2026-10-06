#
# Makefile for the storage manager subsystem
#
# src/backend/storage/Makefile
#

subdir = src/backend/storage
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# The buffer manager is selected by configure: --enable-snudbx-buffer swaps the
# stock buffer/ directory for the course's snudbx_buffer/.  Only one of the two
# is ever compiled, so files under snudbx_buffer/ need no #ifdef SNUDBX; the
# macro (from pg_config.h) is only needed by the *shared* headers and contrib.
ifeq ($(enable_snudbx_buffer),yes)
SUBDIRS     = aio snudbx_buffer file freespace ipc large_object lmgr page smgr sync
else
SUBDIRS     = aio buffer file freespace ipc large_object lmgr page smgr sync
endif

include $(top_srcdir)/src/backend/common.mk
