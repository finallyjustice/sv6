//
// Physical page allocator.
// Slab allocator, for chunks larger than one page.
//

#include "types.h"
#include "mmu.h"
#include "kernel.hh"
#include "spinlock.h"
#include "kalloc.hh"
#include "mtrace.h"
#include "cpu.hh"
#include "multiboot.hh"
#include "wq.hh"
#include "page_info.hh"
#include "kstream.hh"
#include "buddy.hh"
#include "log2.hh"
#include "kstats.hh"
#include "vector.hh"
#include "numa.hh"
#include "lb.hh"

#include <algorithm>
#include <iterator>

// Print memory steal events
#define PRINT_STEAL 0

// The maximum number of buddy allocators.  Each CPU needs at least
// one buddy allocator, and we need some margin in case a CPU's memory
// region spans a physical memory hole.

#define MAX_BUDDIES (NCPU + 16)

struct locked_buddy
{
  spinlock lock;
  buddy_allocator alloc;
  __padout__;

  locked_buddy(buddy_allocator &&alloc)
    : lock(spinlock("buddy")), alloc(std::move(alloc)) { }
};

static static_vector<locked_buddy, MAX_BUDDIES> buddies;

struct mempool : public balance_pool<mempool> {
  int buddy_;      // the buddy allocator this pool; it can contain any phys mem
  uintptr_t base_; // base this pool's local memory
  uintptr_t lim_;  // first address beyond this pool's local memory

  mempool(int buddy, int nfree, uintptr_t base,  uintptr_t sz) : 
    balance_pool(nfree), buddy_(buddy), base_(base), lim_ (base+sz) {};
  ~mempool() {};
  NEW_DELETE_OPS(mempool);

  u64 balance_count() const {
    buddy_allocator::stats stats;
    {
      auto l = buddies[buddy_].lock.guard();
      buddies[buddy_].alloc.get_stats(&stats);
    }
    return stats.free;
  };

  void balance_move_to(mempool *target) {
    u64 avail = balance_count();
    // steal no more than max:
    size_t size = (buddy_allocator::MAX_SIZE > avail/2) ? 
      avail / 2 : buddy_allocator::MAX_SIZE;
    auto lb = &buddies[buddy_];
    auto l = lb->lock.guard();
    // XXX we should steal memory that is close to us. lb helps with this
    // because it is aware of interconnect topology, but does this always line
    // up with NUMA nodes?
    // XXX update stats
    void *res = lb->alloc.alloc_nothrow(size);
#if PRINT_STEAL
    cprintf("balance_move_to: stole %ld at %p from buddy %d\n", size, res, buddy_);
#endif
    if (res) {
      // XXX not exactly hot list stealing but it is stealing
      kstats::inc(&kstats::kalloc_hot_list_steal_count);
      target->kfree(res, size);
    }
  };

  void *get_base() const {  
    return (void*)base_; 
  }

  void *get_limit() const { 
    return (void*)lim_;
  }

  char *kalloc(size_t size) 
  {
    auto lb = &buddies[buddy_];
    auto l = lb->lock.guard();
    void *res = lb->alloc.alloc_nothrow(size);    
    return (char *) res;
  }

  void kfree(void *v, size_t size) 
  {
    auto lb = &buddies[buddy_];
    auto l = lb->lock.guard();
    lb->alloc.free(v, size);
  }
};

static static_vector<mempool, MAX_BUDDIES> mempools;

// A class that tracks the order a core should steal in.  This should
// always start with a core's local buddy allocators and work out from
// there.  In the simple case, the next strata is all of the buddies.
class steal_order
{
public:
  struct segment
  {
    // Steal from buddies [low, high)
    size_t low, high;
  };

private:
  // All up to three stealing strata (so five segments)
  typedef static_vector<segment, 5> segment_vector;
  segment_vector segments_;

  friend void to_stream(print_stream *s, const steal_order &steal);

public:
  class iterator
  {
    const steal_order *order_;
    segment_vector::const_iterator it_;
    size_t pos_;

  public:
    iterator(const steal_order *order)
      : order_(order), it_(order->segments_.begin()), pos_(it_->low) { }

