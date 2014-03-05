/*
* Simple allocator based on implicit list of free blocks
* with boundary tag coalescing. 
* Each block has header and footer of the form:
*
*      31                     3  2  1  0
*      -----------------------------------
*     | s  s  s  s  ... s  s  s  0  0  a/f
*      -----------------------------------
*
* where s are the meaningful size bits and a/f is set
* if the block is allocated. The list has the following form:
*
* begin            Alignment by 8     
* heap     |              List Pointers                    | BLK |  BLK | ...|  epilog  |  /n - null terminator
*  ---------------------------------------------------------------------------------
*  | pad 0 | hdr(24/1)  | prev |  next | Data | ftr(24/1) |  more blocks... | hdr(0/1) |  end of heap
*  ---------------------------------------------------------------------------------
*         listp->| prev | next |                   
*         
* Intial Block Size (minimum) is 24 bytes 
* Explicit list Structure with pointers:
* prev - previous free block
* next - next free block
*                 
* The allocated prologue and epilogue blocks are overhead that
* eliminate edge conditions during coalescing.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mm.h"
#include "memlib.h"

team_t team = {
  "1376931",
  "Kevin Westropp",
  "kevinwestropp@gmail.com",
  "Null",
  "Null"
}; /* so we're compatible with 15213 driver */

/* $begin mallocmacros */
/* Basic constants and macros */
#define WSIZE       4   /* Single word (4 bytes) */ 
#define DSIZE       8   /* Double word (8 bytes) */
#define ALIGNMENT   8   /* Alignment requirement (8 bytes) */
#define OVERHEAD  16    /* Header, Prev, Next, footer (16 bytes in total) */
#define CHUNKSIZE (1<<12) /* ChunkSize of (bytes) */
#define BLKSIZE     4 * WSIZE    /* Mininum Block size (4 words) */

/* rounds up to the nearest multiple of ALIGNMENT */
#define ALIGN(p) (((size_t)(p) + (ALIGNMENT-1)) & ~0x7)

/*Max value of 2 values*/
#define MAX(x, y) ((x) > (y) ? (x) : (y))

/* Pack a size and allocated bit into a word */
#define PACK(size, alloc)  ((size) | (alloc))

/* Read and write a word at address p */
#define GET(p)       (*(int *)(p))
#define PUT(p, val)  (*(int *)(p) = (val))

/* Read the size and allocated fields from address p */
#define SIZE(p) (GET(p) & ~0x7)
#define GET_HSIZE(p)  (GET(HDRP(p)) & ~0x7)
#define GET_HALLOC(p) (GET(HDRP(p)) & 0x1)
#define GET_FSIZE(p)  (GET(FTRP(p)) & ~0x7)
#define GET_FALLOC(p) (GET(FTRP(p)) & 0x1)

/* Given block ptr bp, compute address of its header and footer */
#define HDRP(bp)  ((void *)(bp) - WSIZE)
#define FTRP(bp)  ((void *)(bp) + GET_HSIZE(bp) - DSIZE)

/* Given block ptr bp, compute address of next and previous blocks */
#define NEXT_BLK(bp)  ((void *)(bp) + GET_HSIZE(bp))
#define PREV_BLK(bp)  ((void *)(bp) - SIZE((void *)(bp) - DSIZE))
#define NEXT_PTR(bp)  (*(void **)(bp + WSIZE))
#define PREV_PTR(bp)  (*(void **)(bp))

/* Sets header, footer, prev and next of block */
#define SET_HDRP(bp, val) (PUT(HDRP(bp), (int)val)) 
#define SET_FTRP(bp, val) (PUT(FTRP(bp), (int)val)) 
#define SET_NEXT(bp, qp) (NEXT_PTR(bp) = qp)
#define SET_PREV(bp, qp) (PREV_PTR(bp) = qp)
/* $end mallocmacros */

