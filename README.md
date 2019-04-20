# thmalloc
A rudimentary malloc replacement.

[TOC]

WARN: thmalloc is deprecated.

This is a fun project to understand memory allocators better. It started
during the sunny Easter weekend of 2019 when I was load testing a service I was working on
which showed unexpected memory utilization under load: The process stayed at its peak memory
utilization even when the service was not receiving queries anymore. Assuming this was a
memory leak, I did some troubleshooting, until I realized that utilization would diminish
slowly afterwards, the longer the service remained unloaded. This turned out to be the normal
behaviour of [tcmalloc](http://goog-perftools.sourceforge.net/doc/tcmalloc.html).

The tcmalloc design doc inspired me to have a shot at writing my own memory allocator. The
goal was to implement an allocator less trivial than a textbook free list, but also to cap
implementation to a weekend's time. Success was measured by getting `top` / `htop` (as they
perform a number of allocations/deallocations every second) working using the allocator.

Like tcmalloc, thmalloc distinguishes between small and large object allocations, manages
a page heap for large objects, and has size classes for small objects. But it comes with
additional limitations and significant design shortcuts to achieve the stated goals. 

## Current limitations

* Only 4 GiB of memory can be allocated.
  * thmalloc currently uses a fixed-size array to map a page to a span.
  * To lift this limitation, one would have to replace this fixed-size map, e.g. with a radix tree.
* No multithreading support: thmalloc currently relies on global data structures without locking.
* Spans are not yet coalesced with neighbouring spans once free'd.
* No support to give memory back to the system. All mmap'ed pages are kept forever.

## How to use

To run `top` with `thmalloc`:

```shell
$ gcc -Wall -fPIC -ldl -shared -o thmalloc.o thmalloc.c && LD_PRELOAD=$(pwd)/thmalloc.o top -bn3
```

With debugging symbols and output:

```shell
$ gcc -g -DDEBUG -Wall -fPIC -ldl -shared -o thmalloc.o thmalloc.c && LD_PRELOAD=$(pwd)/thmalloc.o top -bn3
```

To debug the test application with `gdb`:

```shell
$ gcc -g -DDEBUG -Wall -fPIC -ldl -shared -o thmalloc.o thmalloc.c && gcc -g -o test test.c && gdb -tui --args env LD_PRELOAD=$(pwd)/thmalloc.o ./test
(Set breakpoint: b thmalloc.c:72)
```

## Notes

Backtracing without `gdb`:

```c
int n = backtrace((void**)asmap, 100);
char **foo = backtrace_symbols((void**)asmap, n);
for (int i = 0; i < n; i++) {
  printf("%s\n", foo[i]);
}
```