    constexpr iterator()
      : order_(nullptr), it_(), pos_(0) { }

    size_t operator*() const
    {
      return pos_;
    }

    iterator &operator++()
    {
      if (++pos_ == it_->high) {
        if (++it_ == order_->segments_.end())
          *this = iterator();
        else
          pos_ = it_->low;
      }
      return *this;
    }

    bool operator==(const iterator &o) const
    {
      // it_ == o.it_ implies order_ == o.order_
      return (it_ == o.it_ && pos_ == o.pos_);
    }

    bool operator!=(const iterator &o) const
    {
      return !(*this == o);
    }
  };

  iterator begin() const
  {
    return iterator(this);
  }

  static constexpr iterator end()
  {
    return iterator();
  }

  // Return the range of buddy allocators that are "local" to this
  // steal_order.  By convention, this is the first range that was
  // added to this steal_order.
  const segment &get_local() const
  {
    return segments_.front();
  }

  bool is_local(size_t index) const
  {
    auto &s = get_local();
    return s.low <= index && index < s.high;
  }

  // Add a range of buddy indexes to steal from.  This will
  // automatically subtract out any ranges that have already been
  // added.
  void add(size_t low, size_t high)
  {
    for (auto &seg : segments_) {
      if (low == seg.low && high == seg.high) {
        return;
      } else if (low < seg.low && high > seg.high) {
        // Split in two.  Do the upper half first to desynchronize the
        // stealing order of different cores.
        add(seg.high, high);
        high = seg.low;
      } else if (low < seg.low && high > seg.low) {
        // Straddles low boundary
        high = seg.low;
      } else if (low < seg.high && high > seg.high) {
        // Straddles high boundary
        low = seg.high;
      }
    }
    if (low >= high)
      return;
    // Try to merge with the last range, unless it's the local range
    if (segments_.size() > 1) {
      if (segments_.back().high == low) {
        segments_.back().high = high;
        return;
      } else if (high == segments_.back().low) {
        segments_.back().low = low;
        return;
      }
    }
    // Add a new segment
    segments_.push_back(segment{low, high});
  }
};

void
to_stream(print_stream *s, const steal_order &steal)
{
  bool first = true;
  for (auto &seg : steal.segments_) {
    if (first)
      s->print("<");
    else
      s->print(" ");
    if (seg.low == seg.high - 1)
      s->print(seg.low);
    else
      s->print(seg.low, "..", seg.high-1);
    if (first)
      s->print(">");
    first = false;
  }
}

// Our slabs aren't really slabs.  They're just pre-sized and
// pre-named regions.
struct slab {
  char name[MAXNAME];
  u64 order;
};

struct slab slabmem[slab_type_max];

extern char end[]; // first address after kernel loaded from ELF file
char *newend;

page_info *page_info_array;
std::size_t page_info_len;
paddr page_info_base;

struct cpu_mem
{
  steal_order steal;
  int mempool;   // XXX cache align?

  // Hot page cache of recently freed pages
  void *hot_pages[KALLOC_HOT_PAGES];
  size_t nhot;
};

// Prefer mycpu()->mem for local access to this.
static percpu<struct cpu_mem> cpu_mem;

static_vector<numa_node, MAX_NUMA_NODES> numa_nodes;

static int kinited __mpalign__;
static char *pgalloc();

struct memory {
  balancer<memory, mempool> b_;

  memory() : b_(this) {};
  ~memory() {};

  NEW_DELETE_OPS(memory);

  mempool* balance_get(int id) const {
    auto mempool = cpu_mem[id].mempool;
    return &(mempools[mempool]);
  }

  void add(int buddy, void *base, size_t size) {
    buddy_allocator::stats stats;
    {
      auto l = buddies[buddy].lock.guard();
      buddies[buddy].alloc.get_stats(&stats);
    }
    auto m = mempool(buddy, stats.free, (uintptr_t) base, size);
    mempools.emplace_back(m);
  }

