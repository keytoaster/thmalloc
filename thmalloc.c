#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <execinfo.h>
#include <stdint.h>

#define min(a, b) (((a) < (b)) ? (a) : (b)) 
#define max(a, b) (((a) > (b)) ? (a) : (b)) 

#define likely(x)    __builtin_expect(!!(x), 1)
#define unlikely(x)  __builtin_expect(!!(x), 0)

#define DEBUG_STR_SIZE 1024
// A buffer for the LOG_* methods below.
static char debug_str[DEBUG_STR_SIZE];

// Logs an error to stderr. The point of this complicated chain of
// calls is to avoid using printf() because it manages its own internal
// buffer (which involves a malloc() call).
#define LOG_ERROR(...) { \
  snprintf(debug_str, DEBUG_STR_SIZE, "*** Thmalloc *** "); \
  write(1, debug_str, sizeof("*** Thmalloc *** ")); \
  memset(debug_str, 0, DEBUG_STR_SIZE); \
  snprintf(debug_str, DEBUG_STR_SIZE, __VA_ARGS__); \
  write(1, debug_str, DEBUG_STR_SIZE); \
}

#define LOG_FATAL(...) { LOG_ERROR(__VA_ARGS__); exit(EXIT_FAILURE); }

#ifdef DEBUG
  #define LOG_DEBUG(...) LOG_ERROR(__VA_ARGS__)
#else
  #define LOG_DEBUG(...) {}
#endif  // DEBUG

static bool thmalloc_initialized;
static bool fall_back_to_real_malloc;

typedef long span_id_t;

// Holds the page size to avoid calling sysconf everywhere.
static long page_size;

// 4 GiB (with 4kB page size)
#define PAGES_MAX (4 * 256 * 1024)

typedef struct addr_span_map_entry addr_span_map_entry;
struct addr_span_map_entry {
  void *addr;
  span_id_t span_id;  // Only valid iff > 0.
};

// Maps an address to a span. Addresses must be aligned on page boundaries.
//
// A fixed-size array to represent this map presented the easiest option (given that
// we can't use a dynamic data structure), but this choice obviously has huge drawbacks:
//   * A fixed number of elements, i.e. only a fixed number of pages (PAGES_MAX) can be
//   allocated.
//   * Looking up an element requires iterating over the array, i.e. O(n) access time.
// A better implementation might be a radix tree.
//
// Requires sizeof(addr_span_map_entry) * PAGES_MAX = 16 MiB of management overhead.
static addr_span_map_entry asmap[PAGES_MAX];

// The number of valid entries in asmap;
static size_t asmap_length;

typedef struct spaninfo_t {
  size_t num_pages;
  bool has_small_objects;
} spaninfo_t;

// Indexed by span_id.
// Requires sizeof(spaninfo_t) * PAGES_MAX of management overhead.
static spaninfo_t spaninfo[PAGES_MAX];
static size_t next_span_id = 1;

// Defines a number of size classes for small object allocations, as in tcmalloc.
// There are no underlying design choices for these classes/sizes and thus they
// should be considered ephemeral.
#define TINY_SIZE 8
#define TINY_NUM 16  // cum 128
#define SMALL_SIZE 16
#define SMALL_NUM 32   // cum 640
#define MEDIUM_SIZE 32
#define MEDIUM_NUM 44  // cum 2048

#define TOTAL_NUM (TINY_NUM + SMALL_NUM + MEDIUM_NUM)  // 92
#define SMALL_ALLOC_THRESHOLD (TINY_SIZE * TINY_NUM + SMALL_SIZE * SMALL_NUM + MEDIUM_SIZE * MEDIUM_NUM)

typedef struct free_list_entry free_list_entry;

struct free_list_entry {
  free_list_entry *next;
};

// Central page heap, as in tcmalloc.
static free_list_entry *page_heap[256];

// Central free lists for small objects, as in tcmalloc.
static free_list_entry *small_object_lists[TOTAL_NUM];

typedef struct {
  size_t class;
  size_t classsize;
} small_objects_span_header;

static void *ThmallocSmall(size_t size);

static void ThmallocInit() {
  // TODO: Extend to radix tree to support more than 4 GiB of memory.
  
  page_size = sysconf(_SC_PAGESIZE);
  LOG_DEBUG("page_size = %lu\n", page_size);

  thmalloc_initialized = true;
  fall_back_to_real_malloc = false;
}

// Returns the asmap entry for the given ptr. ptr must be
// aligned on a page boundary, otherwise use FindAsmapEntryUnaligned.
static addr_span_map_entry *FindAsmapEntry(void *ptr) {
  addr_span_map_entry *entry = asmap;
  size_t last_span = min(PAGES_MAX, asmap_length) - 1;
  while (entry <= &asmap[last_span]) {
    if (entry->addr == ptr) {
      return entry;
    }
    entry++;
  }
   
  LOG_FATAL("Page not found in asmap\n");
}

// Returns the asmap entry for the given ptr. ptr must *not* be
// aligned on a page boundary.
static addr_span_map_entry *FindAsmapEntryUnaligned(void *ptr) {
  return FindAsmapEntry((void *) ((uintptr_t) ptr & ~(uintptr_t) (page_size - 1)));
}