/* Global variables, heap_listp always points to start of heap */ 
static char *heap_listp = 0; 
/* Global List Pointer = listp
 * listp is so as not to mess too much with the heap pointer,
 */ 
static char *listp = 0;
static int repetitive_values[2] = {0};
/* Function prototypes for internal helper routines */
static void *extend_heap(size_t words);
static void place(void *bp, size_t asize);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void printBlock(void *bp); 
static void checkBlock(void *bp);
static void mm_insert(void *bp); 
static void mm_remove(void *bp); 

static int last_malloced_size = 0;
static int repeat_counter = 0;
// static int is_repeated(int n){
//   if(repetitive_values[0] == n ){
//     repetitive_values[0] = n;
//     return 1;
//   }
//   else{
//       if(repetitive_values[0] == 0 )
//         repetitive_values[0] = n;
//       if(repetitive_values[1] == 0 )
//         repetitive_values[1] = n;
//       return 0;
//   }
// }


/**
 * Initialize the memory manager.
 * @param - void no parameter passed in
 * @return - int 0 for success or -1 for failure
 */
int mm_init(void) {
  
  /*return -1 if unable to get heap space*/
  if ((heap_listp = mem_sbrk(2*BLKSIZE)) == NULL) 
    return -1;

  PUT(heap_listp, 0);                 /* alignment padding */ 
  PUT(heap_listp + (1*WSIZE), PACK(BLKSIZE, 1));    /* prologue header */
  // PUT(heap_listp + (2*WSIZE), 0);           /* prev pointer */
  // PUT(heap_listp + (3*WSIZE), 0);           /* next pointer */
  PUT(heap_listp + BLKSIZE, PACK(BLKSIZE, 1));    /* prologue footer */ 
  PUT(heap_listp+  BLKSIZE + WSIZE, PACK(0, 1));    /* epilogue header */ 
  listp = heap_listp + DSIZE; 

  /* Extend the empty heap with a free block of BLKSIZE bytes */
  if (extend_heap(BLKSIZE) == NULL){ 
    return -1;
  }
  return 0;
}

/**
 * Calculate the adjusted size, asize, to include the header and footer
 * and round up if necessary to satisfy alignment requirement.
 * total size must be a multiple of 8
 * @param - size - requested size (payload only) of a block
 * @return - pointer to allocated block or null if nothing found
 */
void *mm_malloc(size_t size) {
  size_t asize;      /* adjusted block size */
  size_t extendsize; /* amount to extend heap if no fit */
  char *bp;

  /* Ignore spurious requests */
  if (size <= 0){
    return NULL;
  }


  /* Adjust block size to include overhead and alignment reqs */
  asize = MAX(ALIGN(size) + ALIGNMENT, BLKSIZE);

  /* Search the free list for a fit */
  if ((bp = find_fit(asize))) {
    place(bp, asize);
    // last_malloced_size = asize;
    // printf("Last Malloced size updated to %d\n" , last_malloced_size);
    return bp;
   }

  /* No fit found. Get more memory and place the block */
  extendsize = MAX(asize, BLKSIZE);
  /* return NULL if unable to get additional space */
  if ((bp = extend_heap(extendsize)) == NULL) {
    return NULL;
  }
  /* place block and return bp */
  place(bp, asize);
  return bp;
} 

/**
 * Marks this block as free. Calls
 * coalesce to merge with adjacent free blocks
 * if any, then inserts the returned (possibly larger)
 * free block into the tree of free blocks
 * @param - pointer to a block previously allocated
 */
void mm_free(void *bp){
  /* just return if the pointer is NULL */
  if(!bp) return; 
  size_t size = GET_HSIZE(bp);

  /* set this block back to free and coalese */
  SET_HDRP(bp, PACK(size, 0)); 
  SET_FTRP(bp, PACK(size, 0));
  coalesce(bp); 
}

