/*
 * Copyright (C) 2015, Ingo Molnar <mingo@redhat.com>
 */
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>

/*
 * Repurposed code stolen from Ingo Molnar, see:
 * https://lkml.org/lkml/2015/5/19/1009
 */

char *testcase_description = "vfsmix";

void testcase(unsigned long long *iterations, unsigned long nr)
{
        int pagesize = sysconf(_SC_PAGESIZE);

        while (1) {
                char tmpfile[] = "/tmp/willitscale.XXXXXX";
                int fd = mkstemp(tmpfile);

                struct stat stat_buf;
                int ret = lstat(tmpfile, &stat_buf);
                assert(ret == 0);

                ret = lseek(fd, pagesize - 1, SEEK_SET);
                assert(ret == pagesize - 1);

                close(fd);

                fd = open(tmpfile, O_RDWR|O_CREAT|O_TRUNC, 0600);
                assert(fd >= 0);

                {
                        char c = 1;

                        ret = write(fd, &c, 1);
                        assert(ret == 1);
                }

                {
                        char *mmap_buf = (char *)mmap(0, pagesize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

                        assert(mmap_buf != MAP_FAILED);

                        mmap_buf[0] = 1;

                        ret = munmap(mmap_buf, pagesize);
                        assert(ret == 0);
                }

                close(fd);

                ret = unlink(tmpfile);
                assert(ret == 0);

                (*iterations)++;
        }
}