// Returns the span size (number of pages) for entry.
static size_t SpanSize(addr_span_map_entry *entry) {
  span_id_t span_id = entry->span_id;
  return spaninfo[span_id].num_pages;
}

static addr_span_map_entry *GetFirstPageOfSpan(addr_span_map_entry *e) {
  while (e >= asmap) {
    if (e == asmap) {
      return e;
    }
    if ((e - 1)->span_id != e->span_id) {
      return e;
    }
    e--;
  }

  LOG_FATAL("unexpected value passed to GetFirstPageOfSpan.\n");
}

static int GetClassForSize(size_t size) {
  if (size <= TINY_SIZE * TINY_NUM) {
    return (size - 1) / TINY_SIZE;
  }
  
  size -= TINY_SIZE * TINY_NUM;

  if (size <= SMALL_SIZE * SMALL_NUM) {
    return TINY_NUM + (size - 1) / SMALL_SIZE;
  }

  size -= SMALL_SIZE * SMALL_NUM;
  return TINY_NUM + SMALL_NUM + (size - 1) / MEDIUM_SIZE;
}

static int GetClassSizeForSize(size_t size) {
  if (size <= TINY_SIZE * TINY_NUM) {
    return ((size - 1) / TINY_SIZE + 1) * TINY_SIZE;
  }
  
  if (size <= SMALL_SIZE * SMALL_NUM) {
    return ((size - 1) / SMALL_SIZE + 1) * SMALL_SIZE;
  }

  return ((size - 1) / MEDIUM_SIZE + 1) * MEDIUM_SIZE;
}

static int GetSpanSizeForClassSize(size_t classsize) {
  // Target 128 objects for each class.
  return (classsize * 128 - 1) / page_size + 1;
}

