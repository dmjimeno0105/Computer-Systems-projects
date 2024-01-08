/**
 * Description
 * 
 * The structure of our free and allocated blocks:
 *     We defined the structure of our allocated block as a struct block which contains
 *     a struct boundary_tag and char[] fields to store the allocated block's header and
 *     payload. Our MIN_BLOCK_SIZE_WORDS constant also ensures that our struct block has
 *     enough room for a struct boundary_tag footer which we later define using 
 *     set_header_and_footer()
 * 
 * The organization of the free list:
 *     We decided to use segregated fits of 10 free lists stored in an array, 5 of which 
 *     would store a seperate size class for the smaller sizes (8, 24, 32, 40, 120) and 
 *     for the larger sizes we would use ranges (up to 616, 3360, 5792, 41704, 153704) 
 *     where the last free list would store any overflowing range.
 * 
 * How our allocator manipulates the free list:
 *     There is a find_fit_index() function which identifies the proper free list a block
 *     should enter based on an array of 5 exact sizes and 5 ranges selected based on the
 *     provided trace files. This reduced fragmentation and made it easier to find the fit 
 *     for a block. When a block is inserted into a free list, it bases it on the mid of 
 *     the range to either be inserted to the front or the back of the list. If a proper 
 *     fit isn't found in a size class, the insert function moves to the next size class
 *     and inserts the block into that free list. When removing from a free list, the 
 *     block->elem is sent to the list_remove() function and taken out of the free list.
 *     In our find_fit() function, we use a similar approach used when inserting our free
 *     blocks. We first use find_fit_index() to find the smallest free list that could
 *     contain a free block that could fit the requested allocated size. We then iterate
 *     through that free list up to LIMIT times. If no block is found after LIMIT, we take 
 *     the first available free block from a higher class size. Otherwise NULL is returned
 *     and extendheap() must be called.
 * 
 * Other:
 *     Headers and footers act as boundary tags which also tell the size and status of a block.
 *     Blocks are aligned to 16 bytes.
 *     Minimum block size is 8 words.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#include "mm.h"
#include "memlib.h"
#include "config.h"
#include "list.h"

struct boundary_tag
{
    int inuse : 1; // inuse bit
    int size : 31; // size of block, in words
                   // block size
};

/* FENCE is used for heap prologue/epilogue. */
const struct boundary_tag FENCE = {
    .inuse = -1,
    .size = 0};

/* A C struct describing the beginning of each block.
 * For implicit lists, used and free blocks have the same
 * structure, so one struct will suffice for this example.
 *
 * If each block is aligned at 12 mod 16, each payload will
 * be aligned at 0 mod 16.
 */
struct block
{
    struct boundary_tag header; /* offset 0, at address 12 mod 16 */
    char payload[0];            /* offset 4, at address 0 mod 16 */
};

struct free_block
{
    struct boundary_tag header; /* offset 0, at address 12 mod 16 */
    struct list_elem elem;
};

struct free_list
{
    struct list list;
    int mid;
};

/* Basic constants and macros */
#define WSIZE sizeof(struct boundary_tag) /* Word and header/footer size (bytes) */
#define MIN_BLOCK_SIZE_WORDS 8            /* Minimum block size in words */
#define CHUNKSIZE (1 << 5)               /* Extend heap by this amount (words) */
#define FREE_LISTS_SIZE 10
#define SMALL_CLASS_SIZE 5

/**
 * Returns the greater of the two sizes.
*/
static inline size_t max(size_t x, size_t y)
{
    return x > y ? x : y;
}

