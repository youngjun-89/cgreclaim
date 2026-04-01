CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -fPIC -pthread
AR      = ar
ARFLAGS = rcs

LIB_SRCS = cgroup.c cgreclaim.c monitor.c
LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB      = libcgreclaim.a

TEST_BIN = test_cgreclaim

.PHONY: all clean

all: $(LIB) $(TEST_BIN)

$(LIB): $(LIB_OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(TEST_BIN): test.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lcgreclaim -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(LIB_OBJS) $(LIB) $(TEST_BIN)

# Dependencies
cgroup.o: cgroup.c cgroup.h
cgreclaim.o: cgreclaim.c cgreclaim.h cgroup.h
monitor.o: monitor.c cgreclaim.h cgroup.h