  char* kalloc(const char *name, size_t size)
  {
    if (!kinited) {
      // XXX Could have a less restricted boot allocator
      assert(size == PGSIZE);
      return pgalloc();
    }
    void *res = nullptr;
    auto mem = mycpu()->mem;
    if (size == PGSIZE) {
      // allocate from page cache, if possible
      if (mem->nhot > 0) {
        res = mem->hot_pages[--mem->nhot];
      }
    }
    if (!res) {
      res = mempools[mem->mempool].kalloc(size);
      if (!res) {
        b_.balance();
        res = mempools[mem->mempool].kalloc(size);
      }
    }
    if (res) {
      if (ALLOC_MEMSET && size <= 16384) {
        char* chk = (char*)res;
        for (int i = 0; i < size - 2*sizeof(void*); i++) {
          // Ignore buddy allocator list links at the beginning of each
          // page
          if ((uintptr_t)&chk[i] % PGSIZE < sizeof(void*)*2)
            continue;
          if (chk[i] != 1)
            spanic.println(shexdump(chk, size),
                           "kalloc: free memory was overwritten ",
                           (void*)chk, "+", i);
        }
        memset(res, 2, size);
      }
      if (!name)
        name = "kmem";
      mtlabel(mtrace_label_block, res, size, name, strlen(name));
      return (char*)res;
    } else {
      cprintf("kalloc: out of memory\n");
      return nullptr;
    }
  }

  // This returns v to the pool who manages the local memory that contains v.
  // XXX Is the right policy?  Maybe leave in it this node's pool?  Or, only
  // return when we have a big chucnk of memory to return? (e.g., a MAX_SIZE
  // buddy area).
  void kfree_pool(void *v, size_t size) 
  {
    // Find the allocator to return v to.
    // XXX update stats
    auto pool = mycpu()->mem->mempool;
    if (!(mempools[pool].get_base() <= v && v < mempools[pool].get_limit())) {
      // memory from a remote pool; which one?
      auto mp = std::lower_bound(mempools.begin(), mempools.end(), v,
                                 [](mempool &mp, void *v) {
                                   return mp.get_limit() < v;
                                 });
      if (v < mp->get_base())
        panic("kfree: pointer %p is not in an allocated region", v);
      pool = mp - mempools.begin();
#if PRINT_STEAL
      cprintf("return memory %p to pool %d\n", v, pool);
#endif
    }
    mempools[pool].kfree(v, size);
  }

  void kfree(void *v, size_t size)
  {
    // Fill with junk to catch dangling refs.
    if (ALLOC_MEMSET && kinited && size <= 16384)
      memset(v, 1, size);

    if (kinited)
      mtunlabel(mtrace_label_block, v);

    if (size == PGSIZE) {
      // Free to the hot list
      scoped_cli cli;
      auto mem = mycpu()->mem;
      if (mem->nhot == KALLOC_HOT_PAGES) {
        // There's no more room in the hot pages list, so free half of
        // it.  We sort the list so we can merge it with the buddy
        // allocator list.
        kstats::inc(&kstats::kalloc_hot_list_flush_count);
        std::sort(mem->hot_pages, mem->hot_pages + (KALLOC_HOT_PAGES / 2));
        // XXX make kfree_batch_pool to batch moving hot pages
        for (size_t i = 0; i < KALLOC_HOT_PAGES / 2; ++i) {
          void *ptr = mem->hot_pages[i];
          kfree_pool(ptr, size);
        }
        // Shift hot page list down
        // XXX(Austin) Could use two lists and switch off
        mem->nhot = KALLOC_HOT_PAGES - (KALLOC_HOT_PAGES / 2);
        memmove(mem->hot_pages, mem->hot_pages + (KALLOC_HOT_PAGES / 2),
                mem->nhot * sizeof *mem->hot_pages);
      }
      mem->hot_pages[mem->nhot++] = v;
      kstats::inc(&kstats::kalloc_page_free_count);
      return;
    }
    kfree_pool(v, size);
  }
};

static memory allmem;


// This class maintains a set of usable physical memory regions.
class phys_map
{
public:
  // The list of regions, in sorted order and without overlaps.
  struct region
  {
    uintptr_t base, end;
  };

private:
  typedef static_vector<region, 128> region_vector;
  region_vector regions;

public:
  const region_vector &get_regions() const
  {
    return regions;
  }

