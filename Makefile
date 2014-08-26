TARGET      = rlm_redis_ippool
SRCS        = rlm_redis_ippool.c
HEADERS     = $(top_builddir)/src/modules/rlm_redis/rlm_redis.h
RLM_CFLAGS  = -I$(top_builddir)/src/modules/rlm_redis  -I/root
RLM_LIBS    = -L/root/hiredis -lhiredis 
RLM_INSTALL =

include ../rules.mak

$(LT_OBJS): $(HEADERS)