/**
 * Rounds up the size to the nearest multiple of ALIGNMENT
 * 
 * @param size in bytes
 * @return aligned size in bytes
*/
static size_t align(size_t size)
{
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

static bool is_aligned(size_t size) __attribute__((__unused__));
static bool is_aligned(size_t size)
{
    return size % ALIGNMENT == 0;
}

/* Global variables */
static struct block *heap_listp = 0; /* Pointer to first block */
static struct free_list free_lists[FREE_LISTS_SIZE];
static size_t ranges[] = {8, 24, 32, 40, 120, 616, 3360, 5792, 41704, 153704}; /* in words */

/* Function prototypes for internal helper routines */
static struct block *extend_heap(size_t words);
static void place(struct block *bp, size_t asize);
static struct block *find_fit(size_t asize);
static struct block *coalesce(struct block *bp);
static bool insert_free_block(struct free_block *blk);
static void remove_free_block(struct free_block *blk);
static int find_fit_index(size_t words);

/* Given a block, obtain previous's block footer.
   Works for left-most block also. */
static struct boundary_tag *prev_blk_footer(struct block *blk)
{
    return &blk->header - 1;
}

/* Return if block is free */
static bool blk_free(struct block *blk)
{
    return !blk->header.inuse;
}

/**
 * Returns size of block
 *
 * @return size of block in words
 */
static size_t blk_size(struct block *blk)
{
    return blk->header.size;
}

/* Given a block, obtain pointer to previous block.
   Not meaningful for left-most block. */
static struct block *prev_blk(struct block *blk)
{
    struct boundary_tag *prevfooter = prev_blk_footer(blk);
    assert(prevfooter->size != 0);
    return (struct block *)((void *)blk - WSIZE * prevfooter->size);
}

/* Given a block, obtain pointer to next block.
   Not meaningful for right-most block. */
static struct block *next_blk(struct block *blk)
{
    assert(blk_size(blk) != 0);
    return (struct block *)((void *)blk + WSIZE * blk->header.size);
}

/* Given a block, obtain its footer boundary tag */
static struct boundary_tag *get_footer(struct block *blk)
{
    return ((void *)blk + WSIZE * blk->header.size) - sizeof(struct boundary_tag);
}

/* Set a block's size and inuse bit in header and footer */
static void set_header_and_footer(struct block *blk, int size, int inuse)
{
    blk->header.inuse = inuse;
    blk->header.size = size;
    *get_footer(blk) = blk->header; /* Copy header to footer */
}

/* Mark a block as used and set its size. */
static void mark_block_used(struct block *blk, int size)
{
    set_header_and_footer(blk, size, 1);
}

/* Mark a block as free and set its size. */
static void mark_block_free(struct block *blk, int size)
{
    set_header_and_footer(blk, size, 0);
}

/**
 * Inserts the blk into the free list of respective class size
 *
 * @param blk struct free_block *
 * @return false if blk was < MIN_BLOCK_SIZE_WORDS
 */
static bool insert_free_block(struct free_block *blk)
{
    int i = find_fit_index(blk_size((struct block *)blk));

    assert(i != -1); // words < MIN_BLOCK_SIZE_WORDS
    if (i < 0)
        return false;

    if (blk_size((struct block *)blk) < free_lists[i].mid)
        list_push_front(&(free_lists[i].list), &blk->elem);
    else
        list_push_back(&(free_lists[i].list), &blk->elem);

    return true;
}

/**
 * Removes blk from free list
 *
 * @param blk struct free_block *
 */
static void remove_free_block(struct free_block *blk)
{
    list_remove(&blk->elem);
}

/**
 * Returns the index of the free list that could contain a free block that could fit size words.
 *
 * @param words the size in words >= MIN_BLOCK_SIZE_WORDS
 * @return index of free list, -1 if words < MIN_BLOCK_SIZE_WORDS
 */
static int find_fit_index(size_t words)
{
    assert(words >= 0);
    switch (words)
    {
    case 8:
        return 0;
    case 24:
        return 1;
    case 32:
        return 2;
    case 40:
        return 3;
    case 120:
        return 4;
    }

    if (words < MIN_BLOCK_SIZE_WORDS)
        return -1;

    for (int i = SMALL_CLASS_SIZE; i < FREE_LISTS_SIZE; i++)
    {
        if (words < ranges[i])
            return i;
    }

    return FREE_LISTS_SIZE-1;
}

/*
 * Initializes the memory manager.
 */
int mm_init(void)
{
    assert(offsetof(struct block, payload) == 4);
    assert(sizeof(struct boundary_tag) == 4);

    /* Create the initial empty heap */
    struct boundary_tag *initial = mem_sbrk(4 * sizeof(struct boundary_tag));
    if (initial == NULL)
        return -1;

    /* We use a slightly different strategy than suggested in the book.
     * Rather than placing a min-sized prologue block at the beginning
     * of the heap, we simply place two fences.
     * The consequence is that coalesce() must call prev_blk_footer()
     * and not prev_blk() because prev_blk() cannot be called on the
     * left-most block.
     */
    initial[2] = FENCE; /* Prologue footer */
    heap_listp = (struct block *)&initial[3];
    initial[3] = FENCE; /* Epilogue header */
    for (int i = 0; i < FREE_LISTS_SIZE; i++)
    {
        list_init(&(free_lists[i].list));
        free_lists[i].mid = ranges[i] / 2;
    }

    return 0;
}

/**
 * Allocates a block with at least size bytes of payload.
 * 
 * @param size minimum size of bytes to allocate
 * @return pointer to the allocated memory
 */
void *mm_malloc(size_t size)
{
    struct block *bp;

    /* Ignore spurious requests */
    if (size == 0)
        return NULL;

    /* Adjust block size to include overhead and alignment reqs. */
    size_t bsize = align(size + 2 * sizeof(struct boundary_tag)); /* account for tags */
    if (bsize < size)
        return NULL; /* integer overflow */

    /* Adjusted block size in words */
    size_t awords = max(MIN_BLOCK_SIZE_WORDS, bsize / WSIZE); /* respect minimum size */

    /* Search the free list for a fit */
    if ((bp = find_fit(awords)) != NULL)
    {
        place(bp, awords);
        return bp->payload;
    }

    /* No fit found. Get more memory and place the block */
    size_t extendwords = max(awords, CHUNKSIZE); /* Amount to extend heap if no fit */
    if ((bp = extend_heap(extendwords)) == NULL)
        return NULL;

    place(bp, awords);
    return bp->payload;
}

/**
 * Frees the allocated space in memory pointed to by bp.
 * 
 * @param bp pointer to the memory to free
 */
void mm_free(void *bp)
{
    assert(heap_listp != 0); // assert that mm_init was called
    if (bp == 0)
        return;

    /* Find block from user pointer */
    struct block *blk = bp - offsetof(struct block, payload);

    mark_block_free(blk, blk_size(blk));
    coalesce(blk);
}

/**
 * Uses boundary tag coalescing to join adjacent free blocks, if any.
 * 
 * @param bp pointer to the free block to be coalesced
 * @return the [newly sized] free block
 */
static struct block *coalesce(struct block *bp)
{
    bool prev_alloc = prev_blk_footer(bp)->inuse; /* is previous block allocated? */
    bool next_alloc = !blk_free(next_blk(bp));    /* is next block allocated? */
    size_t size = blk_size(bp);

    if (prev_alloc && next_alloc)
    { /* Case 1 */
        // both are allocated, nothing to coalesce
        bool blk_inserted_greater_than_MIN_BLOCK_SIZE_WORDS = insert_free_block((struct free_block *)bp);
        if (!blk_inserted_greater_than_MIN_BLOCK_SIZE_WORDS)
            assert(blk_inserted_greater_than_MIN_BLOCK_SIZE_WORDS);
        return bp;
    }
    else if (prev_alloc && !next_alloc)
    { /* Case 2 */
        remove_free_block((struct free_block *)next_blk(bp));
        // combine this block and next block by extending it
        mark_block_free(bp, size + blk_size(next_blk(bp)));
    }
    else if (!prev_alloc && next_alloc)
    { /* Case 3 */
        remove_free_block((struct free_block *)prev_blk(bp));
        // combine previous and this block by extending previous
        bp = prev_blk(bp);
        mark_block_free(bp, size + blk_size(bp));
    }
    else
    { /* Case 4 */
        remove_free_block((struct free_block *)next_blk(bp));
        remove_free_block((struct free_block *)prev_blk(bp));
        // combine all previous, this, and next block into one
        mark_block_free(prev_blk(bp), size + blk_size(next_blk(bp)) + blk_size(prev_blk(bp)));
        bp = prev_blk(bp);
    }

    bool blk_inserted_greater_than_MIN_BLOCK_SIZE_WORDS = insert_free_block((struct free_block *)bp);
    if (!blk_inserted_greater_than_MIN_BLOCK_SIZE_WORDS)
            assert(blk_inserted_greater_than_MIN_BLOCK_SIZE_WORDS);
    return bp;
}

/**
 * If newrequestedsize is 0, mm_realloc() acts as mm_free().
 * If ptr is NULL, mm_realloc() acts as mm_malloc().
 * 
 * If newblocksize <= oldblocksize, mm_realloc returns the original ptr.
 * Else if the next adjacent block is free and can fit the extra space, mm_realloc
 * uses that extra space for the memory reallocation.
 * Else if the previous adjacent block is free and can fit the extra space, mm_realloc
 * uses that extra space for the memory reallocation.
 * Else if both adjacent blocks are free and can fit the extra space, mm_realloc
 * uses that extra space for the memory reallocation.
 * Else if the block to be reallocated is at the end of heap, extend the heap only
 * for what is needed for the extra space, then use that space for the memory reallocation.
 * Else just use mm_malloc() to find a free block that will fit the newrequestedsize
 * and free the old allocated space.
 *
 * @param ptr to the updated payload location
 * @param newrequestedsize in bytes
 */
void *mm_realloc(void *ptr, size_t newrequestedsize)
{
    /* If size == 0 then this is just free, and we return NULL. */
    if (newrequestedsize == 0)
    {
        mm_free(ptr);
        return 0;
    }

    /* If oldptr is NULL, then this is just malloc. */
    if (ptr == NULL)
    {
        return mm_malloc(newrequestedsize);
    }

    struct block *oldblock = ptr - offsetof(struct block, payload);
    size_t oldblocksize = blk_size(oldblock);                                    // in words
    size_t payloadsize = oldblocksize * WSIZE - 2 * sizeof(struct boundary_tag); /* in bytes */
    /* Adjust block size to include overhead and alignment reqs. */
    size_t newblocksize = align(newrequestedsize + 2 * sizeof(struct boundary_tag)); /* accounts for tags */
    if (newblocksize < newrequestedsize)
        return NULL; /* integer overflow */
    /* Adjusted block size in words */
    newblocksize = max(MIN_BLOCK_SIZE_WORDS, newblocksize / WSIZE); /* respect minimum size */
    bool prev_alloc = prev_blk_footer(oldblock)->inuse;             /* is previous block allocated? */
    bool next_alloc = !blk_free(next_blk(oldblock));                /* is next block allocated? */
    size_t combinedsize;

    if (newblocksize <= oldblocksize)
    { /* Case 0 */
        return ptr;
    }
    // next block is free and can fit the extra space
    else if (prev_alloc && !next_alloc && (combinedsize = oldblocksize + blk_size(next_blk(oldblock))) >= newblocksize)
    { /* Case 2 */
        remove_free_block((struct free_block *)next_blk(oldblock));
        mark_block_used(oldblock, combinedsize);

    }
    // previous block is free and can fit the extra space
    else if (!prev_alloc && next_alloc && (combinedsize = blk_size(prev_blk(oldblock)) + oldblocksize) >= newblocksize)
    { /* Case 1 */
        remove_free_block((struct free_block *)prev_blk(oldblock));
        oldblock = prev_blk(oldblock);
        mark_block_used(oldblock, combinedsize);
        memmove(oldblock->payload, ptr, payloadsize);
    }
    // both adjacent blocks are free and can fit the extra space
    else if (!prev_alloc && !next_alloc && (combinedsize = blk_size(prev_blk(oldblock)) + oldblocksize + blk_size(next_blk(oldblock))) >= newblocksize)
    { /* Case 3 */
        remove_free_block((struct free_block *)prev_blk(oldblock));
        remove_free_block((struct free_block *)next_blk(oldblock));
        oldblock = prev_blk(oldblock);
        mark_block_used(oldblock, combinedsize);
        memmove(oldblock->payload, ptr, payloadsize);
    }
    // the block to be reallocated is at the end of the heap
    else if (blk_size(next_blk(oldblock)) == 0)
    { /* Case 5 */
        struct block *free_block;
        if ((free_block = extend_heap(newblocksize-oldblocksize)) == NULL)
            return NULL;

        remove_free_block((struct free_block *)next_blk(oldblock));
        mark_block_used(oldblock, newblocksize);
    }
    // no combination of adjacent free blocks can satisfy the newly requested size and also not at the end of heap
    else
    { /* Case 4 */
        void *newptr = mm_malloc(newrequestedsize);

        /* If realloc() fails the original block is left untouched  */
        if (!newptr)
        {
            return 0;
        }

        /* Copy the old data. */
        struct block *oldblock = ptr - offsetof(struct block, payload);
        size_t oldpayloadsize = blk_size(oldblock) * WSIZE - 2 * sizeof(struct boundary_tag);
        if (newrequestedsize < oldpayloadsize)
            oldpayloadsize = newrequestedsize;
        memcpy(newptr, ptr, oldpayloadsize);

        /* Free the old block. */
        mm_free(ptr);

        return newptr;
    }

    return oldblock->payload;
}

/* The remaining routines are internal helper routines */

/**
 * Extends the heap size words and coalesces the previous adjacent free block if there was any. Inserts the new free 
 * block into the appropriate free list. And returns the new free block pointer.
 * 
 * @param words the size in words to extend the heap by
 * @return the address to the begof the newly created heap space
 */
static struct block *extend_heap(size_t words)
{
    void *bp = mem_sbrk(words * WSIZE);

    if (bp == NULL)
        return NULL;

    /* Initialize free block header/footer and the epilogue header.
     * Note that we overwrite the previous epilogue here. */
    struct block *blk = bp - sizeof(FENCE);
    mark_block_free(blk, words);
    next_blk(blk)->header = FENCE;

    /* Coalesce if the previous block was free */
    return coalesce(blk);
}

/**
 * Places block of asize words at start of free block bp and split if remainder would be at least MIN_BLOCK_SIZE_WORDS.
 * 
 * @param bp the free block to be used for the allocation
 * @param asize allocation size in words
 */
static void place(struct block *bp, size_t asize)
{
    size_t csize = blk_size(bp); // size of free block in words

    if ((csize - asize) >= MIN_BLOCK_SIZE_WORDS)
    {
        mark_block_used(bp, asize);
        remove_free_block((struct free_block *)bp);
        bp = next_blk(bp); // split the extra free block space as a new smaller free block
        mark_block_free(bp, csize - asize);
        bool blk_inserted_greater_than_MIN_BLOCK_SIZE_WORDS = insert_free_block((struct free_block *)bp);
        if (!blk_inserted_greater_than_MIN_BLOCK_SIZE_WORDS)
            assert(blk_inserted_greater_than_MIN_BLOCK_SIZE_WORDS);
    }
    else
    {
        mark_block_used(bp, csize); // mark the entire free block as used
        remove_free_block((struct free_block *)bp);
    }
}

/**
 * First looks through the smallest free list class size that could accomodate asize, if no block is found after LIMIT,
 * take first available free block from a higher class size.
 * 
 * @param asize allocation size in words
 * @return the free block, NULL if no fit was found
 */
static struct block *find_fit(size_t asize)
{
    int fit_index = find_fit_index(asize);
    int count = 0;
    const int LIMIT = 5;
    
    assert(fit_index != -1); // asize < MIN_BLOCK_SIZE_WORDS
    if (fit_index < 0)
        return NULL;
        
    if (asize < free_lists[fit_index].mid)
    {
        for (struct list_elem *e = list_begin(&(free_lists[fit_index].list)); e != list_end(&(free_lists[fit_index].list)); e = list_next(e)) /* First fit search */
        {
            if (count == LIMIT)
                break;
            struct free_block *current_block = list_entry(e, struct free_block, elem);
            if (blk_size((struct block *)current_block) >= asize)
                return (struct block *)current_block;
            count++;
        }
    }
    else
    {
        for (struct list_elem *e = list_rbegin(&(free_lists[fit_index].list)); e != list_rend(&(free_lists[fit_index].list)); e = list_prev(e)) /* First fit search */
        {
            if (count == LIMIT)
                break;
            struct free_block *current_block = list_entry(e, struct free_block, elem);
            if (blk_size((struct block *)current_block) >= asize)
                return (struct block *)current_block;
            count++;
        }
    }

    for (int i = fit_index+1; i < FREE_LISTS_SIZE; i++)
    {
        count = 0;
        for (struct list_elem *e = list_begin(&(free_lists[i].list)); e != list_end(&(free_lists[i].list)); e = list_next(e)) /* First fit search */
        {
            if (count == LIMIT)
                break;
            struct free_block *current_block = list_entry(e, struct free_block, elem);
            if (blk_size((struct block *)current_block) >= asize)
                return (struct block *)current_block;
            count++;
        }
    }

    return NULL; /* No fit */
}

team_t team = {
    /* Team name */
    "Just commit",
    /* First member's full name */
    "Nathan Damte",
    "nathand@vt.edu",
    /* Second member's full name (leave as empty strings if none) */
    "Dominic Jimeno",
    "dmjimeno0105@vt.edu",
};
