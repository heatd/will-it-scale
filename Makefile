CFLAGS+=-Wall -O2 -g

ifeq ($(HWLOC),)
# Link test for libhwloc
HWLOC:=$(shell echo "int main(){}" | $(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) \
               -x c - -o /dev/null -lhwloc 2>/dev/null && echo 1)
endif

ifeq ($(HWLOC), 1)
LDFLAGS+=-lhwloc
CPPFLAGS+=-DHAVE_HWLOC=1
endif

processes := $(patsubst tests/%.c,%_processes,$(wildcard tests/*.c))
threads := $(patsubst tests/%.c,%_threads,$(wildcard tests/*.c))

all: processes threads

processes: $(processes)

threads: $(threads)

posix_semaphore1_processes_FLAGS+=-lpthread
threadspawn1_processes_FLAGS+=-lpthread

$(processes): %_processes: tests/%.o main.c
	$(CC) $(CFLAGS) $(CPPFLAGS) main.c $< $($@_FLAGS) $(LDFLAGS) -o $@

$(threads): %_threads: tests/%.o main.c
	$(CC) -DTHREADS $(CFLAGS) $(CPPFLAGS) main.c $< -pthread $(LDFLAGS) -o $@

clean:
	rm -f tests/*.o *_processes *_threads