  // Add a region to the physical memory map.
  void add(uintptr_t base, uintptr_t end)
  {
    // Scan for overlap
    auto it = regions.begin();
    for (; it != regions.end(); ++it) {
      if (end >= it->base && base <= it->end) {
        // Found overlapping region
        uintptr_t new_base = MIN(base, it->base);
        uintptr_t new_end = MAX(end, it->end);
        // Re-add expanded region, since it might overlap with another
        regions.erase(it);
        add(new_base, new_end);
        return;
      }
      if (it->base >= base)
        // Found insertion point
        break;
    }
    regions.insert(it, region{base, end});
  }

  // Remove a region from the physical memory map.
  void remove(uintptr_t base, uintptr_t end)
  {
    // Scan for overlap
    for (auto it = regions.begin(); it != regions.end(); ++it) {
      if (it->base < base && end < it->end) {
        // Split this region
        regions.insert(it + 1, region{end, it->end});
        it->end = base;
      } else if (base <= it->base && it->end <= end) {
        // Completely remove region
        it = regions.erase(it) - 1;
      } else if (base <= it->base && end > it->base) {
        // Left truncate
        it->base = end;
      } else if (base < it->end && end >= it->end) {
        // Right truncate
        it->end = base;
      }
    }
  }

  // Remove all regions in another physical memory map.
  void remove(const phys_map &o)
  {
    for (auto &reg : o.regions)
      remove(reg.base, reg.end);
  }

  // Intersect this physical memory map with another.
  void intersect(const phys_map &o)
  {
    if (o.regions.empty()) {
      regions.clear();
      return;
    }
    uintptr_t prevend = 0;
    for (auto &reg : o.regions) {
      remove(prevend, reg.base);
      prevend = reg.end;
    }
    remove(prevend, ~0);
  }

  // Print the memory map.
  void print() const
  {
    for (auto &reg : regions)
      console.println("phys: ", shex(reg.base).width(18).pad(), "-",
                      shex(reg.end - 1).width(18).pad());
  }

  // Return the first region of physical memory of size @c size at or
  // after @c start.  If @c align is provided, the returned pointer
  // will be a multiple of @c align, which must be a power of 2.
  void *alloc(void *start, size_t size, size_t align = 0) const
  {
    // Find region containing start.  Also accept addresses right at
    // the end of a region, in case the caller just right to the last
    // byte of a region.
    uintptr_t pa = v2p(start);
    for (auto &reg : regions) {
      if (pa == 0)
        pa = reg.base;
      if (reg.base <= pa && pa <= reg.end) {
        // Align pa (we do this now so it doesn't matter if alignment
        // pushes it outside of a known region).
        if (align)
          pa = (pa + align - 1) & ~(align - 1);
        // Is there enough space?
        if (pa + size < reg.end)
          return p2v(pa);
        // Not enough space.  Move to next region
        pa = 0;
      }
    }
    if (pa == 0)
      panic("phys_map: out of memory allocating %lu bytes at %p",
            size, start);
    panic("phys_map: bad start address %p", start);
  }

  // Return the maximum allocation size for an allocation starting at
  // @c start.
  size_t max_alloc(void *start) const
  {
    uintptr_t pa = v2p(start);
    for (auto &reg : regions)
      if (reg.base <= pa && pa <= reg.end)
        return reg.end - (uintptr_t)pa;
    panic("phys_map: bad start address %p", start);
  }

  // Return the total number of bytes in the memory map.
  size_t bytes() const
  {
    size_t total = 0;
    for (auto &reg : regions)
      total += reg.end - reg.base;
    return total;
  }

  // Return the lowest base address
  uintptr_t base() const
  {
    uintptr_t b = 0;
    for (auto &reg : regions) {
      if (b == 0)
        b = reg.base;
      if (reg.base < b)
        b = reg.base;
    }
    return b;
  }

  // Return the total number of bytes after address start.
  size_t bytes_after(void *start) const
  {
    uintptr_t pa = v2p(start);
    size_t total = 0;
    for (auto &reg : regions)
      if (reg.base > pa)
        total += reg.end - reg.base;
      else if (reg.base <= pa && pa <= reg.end)
        total += reg.end - (uintptr_t)pa;
    return total;
  }