/**
 * Return a block of size >= newsize
 * that preserves all the data from the
 * payload of the block bp.
 * @param - a pointer to an allocated block
 * @param - size - a new size requested
 * @return - pointer to a block size >= newsize
 */
void *mm_realloc(void *bp, size_t size){
  if(size <= 0){ 
    mm_free(bp); 
    return NULL; 
  }else if(bp == NULL){
    bp = mm_malloc(size);
    return bp;
  }
  
  if(size > 0){ 
    size_t currentsize = GET_HSIZE(bp); 
    size_t newsize = ALIGN(size + OVERHEAD); 
    /* newsize is less than currentsize just return bp */
    if(newsize <= currentsize){  
      return bp; 
    } /* newsize is greater than currentsize */ 
    else { 
      size_t next_alloc = GET_HALLOC(NEXT_BLK(bp)); 
      size_t csize;
      size_t asize;     
      /* next block is free and the size of the two blocks is greater than or equal the new size  */ 
      if(!next_alloc && ((csize = currentsize + GET_HSIZE(NEXT_BLK(bp)))) >= newsize){ 
        mm_remove(NEXT_BLK(bp)); 
        SET_HDRP(bp, PACK(csize, 1)); 
        SET_FTRP(bp, PACK(csize, 1)); 
        return bp; 
      } /* next block is free and the block is the last block before the epilogue */
      else if(!next_alloc && ((GET_HSIZE(NEXT_BLK(NEXT_BLK(bp)))) == 0)){
        csize = newsize - currentsize + GET_HSIZE(NEXT_BLK(bp));
        void *temp = extend_heap(csize);
        asize = currentsize + GET_HSIZE(temp);
        SET_HDRP(bp, PACK(asize, 1));
        SET_FTRP(bp, PACK(asize, 1));
        return bp; 
      } /* if bp is the last block before epilogue */
      else if(GET_HSIZE(NEXT_BLK(bp)) == 0){
        csize = newsize - currentsize;
        void *temp = extend_heap(csize);
        asize = currentsize + GET_HSIZE(temp);
        SET_HDRP(bp, PACK(asize, 1));
        SET_FTRP(bp, PACK(asize, 1));
        return bp;
      } /* last ditch attemp try to extend heap for additional size */
      else {  
        void *newbp = mm_malloc(newsize);  
        place(newbp, newsize);
        memcpy(newbp, bp, newsize); 
        mm_free(bp); 
        return newbp; 
      } 
    } /* return NULL */
  }else{ 
    return NULL;
  } 
} 

/** 
 * Check the heap for consistency.
 * HELPER FUNCTION
 * @param - int verbose
 */
void mm_checkheap(int verbose){
  void *bp = heap_listp; 

  if (verbose) {
    printf("Heap (%p):\n", heap_listp);
  }

  if (((GET_HSIZE(heap_listp)) != BLKSIZE) || !GET_HALLOC(heap_listp)){
    printf("Bad prologue header\n");
  }
  checkBlock(heap_listp);

  for (bp = listp; GET_HALLOC(bp) == 0; bp = NEXT_PTR(bp)) 
  {
    if (verbose){ 
      printBlock(bp);
    }
    checkBlock(bp);
  }
  
  if ((GET_HSIZE(bp) != 0) || !(GET_HALLOC(bp))){
    printf("Bad epilogue header\n");
  }
}

/**
 * extend_heap - Extend heap with free block and return its block pointer
 * Allocates a new free block of size which is a multiple of 8 immediately after
 * the last block. Merges this new block with the last block if that block is free.
 * Rewrites an epilog block after the new block.
 * @param - words - extend head by size of words(4)
 * @return - bp - pointer to the new free block (not yet in the tree of free blocks)
 */
