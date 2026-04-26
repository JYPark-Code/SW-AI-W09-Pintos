#include "threads/palloc.h"
#include <bitmap.h>
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/init.h"
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

/* Page allocator.  Hands out memory in page-size (or
   page-multiple) chunks.  See malloc.h for an allocator that
   hands out smaller chunks.

   System memory is divided into two "pools" called the kernel
   and user pools.  The user pool is for user (virtual) memory
   pages, the kernel pool for everything else.  The idea here is
   that the kernel needs to have memory for its own operations
   even if user processes are swapping like mad.

   By default, half of system RAM is given to the kernel pool and
   half to the user pool.  That should be huge overkill for the
   kernel pool, but that's just fine for demonstration purposes. */

/* A memory pool. */
struct pool
{
	struct lock lock;		 /* Mutual exclusion. */
	struct bitmap *used_map; /* Bitmap of free pages. */
	uint8_t *base;			 /* Base of pool. */
};

/* Two pools: one for kernel data, one for user pages. */
static struct pool kernel_pool, user_pool;

/* Maximum number of pages to put in user pool. */
size_t user_page_limit = SIZE_MAX;
static void
init_pool(struct pool *p, void **bm_base, uint64_t start, uint64_t end);

static bool page_from_pool(const struct pool *, void *page);

/* multiboot info */
/**
 * 메모리에서 배치
 * offset + 0 : flags
 * offset + 4 : mem_low
 * offset + 8 : mem_high
 * offset + 12 : __unused[0] (패딩)
 * ...
 * offset + 40 : __unused[7]
 * offset + 44 : mmap_len
 * offset + 48 : mmap_base
 *
 * 실제 이 오프셋은 실제 loader 쪽 상수와 정확히 맞아야 함
 *
 * flags : 어떤 부트 정보 필드가 유효한지를 나타내는 비트마스크
 * loader가 0x40을 써 넣음 : loader.S
 * 의미는 대충 "메모리 맵 정보(mmap_len, mmap_base)가 유효하다"는 표시
 * 현재 palloc.c는 이 flags를 직접 검사하진 않지만, 원래 포맷의 일부라서 구조체에 있어야 함.
 *
 * mem_low
 * 전통적인 x86 부트 정보에서 "1MB 이하 low memory 크기"를 담는 필드
 * mem_high
 * 1MB 위쪽 "high memory 크기"를 담는 전통적인 요약 필드
 * 사실상 쓰지 않음 -> e820가 훨씬 정확해서 (mem_low, mem_high은 옛날 방식의 대략적인 메모리 크기 정보)
 */
struct multiboot_info
{
	uint32_t flags;
	uint32_t mem_low;
	uint32_t mem_high;
	uint32_t __unused[8];
	uint32_t mmap_len;
	uint32_t mmap_base;
};

/* e820 entry */
/**
 * BIOS int 0x15, eax=0xe820가 돌려주는 메모리 맵 한줄(entry)을 표현하는 구조체
 * mem_lo, mem_hi : 시작 물리 주소 64bit
 * len_lo, len_hi : 길이 64비트
 * type : 이 구간이 usable인지 reserved인지 등
 */
struct e820_entry
{
	uint32_t size;
	uint32_t mem_lo;
	uint32_t mem_hi;
	uint32_t len_lo;
	uint32_t len_hi;
	uint32_t type;
};

/* Represent the range information of the ext_mem/base_mem */
struct area
{
	uint64_t start;
	uint64_t end;
	uint64_t size;
};

#define BASE_MEM_THRESHOLD 0x100000
#define USABLE 1
#define ACPI_RECLAIMABLE 3
#define APPEND_HILO(hi, lo) (((uint64_t)((hi)) << 32) + (lo))