  // Return the first physical address above all of the regions.
  size_t max() const
  {
    if (regions.empty())
      return 0;
    return regions.back().end;
  }
};

static phys_map mem;

// Parse a multiboot memory map.
static void
parse_mb_map(struct Mbdata *mb)
{
  if(!(mb->flags & (1<<6)))
    panic("multiboot header has no memory map");

  // Print the map
  uint8_t *p = (uint8_t*) p2v(mb->mmap_addr);
  uint8_t *ep = p + mb->mmap_length;
  while (p < ep) {
    struct Mbmem *mbmem = (Mbmem *)p;
    p += 4 + mbmem->size;
    console.println("e820: ", shex(mbmem->base).width(18).pad(), "-",
                    shex(mbmem->base + mbmem->length - 1).width(18).pad(), " ",
                    mbmem->type == 1 ? "usable" : "reserved");
  }

  // The E820 map can be out of order and it can have overlapping
  // regions, so we have to clean it up.

  // Add and merge usable regions
  p = (uint8_t*) p2v(mb->mmap_addr);
  while (p < ep) {
    struct Mbmem *mbmem = (Mbmem *)p;
    p += 4 + mbmem->size;
    if (mbmem->type == 1)
      mem.add(mbmem->base, mbmem->base + mbmem->length);
  }

  // Remove unusable regions
  p = (uint8_t*) p2v(mb->mmap_addr);
  while (p < ep) {
    struct Mbmem *mbmem = (Mbmem *)p;
    p += 4 + mbmem->size;
    if (mbmem->type != 1)
      mem.remove(mbmem->base, mbmem->base + mbmem->length);
  }
}

// simple page allocator to get off the ground during boot
static char *
pgalloc(void)
{
  if (newend == 0)
    newend = end;

  void *p = (void*)PGROUNDUP((uptr)newend);
  memset(p, 0, PGSIZE);
  newend = newend + PGSIZE;
  return (char*) p;
}

void
kmemprint()
{
  for (int cpu = 0; cpu < NCPU; ++cpu) {
    auto &local = cpu_mem[cpu].steal.get_local();
    console.print("cpu ", cpu, ":");
    for (auto buddy = local.low; buddy < local.high; ++buddy) {
      buddy_allocator::stats stats;
      {
        auto l = buddies[buddy].lock.guard();
        buddies[buddy].alloc.get_stats(&stats);
      }
      console.print(" ", buddy, ":[");
      for (size_t order = 0; order <= buddy_allocator::MAX_ORDER; ++order)
        console.print(stats.nfree[order], " ");
      console.print("free ", stats.free, "]");
    }
    console.println();
  }
}