static void *extend_heap(size_t words) {
  char *bp;
  size_t size;

  /* Allocate an even number of words to maintain alignment */
  size = (words % 2) ? (words+1) * WSIZE : words * WSIZE;
  if (size < BLKSIZE){
    size = BLKSIZE;
  }
  /* call for more memory space */
  if ((int)(bp = mem_sbrk(size)) == -1){ 
    return NULL;
  }

  /* Initialize free block header/footer and the epilogue header */
  SET_HDRP(bp, PACK(size, 0));         /* free block header */
  SET_FTRP(bp, PACK(size, 0));         /* free block footer */
  SET_HDRP(NEXT_BLK(bp), PACK(0, 1)); /* new epilogue header */
  /* coalesce bp with next and previous blocks */
  return coalesce(bp);
}

/**
 * Place block of asize bytes at start of free block bp
 * and split if remainder would be at least minimum block size
 * @param - bp - pointer to a free block, not on the tree of free blocks
 * @param - asize - the requested size of free block
 */
static void place(void *bp, size_t asize){
  size_t csize = GET_HSIZE(bp);

  if ((csize - asize) >= BLKSIZE) {
    SET_HDRP(bp, PACK(asize, 1));
    SET_FTRP(bp, PACK(asize, 1));
    mm_remove(bp);
    bp = NEXT_BLK(bp);
    SET_HDRP(bp, PACK(csize-asize, 0));
    SET_FTRP(bp, PACK(csize-asize, 0));
    coalesce(bp);
  }
  else {
    SET_HDRP(bp, PACK(csize, 1));
    SET_FTRP(bp, PACK(csize, 1));
    mm_remove(bp);
  }
}

/**
 * Search through the list for free blocks whose size is
 * >= asize (requested size) remove this block from the 
 * list and return it. If no block is found, return null.
 * First Fit Algorithm - Find a first fit for a block with asize bytes
 * @param - asize - a size block from the free list
 * @return bp - pointer to best fit block
 */
static void *find_fit(size_t asize){
  void *bp;

  /* for loop through list to find first fit */
  // This list is of pointers of free blocks only. Hence GET_HALLOC(bp) has to be 0 as long as we are in this list.
  // Instead we can also keep it as NEXT_PTR(bp) != NULL. Decreases efficiency by 1. 
  
  if( last_malloced_size == asize){
      if(repeat_counter>5){  
        bp=mem_sbrk(asize);
        // printf("%x\n" ,bp);
        // printf("Yes\n\n");
        repeat_counter++;
        // printf("repeat_counter : %d\n", repeat_counter);
        if(bp!=NULL){
          /* Initialize free block header/footer and the epilogue header */
            SET_HDRP(bp, PACK(asize, 1));          //free block header 
            SET_FTRP(bp, PACK(asize, 1));         /* free block footer */
            SET_HDRP(NEXT_BLK(bp), PACK(0, 1)); /* new epilogue header */
            /* coalesce bp with next and previous blocks */

            last_malloced_size = asize;
            // printf("Last Malloced size updated to %d\n" , last_malloced_size);

            return coalesce(bp);
        }
      }
      else{
        repeat_counter++;
        // printf("repeat_counter : %d\n", repeat_counter);
      }
  }
  else {
    // printf("No\n\n");
    repeat_counter = 0;
    // printf("repeat_counter : %d\n", repeat_counter);
  }
  for (bp = listp; GET_HALLOC(bp) == 0; bp = NEXT_PTR(bp)) 
  {
    if (asize <= (size_t)GET_HSIZE(bp)){
      last_malloced_size = asize;
      // printf("Last Malloced size updated to %d\n" , last_malloced_size);
      return bp;
    }
  }
  return NULL; /* returns NULL if can't find it in the list */
}

/**
 * coalesce - boundary tag coalescing. Return ptr to coalesced block
 * Removes adjacent blocks from the free list if either one or both are free.
 * Merges block bp with these free adjacent blocks and inserts it onto list.
 * @return - bp - pointer to the merged block 
 */