// Allocates a span of span_size pages from the OS.
static void *SysGetSpan(size_t span_size) {
  void *ret = mmap(NULL, page_size * span_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
  if (ret == (void *)-1) {
    perror("mmap");
    return NULL;
  }

  addr_span_map_entry *entry = asmap;
  size_t free_entries_found = 0;
  for (int i = 0; i < asmap_length; i++) {
    if (entry->span_id == 0) {
      free_entries_found++;
      if (free_entries_found == span_size) {
        break;
      }
    } else {
      free_entries_found = 0;
    }
  }

  bool found = false;

  if (free_entries_found == span_size) {
    // Go back to first entry.
    entry -= (span_size - 1);
    found = true;
  } else if (asmap_length + span_size <= PAGES_MAX) {
    entry = &asmap[asmap_length];
    asmap_length += span_size;
    found = true;
  }

  LOG_DEBUG("asmap_length now %lu\n", asmap_length);
  if (!found) {
    LOG_DEBUG("not enough contiguous addr_span_map_entry elements.\n");
    return NULL;
  }

  for (int i = 0; i < span_size; i++) {
    entry->addr = ret + i * page_size;
    entry->span_id = next_span_id;

    entry++;
  }

  spaninfo[next_span_id].num_pages = span_size;
  spaninfo[next_span_id].has_small_objects = false;

  next_span_id++;

  return ret;
}

// Main function for large object allocation.
// Allocates a span capable of holding size many bytes.
static void *ThmallocLarge(size_t size) {
  size_t pages_required = (size - 1) / page_size + 1;
  LOG_DEBUG("pages required: %lu\n", pages_required);

  if (page_heap[pages_required]) {
    // Serve from page_heap.
    free_list_entry *entry = page_heap[pages_required];
    page_heap[pages_required] = entry->next;
    return entry;
  }

  void *span = SysGetSpan(pages_required);
  if (!span) {
    LOG_DEBUG("Could not get span\n");
    return NULL;
  }

  return span;
}

// Main function for small object allocation.
// Returns an object from the free list for size class of the requested
// size. If the free list is empty, it allocates a span of pages from
// the central page heap, splits them into a set of objects of this
// size class, and places them on the free list. 
static void *ThmallocSmall(size_t size) {
  LOG_DEBUG("ThmallocSmall(%lu)\n", size);
  
  size_t class = GetClassForSize(size);
  size_t classsize = GetClassSizeForSize(size);
  size_t span_size = GetSpanSizeForClassSize(classsize);

  if (size > classsize) {
    LOG_FATAL("calculated size class is invalid\n");
  }

  // Fast path: If an object is available on free list, pop it.
  free_list_entry *entry = small_object_lists[class];
  if (entry) {
    LOG_DEBUG("fastpath\n");
    small_object_lists[class] = entry->next;
    return entry;
  }
    
  // If none available, get new span and split it.
  void *p = ThmallocLarge(span_size * page_size);
  // TODO: This is a pretty expensive and redundant call given that ThmallocLarge already
  // knows the entry. Consider making it return all the info we need.
  addr_span_map_entry *map_entry = FindAsmapEntry(p);
  size_t span_id = map_entry->span_id;

  spaninfo[span_id].has_small_objects = true;

  // The span header is placed at the beginning of the first page to indicate how big the objects
  // in this span are.
  small_objects_span_header *header = p;
  header->class = class;
  header->classsize = classsize;

  LOG_DEBUG("Got %p\n", p);
  LOG_DEBUG("class %lu\n", class);
  LOG_DEBUG("classize %lu\n", classsize);
  
  void *ret = p + sizeof(small_objects_span_header);
  LOG_DEBUG("First object: %p\n", ret);
   
  // Create a free list using the pages in the span.
  // No need to zero it out because we set all members explicitly.
  
  // Don't put the first object on the free list as we return it in this call.
  void *it = ret + classsize;
  small_object_lists[class] = it;

  int count = 1;
  void *first_unallocated_address = p + spaninfo[span_id].num_pages * page_size;
  // Checks if an entire element after the current element still fits into the allocated space.
  while (it + 2 * classsize <= first_unallocated_address) {
    ((free_list_entry *)it)->next = it + classsize;
    it += classsize;
    count++;
  }
  // The memory may have been in use before and put back into the page heap,
  // where we obtain it from, so it's not guaranteed to be 0-initialized.
  ((free_list_entry *)it)->next = 0;
  return ret;
}

static void *Thmalloc(size_t size) {
  if (size == 0) {
    return NULL;
  }

  if (size <= SMALL_ALLOC_THRESHOLD) {
    return ThmallocSmall(size);
  }

  return ThmallocLarge(size);
}

static void Thfree(void *ptr) {
  if (ptr == NULL) {
    return;
  }

  LOG_DEBUG("free(%p)\n", ptr);

  addr_span_map_entry *entry = FindAsmapEntryUnaligned(ptr);

  if (!entry) {
    LOG_FATAL("free(%p): Object was not allocated with Thmalloc.\n", ptr);
    // If we find a case where this might be useful, forward to real_free:
    // void (*real_free)(void *) = dlsym(RTLD_NEXT, "free");
    // real_free(ptr);
  }

  span_id_t span_id = entry->span_id;
  spaninfo_t *info = &spaninfo[span_id];
  if (!info->has_small_objects) {
    if (entry != &asmap[0]) {
      // TODO: Coalesce with previous span.
    }
  
    if (entry != &asmap[PAGES_MAX - 1]) {
      // TODO: Coalesce with next span.
    }

    size_t span_size = SpanSize(entry);

    if (!span_size) {
      LOG_DEBUG("free: span_size is 0\n");
    }
    // Store the free_list_entry in the free page.
    free_list_entry *new_entry = ptr;
    new_entry->next = page_heap[span_size];
    page_heap[span_size] = new_entry;
  } else {
    LOG_DEBUG("entry->addr: %p\n", entry->addr);

    addr_span_map_entry *first = GetFirstPageOfSpan(entry);
     
    small_objects_span_header *header = first->addr;
    LOG_DEBUG("class: %lu\n", header->class);
    if ((header->class == 0 && header->classsize != 8) || header->class > TOTAL_NUM) {
      LOG_FATAL("invalid span header found.\n");
    }
    
    free_list_entry *new_entry = ptr;
    new_entry->next = small_object_lists[header->class];
    small_object_lists[header->class] = new_entry; 
  }
}

void *malloc(size_t size) {
  // Constructor is called too late, so we have to initialize on the first malloc() call.
  if (unlikely(!thmalloc_initialized)) {
    ThmallocInit();
  }

  if (unlikely(fall_back_to_real_malloc)) {
    void *(*real_malloc)(size_t s) = dlsym(RTLD_NEXT, "malloc");
    return real_malloc(size);
  }
  
  LOG_DEBUG("malloc(%lu) = ...\n", size);
  void *p = Thmalloc(size);
  LOG_DEBUG("malloc(%lu) = %p\n", size, p);
  return p;
}

void free(void *ptr) {
  if (unlikely(!thmalloc_initialized)) {
    LOG_DEBUG("FREE BEFORE FIRST MALLOC\n");
    // ThmallocInit();
  }

  if (unlikely(fall_back_to_real_malloc)) {
    void (*real_free)(void *) = dlsym(RTLD_NEXT, "free");
    real_free(ptr);
    return;
  }
  
  Thfree(ptr);
}

void *calloc(size_t nmemb, size_t size) {
  void *ptr = malloc(nmemb * size);
  if (!ptr) {
    return NULL;
  }

  memset(ptr, 0, nmemb * size);
  return ptr;
}

void *realloc(void *ptr, size_t size) {
  LOG_DEBUG("realloc(%p, %lu) = ...\n", ptr, size);
  void *p = malloc(size);
  if (!p) {
    perror("malloc");
    return NULL;
  }

  if (!ptr) {
    return p;
  }

  if (size == 0) {
    free(ptr);
    return NULL;
  }

  addr_span_map_entry *entry = FindAsmapEntryUnaligned(ptr);
  size_t old_size = SpanSize(entry) * page_size;
  size_t to_copy = min(old_size, size);
  memcpy(p, ptr, to_copy);

  free(ptr);
  LOG_DEBUG("realloc(%p, %lu) = %p\n", ptr, size, p);
  return p;
}
