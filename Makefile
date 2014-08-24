#
# $Id$
#

TARGET		= rlm_redisn
SRCS		= rlm_redisn.c redisn.c
HEADERS		= rlm_redisn.h conf.h
RLM_CFLAGS	= -I/usr/local/include/hiredis $(INCLTDL) -I$(top_builddir)/src/modules/rlm_redisn
RLM_LIBS	= -lhiredis $(LIBLTDL)

include ../rules.mak

$(LT_OBJS): $(HEADERS)