static void *coalesce(void *bp){

  //if previous block is allocated or its size is zero then prev_alloc will be set.
  size_t prev_alloc = GET_FALLOC(PREV_BLK(bp)) || PREV_BLK(bp) == bp;
  size_t next_alloc = GET_HALLOC(NEXT_BLK(bp));
  size_t size = GET_HSIZE(bp);
  
  /* Case 1 next block is free */   
  if (prev_alloc && !next_alloc) {                  
    size += GET_HSIZE(NEXT_BLK(bp));
    mm_remove(NEXT_BLK(bp));
    SET_HDRP(bp, PACK(size, 0));
    SET_FTRP(bp, PACK(size, 0));
  }/* Case 2 prev block is free */
  else if (!prev_alloc && next_alloc) {               
    size += GET_HSIZE(PREV_BLK(bp));
    bp = PREV_BLK(bp);
    mm_remove(bp);
    SET_HDRP(bp, PACK(size, 0));
    SET_FTRP(bp, PACK(size, 0));
  }/* Case 3 both blocks are free */ 
  else if (!prev_alloc && !next_alloc) {                
    size += GET_HSIZE(PREV_BLK(bp)) + GET_HSIZE(NEXT_BLK(bp));
    mm_remove(PREV_BLK(bp));
    mm_remove(NEXT_BLK(bp));
    bp = PREV_BLK(bp);
    SET_HDRP(bp, PACK(size, 0));
    SET_FTRP(bp, PACK(size, 0));
  }/* lastly insert bp into free list and return bp */
  mm_insert(bp);
  return bp;
}


/**
 * Prints the block payload which 
 * bp is pointing to.
 * @param bp - pointer to block
 */ 
static void printBlock(void *bp){
  size_t hsize = GET_HSIZE(bp);
  size_t halloc = GET_HALLOC(bp);
  size_t fsize = GET_HSIZE(bp);
  size_t falloc = GET_HALLOC(bp);

  if (hsize == 0) 
  {
    printf("%p: EOL\n", bp);
    return;
  }
  if (halloc)
    printf("%p: header:[%d:%c] footer:[%d:%c]\n", bp,
      hsize, (halloc ? 'a' : 'f'),
      fsize, (falloc ? 'a' : 'f'));
  else
    printf("%p:header:[%d:%c] prev:%p next:%p footer:[%d:%c]\n",
      bp, hsize, (halloc ? 'a' : 'f'), 
      PREV_PTR(bp),
      NEXT_PTR(bp), 
      fsize, (falloc ? 'a' : 'f'));
}

/** 
 * Checks the block for proper alignment. 
 * @param bp - pointer to block
 */
static void checkBlock(void *bp)
{
  if ((size_t)bp % 8)
    printf("Error: %p is not doubleword aligned\n", bp);
  if (GET(HDRP(bp)) != GET(FTRP(bp)))
    printf("Error: header does not match footer\n");
}

/**
 * Adds this block to the free list of
 * free blocks.
 * @param - bp - pointer to a block that is already marked free.
 */
static void mm_insert(void *bp){
  /* set bp next to listp */
  SET_NEXT(bp, listp); 
  /* set prev listp to bp */
  SET_PREV(listp, bp); 
  /* set prev bp to NULL */
  SET_PREV(bp, NULL); 
  /* set start of list to bp */
  listp = bp; 
}

/**
 * Removes this block from the list of
 * free blocks.
 * @param - bp - pointer to a block that is on the list of free blocks.
 */
static void mm_remove(void *bp){
  /* if prev bp, then set it's next to bp next */
  if (PREV_PTR(bp)){ 
    SET_NEXT(PREV_PTR(bp), NEXT_PTR(bp));
  }
  else{ /* set listp to bp's next */
    listp = NEXT_PTR(bp);
  }   
  /* set prev of next after bp to prev of bp */
  SET_PREV(NEXT_PTR(bp), PREV_PTR(bp));
}
