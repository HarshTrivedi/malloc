void mem_init(void);               
void mem_deinit(void);
void *mem_sbrk(intptr_t incr);
void mem_reset_brk(void); 
void *mem_heap_lo(void);
void *mem_heap_hi(void);
size_t mem_heapsize(void);
size_t mem_pagesize(void);