#if KALLOC_LOAD_BALANCE
char*
kalloc(const char *name, size_t size)
{
  return allmem.kalloc(name, size);
}
#else
char*
kalloc(const char *name, size_t size)
{
  if (!kinited) {
    // XXX Could have a less restricted boot allocator
    assert(size == PGSIZE);
    return pgalloc();
  }

  void *res = nullptr;
  const char *source = nullptr;

  if (size == PGSIZE) {
    // Go to the hot list
    scoped_cli cli;
    auto mem = mycpu()->mem;
    if (mem->nhot == 0) {
      // No hot pages; fill half of the cache
      kstats::inc(&kstats::kalloc_hot_list_refill_count);
      auto buddyit = mem->steal.begin(), buddyend = mem->steal.end();
      auto lb = &buddies[*buddyit];
      auto l = lb->lock.guard();
      while (mem->nhot < KALLOC_HOT_PAGES / 2 && buddyit != buddyend) {
        void *page = lb->alloc.alloc_nothrow(PGSIZE);
        if (!page) {
          // Move to the next allocator
          if (++buddyit == buddyend && mem->nhot == 0) {
            // We couldn't allocate any pages; we're probably out of
            // memory, but drop through to the more aggressive
            // general-purpose allocator.
            goto general;
          }
          lb = &buddies[*buddyit];
          l = lb->lock.guard();
          if (!mem->steal.is_local(*buddyit)) {
            kstats::inc(&kstats::kalloc_hot_list_steal_count);
#if PRINT_STEAL
            cprintf("CPU %d stealing hot list from buddy %lu\n",
                    myid(), *buddyit);
#endif
          }
        } else {
          mem->hot_pages[mem->nhot++] = page;
        }
      }
      source = "refilled hot list";
    }
    res = mem->hot_pages[--mem->nhot];
    kstats::inc(&kstats::kalloc_page_alloc_count);
    if (!source)
      source = "hot list";
  } else {
    // General allocation path for non-PGSIZE allocations or if we
    // can't fill our hot page cache.
  general:
    // XXX(Austin) Would it be better to linear scan our local buddies
    // and then randomly traverse the others to avoid hot-spots?
    for (auto idx : mycpu()->mem->steal) {
      auto &lb = buddies[idx];
      auto l = lb.lock.guard();
      res = lb.alloc.alloc_nothrow(size);
#if PRINT_STEAL
      if (res && mycpu()->mem.steal.is_local(idx))
        cprintf("CPU %d stole from buddy %lu\n", myid(), idx);
#endif
      if (res)
        break;
    }
    source = "buddy";
  }
  if (res) {
    if (ALLOC_MEMSET && size <= 16384) {
      char* chk = (char*)res;
      for (int i = 0; i < size - 2*sizeof(void*); i++) {
        // Ignore buddy allocator list links at the beginning of each
        // page
        if ((uintptr_t)&chk[i] % PGSIZE < sizeof(void*)*2)
          continue;
        if (chk[i] != 1)
          spanic.println(shexdump(chk, size),
                         "kalloc: free memory from ", source,
                         " was overwritten ", (void*)chk, "+", shex(i));
      }
      memset(res, 2, size);
    }
    if (!name)
      name = "kmem";
    mtlabel(mtrace_label_block, res, size, name, strlen(name));
    return (char*)res;
  } else {
    cprintf("kalloc: out of memory\n");
    return nullptr;
  }
}
#endif

void *
ksalloc(int slab)
{
  // XXX(Austin) kalloc should have a kalloc_order variant
  return kalloc(slabmem[slab].name, 1 << slabmem[slab].order);
}