/* Iterate on the e820 entry, parse the range of basemem and extmem. */
static void
resolve_area_info(struct area *base_mem, struct area *ext_mem)
{
	/**
	 * "부트로더가 커널에 넘겨주는 부트 정보" (규칙)
	 */
	struct multiboot_info *mb_info = ptov(MULTIBOOT_INFO);
	/**
	 * mb_info의 mmap_base를 가져와서 세팅
	 */
	struct e820_entry *entries = ptov(mb_info->mmap_base);
	uint32_t i;

	for (i = 0; i < mb_info->mmap_len / sizeof(struct e820_entry); i++)
	{
		struct e820_entry *entry = &entries[i];
		// reclaimable : 재생가능한, 교환가능한
		if (entry->type == ACPI_RECLAIMABLE || entry->type == USABLE)
		{
			uint64_t start = APPEND_HILO(entry->mem_hi, entry->mem_lo);
			uint64_t size = APPEND_HILO(entry->len_hi, entry->len_lo);
			uint64_t end = start + size;
			printf("%llx ~ %llx %d\n", start, end, entry->type);

			struct area *area = start < BASE_MEM_THRESHOLD ? base_mem : ext_mem;

			// First entry that belong to this area.
			if (area->size == 0)
			{
				*area = (struct area){
					.start = start,
					.end = end,
					.size = size,
				};
			}
			else
			{ // otherwise
				// Extend start
				if (area->start > start)
					area->start = start;
				// Extend end
				if (area->end < end)
					area->end = end;
				// Extend size
				area->size += size;
			}
		}
	}
}

/*
 * Populate the pool.
 * All the pages are manged by this allocator, even include code page.
 * Basically, give half of memory to kernel, half to user.
 * We push base_mem portion to the kernel as much as possible.
 */