// Initialize free list of physical pages.
void
initkalloc(u64 mbaddr)
{
  parse_mb_map((Mbdata*) p2v(mbaddr));

  // Consider first 1MB of memory unusable
  mem.remove(0, 0x100000);

  console.println("Scrubbed memory map:");
  mem.print();

  // Make sure newend is in the KBASE mapping, rather than the KCODE
  // mapping (which may be too small for what we do below).
  newend = (char*)p2v(v2p(newend));

  // Round newend up to a page boundary so allocations are aligned.
  newend = PGROUNDUP(newend);

  // Allocate the page metadata array.  Try allocating it at the
  // current beginning of free memory.  If this succeeds, then we only
  // need to size it to track the pages *after* the metadata array
  // (since there's no point in tracking the pages that store the page
  // metadata array itself).
  page_info_len = 1 + (mem.max() - v2p(newend)) / (sizeof(page_info) + PGSIZE);
  auto page_info_bytes = page_info_len * sizeof(page_info);
  page_info_array = (page_info*)mem.alloc(newend, page_info_bytes);

  if ((char*)page_info_array == newend) {
    // We were able to allocate it at newend, so we only have to track
    // physical pages following the array.
    newend = PGROUNDUP((char*)page_info_array + page_info_bytes);
    page_info_base = v2p(newend);
  } else {
    // We weren't able to allocate it at the beginning of free memory,
    // so re-allocate it and size it to track all of memory.
    console.println("First memory hole too small for page metadata array");
    page_info_len = 1 + mem.max() / PGSIZE;
    page_info_bytes = page_info_len * sizeof(page_info);
    page_info_array = (page_info*)mem.alloc(newend, page_info_bytes);
    page_info_base = 0;
    // Mark this as a hole in the memory map so we don't use it to
    // initialize the physical allocator below.
    mem.remove(v2p(page_info_array), v2p(page_info_array) + page_info_bytes);
  }

  // Remove memory before newend from the memory map
  mem.remove(0, v2p(newend));

  // XXX(Austin) This handling of page_info_array is somewhat
  // unfortunate, given how sparse physical memory can be.  We could
  // break it up into chunks with a fast lookup table.  We could
  // virtually map it (probably with global large pages), though that
  // would increase TLB pressure.

  // XXX(Austin) Spread page_info_array across the NUMA nodes, both to
  // limit the impact on node 0's space and to co-locate it with the
  // pages it stores metadata for.

  if (VERBOSE)
    cprintf("%lu mbytes\n", mem.bytes() / (1<<20));

  // Construct one or more buddy allocators for each NUMA node
  // XXX(austin) To reduce lock pressure, we might want to further
  // subdivide these and spread out CPUs within a node (but still
  // prefer stealing from the same node before others).

#if KALLOC_LOAD_BALANCE
  void *base = p2v(mem.base());
  size_t sz = (size_t) p2v(mem.max()) - (size_t) base;
#endif
  for (auto &node : numa_nodes) {
    phys_map node_mem;
    // Intersect node memory region with physical memory map to get
    // the available physical memory in the node
    for (auto &mem : node.mems)
      node_mem.add(mem.base, mem.base + mem.length);
    node_mem.intersect(mem);
    // Remove this node from the physical memory map, just in case
    // there are overlaps between nodes
    mem.remove(node_mem);

    if (ALLOC_MEMSET)
      console.println("kalloc: Clearing node ", node.id);

    // Divide the node into at least subnodes buddy allocators
#if KALLOC_BUDDY_PER_CPU
    size_t subnodes = node.cpus.size();
#else
    size_t subnodes = 1;
#endif
    size_t size_limit = (node_mem.bytes() + subnodes - 1) / subnodes;

    // Create buddies
    size_t node_low = buddies.size();
    for (auto &reg : node_mem.get_regions()) {
      if (ALLOC_MEMSET)
        memset(p2v(reg.base), 1, reg.end - reg.base);

      // Subdivide region
      auto remaining = reg;
      while (remaining.base < remaining.end) {
        size_t subsize = std::min(remaining.end - remaining.base, size_limit);
#if KALLOC_LOAD_BALANCE
        // Make an allocator for [base, base+sz) but only mark
        // [reg.base, reg.base+size) as free.  This allows us to move
        // phys memory from one buddy to another during
        // balance_move_to().
        auto buddy = buddy_allocator(p2v(remaining.base), subsize, base, sz);
#else
        // The buddy allocator can manage any page within this node
        auto buddy = buddy_allocator(p2v(remaining.base), subsize,
                                     p2v(reg.base), reg.end - reg.base);
#endif
        if (!buddy.empty()) {
          buddies.emplace_back(std::move(buddy));
          allmem.add(buddies.size()-1, p2v(remaining.base), subsize);
        }
        // XXX(Austin) It would be better if we knew what free_init
        // has rounded the upper bound to.
        remaining.base += subsize;
      }
    }
    size_t node_buddies = buddies.size() - node_low;

    // Associate buddies with CPUs
    size_t cpu_index = 0;
    for (auto &cpu : node.cpus) {
      cpu->mem = &cpu_mem[cpu->id];
      // Divvy up the subnodes between the CPUs in this node.  Assume
      // at first that this is disjoint.
      size_t cpu_low = node_low + cpu_index * node_buddies / node.cpus.size(),
        cpu_high = node_low + (cpu_index + 1) * node_buddies / node.cpus.size();
      // If we have more CPUs than subnodes, we need the assignments
      // to overlap.
      if (cpu_low == cpu_high)
        ++cpu_high;
      assert(cpu_high <= node_low + node_buddies);
      // First allocate from the subnodes assigned to this CPU.
      cpu->mem->steal.add(cpu_low, cpu_high);
      // Then steal from the whole node (this will be a no-op if
      // there's only one subnode).
      cpu->mem->steal.add(node_low, node_low + node_buddies);
      cpu->mem->nhot = 0;
      cpu->mem->mempool = node_low;
      ++cpu_index;
    }
  }

  // Finally, allow CPUs to steal from any buddy
  for (int cpu = 0; cpu < NCPU; ++cpu)
    cpus[cpu].mem->steal.add(0, buddies.size());

  if (0) {
    console.println("kalloc: Buddy steal order (<local> remote)");
    for (int cpu = 0; cpu < NCPU; ++cpu)
      console.println("  CPU ", cpu, ": ", cpus[cpu].mem->steal);
  }

  if (!mem.get_regions().empty())
    // XXX(Austin) Maybe just warn?
    panic("Physical memory regions missing from NUMA map");

  // Configure slabs
  strncpy(slabmem[slab_stack].name, "kstack", MAXNAME);
  slabmem[slab_stack].order = ceil_log2(KSTACKSIZE);

  strncpy(slabmem[slab_perf].name, "kperf", MAXNAME);
  slabmem[slab_perf].order = ceil_log2(PERFSIZE);

  strncpy(slabmem[slab_wq].name, "wq", MAXNAME);
  slabmem[slab_wq].order = ceil_log2(PGROUNDUP(wq_size()));

  kminit();
  kinited = 1;
}