static void
populate_pools(struct area *base_mem, struct area *ext_mem)
{
	/**
	 * _end는 다른 .c 파일에서 정의된 전역변수가 아니라, 링커 스크립트가 만들어주는 심볼
	 * kernel.lds.S
	 * PROVIDE(_end = .)
	 * . = 현재 링크 위치(location counter)
	 * PROVICE(_end = .); = "지금 위치를 _end 라는 이름의 심볼로 만들어라"
	 * 정확하게는 모르겠는데, _end는 커널 이미지의 끝 주소를 가리킨다고 함.
	 *
	 * 즉, 다른 PROVIDE 보다 가장 뒤쪽에 있으므로 커널 이미지의 끝 주소를 가리킨다고 해석하면 됨
	 */
	extern char _end;
	/**
	 * 전체 사이즈를 4096 바이트 의 배수 단위로 정렬하는 것
	 * 커널이 이미 차지한 영역 끝 다음부터
	 * 페이지 경계로 올림해서
	 * 그 뒤 공간부터 page allocator가 쓰기 시작하겠다는 뜻
	 */
	void *free_start = pg_round_up(&_end);

	/**
	 * 커널 페이지와 유저 페이지 나누는 이유
	 * 유저 프로그램이 메모리를 많이 써도 커널이 죽지 않게 하려는 것
	 * user pool은 유저 페이지용
	 * kernel pool은 커널이 쓰는 페이지용
	 *
	 * 유저 프로세스가 메모리를 미친 듯이 사용해도 커널 자체 동작에 필요한 메모리는 남겨두기
	 *
	 * 한개의 풀만 썼을 때 발생하는 문제
	 * 1. 유저 프로그램이 페이지를 엄청 많이 할당
	 * 2. 물리 메모리가 거의 바닥
	 * 3. 커널이 새 스레드 만들기, 페이지 테이블 만들기, 버퍼 만들기 같은 작업을 하려는데 커널도 메모리를 못 구해서 시스템 전체가 불안정해지는 것
	 *
	 * 따라서 pool을 나눠
	 * PAL_USER 요청은 user pool에만 할당
	 * 그 외 커널 요청은 kernel pool에서만 할당
	 */
	uint64_t total_pages = (base_mem->size + ext_mem->size) / PGSIZE;
	uint64_t user_pages = total_pages / 2 > user_page_limit ? user_page_limit : total_pages / 2;
	uint64_t kern_pages = total_pages - user_pages;

	// Parse E820 map to claim the memory region for each pool.
	enum
	{
		KERN_START,
		KERN,
		USER_START,
		USER
	} state = KERN_START;
	uint64_t rem = kern_pages;
	uint64_t region_start = 0, end = 0, start, size, size_in_pg;

	struct multiboot_info *mb_info = ptov(MULTIBOOT_INFO);
	struct e820_entry *entries = ptov(mb_info->mmap_base);

	uint32_t i;
	for (i = 0; i < mb_info->mmap_len / sizeof(struct e820_entry); i++)
	{
		struct e820_entry *entry = &entries[i];
		if (entry->type == ACPI_RECLAIMABLE || entry->type == USABLE)
		{
			start = (uint64_t)ptov(APPEND_HILO(entry->mem_hi, entry->mem_lo));
			size = APPEND_HILO(entry->len_hi, entry->len_lo);
			end = start + size;
			size_in_pg = size / PGSIZE;

			if (state == KERN_START)
			{
				region_start = start;
				state = KERN;
			}

			switch (state)
			{
			case KERN:
				if (rem > size_in_pg)
				{
					rem -= size_in_pg;
					break;
				}
				// generate kernel pool
				init_pool(&kernel_pool,
						  &free_start, region_start, start + rem * PGSIZE);
				// Transition to the next state
				if (rem == size_in_pg)
				{
					rem = user_pages;
					state = USER_START;
				}
				else
				{
					region_start = start + rem * PGSIZE;
					rem = user_pages - size_in_pg + rem;
					state = USER;
				}
				break;
			case USER_START:
				region_start = start;
				state = USER;
				break;
			case USER:
				if (rem > size_in_pg)
				{
					rem -= size_in_pg;
					break;
				}
				ASSERT(rem == size);
				break;
			default:
				NOT_REACHED();
			}
		}
	}

	// generate the user pool
	init_pool(&user_pool, &free_start, region_start, end);

	// Iterate over the e820_entry. Setup the usable.
	uint64_t usable_bound = (uint64_t)free_start;
	struct pool *pool;
	void *pool_end;
	size_t page_idx, page_cnt;

	for (i = 0; i < mb_info->mmap_len / sizeof(struct e820_entry); i++)
	{
		struct e820_entry *entry = &entries[i];
		if (entry->type == ACPI_RECLAIMABLE || entry->type == USABLE)
		{
			uint64_t start = (uint64_t)
				ptov(APPEND_HILO(entry->mem_hi, entry->mem_lo));
			uint64_t size = APPEND_HILO(entry->len_hi, entry->len_lo);
			uint64_t end = start + size;

			// TODO: add 0x1000 ~ 0x200000, This is not a matter for now.
			// All the pages are unuable
			if (end < usable_bound)
				continue;

			start = (uint64_t)
				pg_round_up(start >= usable_bound ? start : usable_bound);
		split:
			if (page_from_pool(&kernel_pool, (void *)start))
				pool = &kernel_pool;
			else if (page_from_pool(&user_pool, (void *)start))
				pool = &user_pool;
			else
				NOT_REACHED();

			pool_end = pool->base + bitmap_size(pool->used_map) * PGSIZE;
			page_idx = pg_no(start) - pg_no(pool->base);
			if ((uint64_t)pool_end < end)
			{
				page_cnt = ((uint64_t)pool_end - start) / PGSIZE;
				bitmap_set_multiple(pool->used_map, page_idx, page_cnt, false);
				start = (uint64_t)pool_end;
				goto split;
			}
			else
			{
				page_cnt = ((uint64_t)end - start) / PGSIZE;
				bitmap_set_multiple(pool->used_map, page_idx, page_cnt, false);
			}
		}
	}
}

/* Initializes the page allocator and get the memory size */
/**
 * 이 머신에서 실제로 어떤 물리 메모리 구간이 사용 가능한지 파악하는 것
 */
uint64_t
palloc_init(void)
{
	/* End of the kernel as recorded by the linker.
	   See kernel.lds.S. */
	extern char _end;
	/**
	 * {.size = 0} -> C의 designated initializer 문법
	 * struct area의 멤버 중
	 * size 만 명시적으로 0으로 초기화하겠다는 뜻
	 * BIOS가 알려준 메모리 맵을 순회하면서 usable memory 구간만 골라내고 그 중 1MB 미만은 base_mem 1MB 이상은 ext_mem
	 * base_mem : 아래쪽 메모리, 보통 0 ~ 1 MB 미만
	 * ext_mem : 1MB 이상 확장 메모리
	 */
	struct area base_mem = {.size = 0};
	struct area ext_mem = {.size = 0};

	/**
	 *
	 */
	resolve_area_info(&base_mem, &ext_mem);
	printf("Pintos booting with: \n");
	printf("\tbase_mem: 0x%llx ~ 0x%llx (Usable: %'llu kB)\n",
		   base_mem.start, base_mem.end, base_mem.size / 1024);
	printf("\text_mem: 0x%llx ~ 0x%llx (Usable: %'llu kB)\n",
		   ext_mem.start, ext_mem.end, ext_mem.size / 1024);
	populate_pools(&base_mem, &ext_mem);
	return ext_mem.end;
}

/* Obtains and returns a group of PAGE_CNT contiguous free pages.
   If PAL_USER is set, the pages are obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the pages are filled with zeros.  If too few pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics. */
void *
palloc_get_multiple(enum palloc_flags flags, size_t page_cnt)
{
	struct pool *pool = flags & PAL_USER ? &user_pool : &kernel_pool;

	lock_acquire(&pool->lock);
	size_t page_idx = bitmap_scan_and_flip(pool->used_map, 0, page_cnt, false);
	lock_release(&pool->lock);
	void *pages;

	if (page_idx != BITMAP_ERROR)
		pages = pool->base + PGSIZE * page_idx;
	else
		pages = NULL;

	if (pages)
	{
		/**
		 * PAL_ZERO : 할당된 메모리를 0으로 초기화
		 * PAL_ASSERT : 메모리 부족 시 커널 강제 종료
		 * PAL_USER : user pool에서 할당
		 */
		if (flags & PAL_ZERO)
			memset(pages, 0, PGSIZE * page_cnt);
	}
	else
	{
		if (flags & PAL_ASSERT)
			PANIC("palloc_get: out of pages");
	}

	return pages;
}

/* Obtains a single free page and returns its kernel virtual
   address.
   If PAL_USER is set, the page is obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the page is filled with zeros.  If no pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics. */
void *
palloc_get_page(enum palloc_flags flags)
{
	return palloc_get_multiple(flags, 1);
}

/* Frees the PAGE_CNT pages starting at PAGES. */
void palloc_free_multiple(void *pages, size_t page_cnt)
{
	struct pool *pool;
	size_t page_idx;

	ASSERT(pg_ofs(pages) == 0);
	if (pages == NULL || page_cnt == 0)
		return;

	if (page_from_pool(&kernel_pool, pages))
		pool = &kernel_pool;
	else if (page_from_pool(&user_pool, pages))
		pool = &user_pool;
	else
		NOT_REACHED();

	page_idx = pg_no(pages) - pg_no(pool->base);

#ifndef NDEBUG
	memset(pages, 0xcc, PGSIZE * page_cnt);
#endif
	ASSERT(bitmap_all(pool->used_map, page_idx, page_cnt));
	bitmap_set_multiple(pool->used_map, page_idx, page_cnt, false);
}

/* Frees the page at PAGE. */
void palloc_free_page(void *page)
{
	palloc_free_multiple(page, 1);
}

/* Initializes pool P as starting at START and ending at END */
static void
init_pool(struct pool *p, void **bm_base, uint64_t start, uint64_t end)
{
	/* We'll put the pool's used_map at its base.
	   Calculate the space needed for the bitmap
	   and subtract it from the pool's size. */
	uint64_t pgcnt = (end - start) / PGSIZE;
	size_t bm_pages = DIV_ROUND_UP(bitmap_buf_size(pgcnt), PGSIZE) * PGSIZE;

	lock_init(&p->lock);
	p->used_map = bitmap_create_in_buf(pgcnt, *bm_base, bm_pages);
	p->base = (void *)start;

	// Mark all to unusable.
	bitmap_set_all(p->used_map, true);

	*bm_base += bm_pages;
}

/* Returns true if PAGE was allocated from POOL,
   false otherwise. */
static bool
page_from_pool(const struct pool *pool, void *page)
{
	size_t page_no = pg_no(page);
	size_t start_page = pg_no(pool->base);
	size_t end_page = start_page + bitmap_size(pool->used_map);
	return page_no >= start_page && page_no < end_page;
}