#if KALLOC_LOAD_BALANCE
void
kfree(void *v, size_t size)
{
  allmem.kfree(v, size);
}
#else
void
kfree(void *v, size_t size)
{
  // Fill with junk to catch dangling refs.
  if (ALLOC_MEMSET && kinited && size <= 16384)
    memset(v, 1, size);

  if (kinited)
    mtunlabel(mtrace_label_block, v);

  auto mem = mycpu()->mem;
  if (size == PGSIZE) {
    // Free to the hot list
    scoped_cli cli;
    if (mem->nhot == KALLOC_HOT_PAGES) {
      // There's no more room in the hot pages list, so free half of
      // it.  We sort the list so we can merge it with the buddy
      // allocator list, minimizing and batching our locks.
      kstats::inc(&kstats::kalloc_hot_list_flush_count);
      std::sort(mem->hot_pages, mem->hot_pages + (KALLOC_HOT_PAGES / 2));
      locked_buddy *lb = nullptr;
      lock_guard<spinlock> lock;
      for (size_t i = 0; i < KALLOC_HOT_PAGES / 2; ++i) {
        void *ptr = mem->hot_pages[i];
        // Do we have the right buddy?
        if (!lb || !lb->alloc.contains(ptr)) {
          // Find the first buddy in steal order that contains ptr.
          // We do it this way in case there are overlapping buddies.
          lock.release();
          lb = nullptr;
          for (auto buddyidx : mem->steal) {
            if (buddies[buddyidx].alloc.contains(ptr)) {
              lb = &buddies[buddyidx];
              break;
            }
          }
          assert(lb);
          if (!mem->steal.is_local(lb - &buddies[0])) {
            kstats::inc(&kstats::kalloc_hot_list_remote_free_count);
#if PRINT_STEAL
            cprintf("CPU %d returning hot list to buddy %lu\n", myid(),
                    lb - &buddies[0]);
#endif
          }
          lock = lb->lock.guard();
        }
        lb->alloc.free(ptr, PGSIZE);
      }
      lock.release();
      // Shift hot page list down
      // XXX(Austin) Could use two lists and switch off
      mem->nhot = KALLOC_HOT_PAGES - (KALLOC_HOT_PAGES / 2);
      memmove(mem->hot_pages, mem->hot_pages + (KALLOC_HOT_PAGES / 2),
              mem->nhot * sizeof *mem->hot_pages);
    }
    mem->hot_pages[mem->nhot++] = v;
    kstats::inc(&kstats::kalloc_page_free_count);
    return;
  }

  // Find the first allocator in steal order to return v to.  This
  // will check our local allocators first and handle overlapping
  // buddies.
  for (auto buddyidx : mem->steal) {
    if (buddies[buddyidx].alloc.contains(v)) {
      auto l = buddies[buddyidx].lock.guard();
      buddies[buddyidx].alloc.free(v, size);
      return;
    }
  }
  panic("kfree: pointer %p is not in an allocated region", v);
}
#endif

void
ksfree(int slab, void *v)
{
  kfree(v, 1 << slabmem[slab].order);
}
