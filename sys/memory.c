/*! @file memory.c
    @brief Physical and virtual memory manager
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#include <stdint.h>
#include <sys/cdefs.h>
#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"

#include "conf.h"
#include "console.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "process.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE)  // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE)  // gigapage size

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk *next;  ///< Next page in list
    unsigned long pagecnt;    ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags : 8;
    uint64_t rsw : 2;
    uint64_t ppn : 44;
    uint64_t reserved : 7;
    uint64_t pbmt : 2;
    uint64_t n : 1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2 * 9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1 * 9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0 * 9)) % PTE_CNT)

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) \
    (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//

static void ptab_reset(struct pte *ptab  // page table to reset
);

static struct pte *ptab_clone(struct pte *ptab  // page table to clone
);

static void ptab_discard(struct pte *ptab  // page table to discard
);

static void ptab_insert(struct pte *ptab,   // page table to modify
                        unsigned long vpn,  // virtual page number to insert
                        void *pp,           // pointer to physical page to insert
                        int rwxug_flags     // flags for inserted mapping
);

static void *ptab_remove(struct pte *ptab, unsigned long vpn);

static void ptab_adjust(struct pte *ptab, unsigned long vpn, int rwxug_flags);

struct pte *ptab_fetch(struct pte *ptab, unsigned long vpn);

static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte *root, unsigned int asid);
static inline struct pte *mtag_to_ptab(mtag_t mtag);
static inline struct pte *active_space_ptab(void);

static inline void *pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void *p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT] __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk *free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
//

void memory_init(void) {
    const void *const text_start = _kimg_text_start;
    const void *const text_end = _kimg_text_end;
    const void *const rodata_start = _kimg_rodata_start;
    const void *const rodata_end = _kimg_rodata_end;
    const void *const data_start = _kimg_data_start;

    void *heap_start;
    void *heap_end;

    uintptr_t pma;
    const void *pp;

    trace("%s()", __func__);

    assert(RAM_START == _kimg_start);

    debug("           RAM: [%p,%p): %zu MB", RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    debug("  Kernel image: [%p,%p)", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)

    if (MEGA_SIZE < _kimg_end - _kimg_start) panic(NULL);

    // Initialize main page table with the following direct mapping:
    //
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB

    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void *)pma, PTE_R | PTE_W | PTE_G);

    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void *)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP(HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end) panic("out of memory");

    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    debug("Heap allocator: [%p,%p): %zu KB free", heap_start, heap_end,
          (heap_end - heap_start) / 1024);

    // FIXME: Initialize the free chunk list here

    struct page_chunk * init_chunk = __align_up(heap_end, PAGE_SIZE); // pointer to contiguous chunk of free physical memory

    init_chunk->pagecnt = (RAM_END - (void*)init_chunk) / PAGE_SIZE; // page count is # of pages between ram end and init chunk 
    init_chunk->next = NULL; // no next poniter, chunk is currently entire physical space

    free_chunk_list = init_chunk; // head of free chunk list is initial chunk

    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}


mtag_t active_mspace(void) { return active_space_mtag(); }

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;

    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

// mtag_t clone_active_mspace(void)
// Inputs: None
// Outputs: mtag of clones memory space
// Description: Gets the current memory space and clones the page tables and pages 
// Side Effects: None 
mtag_t clone_active_mspace(void) {
    // FIXME
    mtag_t cur = active_space_mtag(); // get current memory space tag (SATP value)
    struct pte * lvl_2 = mtag_to_ptab(cur); // get pointer to start of level 2 (root) table

    if(!lvl_2){
        return -EBADFMT;
    }

    struct pte * new_root = alloc_phys_page(); // allocate physical memory for a new root page

    if(!new_root){ // check if alloc fails
        return -ENOMEM;
    }

    memset(new_root, 0, PAGE_SIZE); // clear space 
    
    
    for(unsigned lv2 = 0; lv2 < PTE_CNT; lv2++){ // iterate through level two page
        struct pte cur_pte = lvl_2[lv2];
        if(!PTE_VALID(cur_pte)){ // skip if invalid PTE
            continue;
        }
        
        // If current page table entry is a leaf:
        // pte contains physical address of data we want
        // allocate a new page -> get the data from the current leaf -> copy contents of exisitng leaf page 
        // to new leaf page -> create pte entry for new leaf and put it in new root table
        if(PTE_LEAF(cur_pte)){ 
            
            // shallow copy if global 
            if(PTE_GLOBAL(cur_pte)){
                new_root[lv2] = leaf_pte(pageptr(cur_pte.ppn), cur_pte.flags);
                continue;
            }

            // deep copy if not global
            struct pte * new_leaf = alloc_phys_page();

            if(!new_leaf){ // check if alloc fails
                return -ENOMEM;
            }

            void * cur_ptr = pageptr(cur_pte.ppn); 
            memcpy(new_leaf, cur_ptr, PAGE_SIZE);
            new_root[lv2] = leaf_pte(new_leaf, cur_pte.flags); 
            continue;
        }

        // If current pte is not a leaf, it points to a level 1 page 
        // Allocate a new level 1 page -> put new level 1 page entry in root table -> get old level 1 page as reference 
        struct pte * new_lvl_1 = alloc_phys_page();

        if(!new_lvl_1){ // check if alloc fails
            return -ENOMEM;
        }

        memset(new_lvl_1, 0, PAGE_SIZE); // clear space 

        new_root[lv2] = ptab_pte(new_lvl_1, cur_pte.flags & PTE_G);
        struct pte * lvl_1 = pageptr(lvl_2[lv2].ppn); 
        
        for(unsigned lv1 = 0; lv1 < PTE_CNT; lv1++){
            cur_pte = lvl_1[lv1];
            if(!PTE_VALID(cur_pte)){ // skip if invalid PTE
                continue;
            }

            // If leaf, exact same as lv2 situation
            if(PTE_LEAF(cur_pte)){ 
                
                // shallow copy if global 
                if(PTE_GLOBAL(cur_pte)){
                    new_lvl_1[lv1] = leaf_pte(pageptr(cur_pte.ppn), cur_pte.flags);
                    continue;
                }

                // deep copy if not global
                struct pte * new_leaf = alloc_phys_page();

                if(!new_leaf){ // check if alloc fails
                    return -ENOMEM;
                }

                void * cur_ptr = pageptr(cur_pte.ppn); 
                memcpy(new_leaf, cur_ptr, PAGE_SIZE);
                new_lvl_1[lv1] = leaf_pte(new_leaf, cur_pte.flags); 
                continue;
            }

            // Not leaf, entry is now last level (0)
            // Iterate and initialize exactly like level 1
            struct pte * new_lvl_0 = alloc_phys_page();

            if(!new_lvl_0){ // check if alloc fails
                return -ENOMEM;
            }

            memset(new_lvl_0, 0, PAGE_SIZE); // clear space
            new_lvl_1[lv1] = ptab_pte(new_lvl_0, cur_pte.flags & PTE_G);
            struct pte * lvl_0 = pageptr(lvl_1[lv1].ppn); 

            for(unsigned lv0 = 0; lv0 < PTE_CNT; lv0++){
                // HAS to be a leaf -> do same as above:
                struct pte cur_pte = lvl_0[lv0];
                if(!PTE_VALID(cur_pte)){ // skip if invalid PTE
                    continue;
                }

                // shallow copy if global 
                if(PTE_GLOBAL(cur_pte)){
                    new_lvl_0[lv0] = leaf_pte(pageptr(cur_pte.ppn), cur_pte.flags);
                    continue;
                }

                // deep copy if not global
                struct pte * new_leaf = alloc_phys_page();

                if(!new_leaf){ // check if alloc fails
                    return -ENOMEM;
                }

                void * cur_ptr = pageptr(cur_pte.ppn); 
                memcpy(new_leaf, cur_ptr, PAGE_SIZE);
                new_lvl_0[lv0] = leaf_pte(new_leaf, cur_pte.flags); 
            }
        }
    }

    mtag_t toRet = ptab_to_mtag(new_root, 0); // create new mtag and return
    return toRet;
}

// void reset_active_mspace(void)
// Inputs: None
// Outputs: None
// Description: Resets current memspace -> frees and unmaps all pages that don't have global flag
// Side Effects: None 
void reset_active_mspace(void) {
    // FIXME
    mtag_t cur = active_space_mtag(); // get current memory space tag (SATP value)
    struct pte * lvl_2 = mtag_to_ptab(cur); // get pointer to start of level 2 (root) table

    if(!lvl_2){ // return if table DNE
        return;
    }
    
    for(unsigned lv2 = 0; lv2 < PTE_CNT; lv2++){ // iterate through level two page
        struct pte cur_pte = lvl_2[lv2];
        if(!PTE_VALID(cur_pte)){ // skip if invalid PTE
            continue;
        }
        
        // If current page table entry is a leaf:
        // pte contains physical address of data we want
        // If leaf global flag is set to zero -> free page and unmap in table
        if(PTE_LEAF(cur_pte)){ 
            void * cur_ptr = pageptr(cur_pte.ppn);
            if(!(PTE_GLOBAL(lvl_2[lv2]))){
                free_phys_page(cur_ptr);
                lvl_2[lv2] = null_pte();
            } 
            continue;
        }

        // If current pte is not a leaf, it points to a level 1 page 
        struct pte * lvl_1 = pageptr(lvl_2[lv2].ppn); 
        
        for(unsigned lv1 = 0; lv1 < PTE_CNT; lv1++){
            cur_pte = lvl_1[lv1];
            if(!PTE_VALID(cur_pte)){ // skip if invalid PTE
                continue;
            }

            // If leaf, exact same as lv2 situation
            if(PTE_LEAF(cur_pte)){ 
                void * cur_ptr = pageptr(cur_pte.ppn);
                if(!(PTE_GLOBAL(lvl_1[lv1]))){
                    free_phys_page(cur_ptr);
                    lvl_1[lv1] = null_pte();
                } 
                continue;
            }

            // Not leaf, entry is now last level (0)
            struct pte * lvl_0 = pageptr(lvl_1[lv1].ppn); 

            // Iterate through lvl 0
            for(unsigned lv0 = 0; lv0 < PTE_CNT; lv0++){ 
                // HAS to be a leaf -> do same as above:
                struct pte cur_pte = lvl_0[lv0];

                void * cur_ptr = pageptr(cur_pte.ppn);
                if(!(PTE_GLOBAL(lvl_0[lv0]))){
                    free_phys_page(cur_ptr);
                    lvl_0[lv0] = null_pte();
                } 
            }

            // reset entire table if not global
            if(!(PTE_GLOBAL(lvl_1[lv1]))){
                free_phys_page(lvl_0);
                lvl_1[lv1] = null_pte();
            }
        }
        // reset entire table if not global
        if(!(PTE_GLOBAL(lvl_2[lv2]))){
            free_phys_page(lvl_1);
            lvl_2[lv2] = null_pte();
        }
    }

    sfence_vma(); // changed vitual memory, flush

    return;
}

// mtag_t discard_active_mspace(void)
// Inputs: None
// Outputs: Tag corresponding to main memory space
// Description: Switches memory spaces to main, unmaps and frees all non-global pages from the previously active memory space
// Side Effects: None
mtag_t discard_active_mspace(void) {
    // FIXME
    reset_active_mspace(); // unmaps and frees all non-global pages from prev (current rn) active space 

    mtag_t main = switch_mspace(main_mtag); // switches to the main memory space and gets the main MTAG

    return main;
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

// void *map_page(uintptr_t vma, void *pp, int rwxug_flags)
// Inputs: uintptr_t vma - vitual memory address
//         void *pp - pointer to physical page
//         int rwxug_flags - flags to set
// Outputs: Virtual memory address that was mapped
// Description: Adds page with provided virtual memory address and flags to page table
// Side Effects: None
void *map_page(uintptr_t vma, void *pp, int rwxug_flags) {
    // FIXME
    if(!pp){ // input checks 
        return NULL;
    }

    if(!wellformed(vma)){ // return if bad VMA
        return NULL;
    }

    // if(!(vma % PAGE_SIZE == 0)){ // return if vma is not PAGE_SIZE increment
    //     return NULL;
    // }

    mtag_t cur = active_space_mtag(); // get current memory space tag (SATP value)
    struct pte * lvl_2 = mtag_to_ptab(cur); // get pointer to start of level 2 (root) table

    //init
    struct pte * lvl_1;
    struct pte * lvl_0;

    if(!lvl_2){ // return if table DNE
        return NULL;
    }

    // We need to find correct address in page table -> use VMA
    // VPN 2 -> index into lvl 2 page table | VPN 1 -> index into level 1 page table |
    // VPN 0 -> index into level 0 table
    // Then, set level zero pte to values passed in (ppn and flags)

    struct pte cur_pte = lvl_2[VPN2(vma)];

    // If level 1 table is not allocated yet, we need to allocate one!
    // If level 1 table exists, just get physical address from ppn
    if(!PTE_VALID(cur_pte)){
        lvl_1 = alloc_phys_page();
        if(!lvl_1){
            return NULL;
        }
        memset(lvl_1, 0, PAGE_SIZE);
        lvl_2[VPN2(vma)] = ptab_pte(lvl_1, 0);
    }
    else{
        lvl_1 = pageptr(cur_pte.ppn);
    }
    
    // Same ordeal as level 1
    cur_pte = lvl_1[VPN1(vma)];
    if(!PTE_VALID(cur_pte)){
        lvl_0 = alloc_phys_page();
        if(!lvl_0){
            return NULL;
        }
        memset(lvl_0, 0, PAGE_SIZE);
        lvl_1[VPN1(vma)] = ptab_pte(lvl_0, 0);
    }
    else{
        lvl_0 = pageptr(cur_pte.ppn);
    }
    
    // When at root entry, create a leaf with args matching the ones passed in
    lvl_0[VPN0(vma)] = leaf_pte(pp, rwxug_flags);

    sfence_vma(); // we updated virtual memory, so lets flush the cache 

    return (void*)vma;
}

// void *map_range(uintptr_t vma, size_t size, void *pp, int rwxug_flags)
// Inputs: uintptr_t vma - vitual memory address
//         size_t size - size of range to allocate 
//         void *pp - pointer to physical page
//         int rwxug_flags - flags to set
// Outputs: Nothing
// Description: Adds a range of contiguous pages with provided virtual memory address, size, and flags to page table
// Side Effects: None
void *map_range(uintptr_t vma, size_t size, void *pp, int rwxug_flags) {
    // FIXME

    // input checks 
    if(!wellformed(vma) || !pp || size <=0){ 
        return NULL;
    }

    // Initializations:
    // We will be changing VMA and PP, so just make new vars
    uintptr_t running_vma = vma;
    void * running_pp = pp; // lol
    size_t num_pages = (size+PAGE_SIZE-1)/PAGE_SIZE; // round up to next page 

    // Pretty simple for this function, just call map_page for num pages,
    // just remeber to update vma and pp to +PAGE_SIZE every time we go to a new page 
    for(unsigned page = 0; page < num_pages; page++){
        map_page(running_vma, running_pp, rwxug_flags);
        running_vma += PAGE_SIZE;
        running_pp = (void*)((uintptr_t)running_pp + PAGE_SIZE); // cast so it adds 512 BYTES 
    }
    
    return (void*)vma;
}

// void *alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags)
// Inputs: uintptr_t vma - vitual memory address
//         size_t size - size of range to allocate 
//         int rwxug_flags - flags to set
// Outputs: Nothing
// Description: Allocates memory for and maps a range of pages starting at provided virtual memory address.
//              Rounds up size to be a multiple of PAGE_SIZE
// Side Effects: None
void *alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    // FIXME
    
    if(!wellformed(vma) || size <= 0){
        return NULL;
    }

    // Init
    void * pp;
    size_t num_pages = (size + PAGE_SIZE-1)/PAGE_SIZE;
    

    // Pretty much the same as map_range, we just have to initialize pp ourselves
    // Create pp pointer -> allocate multiple pages (rounded up size) -> call map_range
    pp = alloc_phys_pages(num_pages);

    return map_range(vma, num_pages*PAGE_SIZE, pp, rwxug_flags);
}

// void set_range_flags(const void *vp, size_t size, int rwxug_flags)
// Inputs: const void *vp - virtual memory address
//         size_t size - size of range to set
//         int rwxug_flags - flags to set
// Outputs: Nothing
// Description: Sets passed flags for pages in range. Rounds up size to be a multiple of PAGE_SIZE
// Side Effects: None
void set_range_flags(const void *vp, size_t size, int rwxug_flags) {
    // FIXME

    if(!vp || size <= 0){ // input checks 
        return;
    }

    // init
    size_t num_pages = (size + PAGE_SIZE-1)/PAGE_SIZE; // rounded up
    uintptr_t running_vma = (uintptr_t)vp; // get changeable vp
    mtag_t cur = active_space_mtag(); // get current memory space tag (SATP value)
    struct pte * lvl_2 = mtag_to_ptab(cur); // get pointer to start of level 2 (root) table
    struct pte * lvl_1;
    struct pte * lvl_0;

    if(!((running_vma % PAGE_SIZE) == 0)){
        return;
    }

    if(!lvl_2){ // return if table DNE
        return;
    }

    // We will assume PTE and tables exist for this one
    // For page in range num pages -> go through page tables using VPN 0, 1, ... -> set flags
    
    for(unsigned page = 0; page < num_pages; page++){
        
        // "Iterate" down the tables -> set flags -> update running VMA 
        struct pte cur_pte = lvl_2[VPN2(running_vma)];
        if(!PTE_VALID(cur_pte)){
            return;
        }
        lvl_1 = pageptr(cur_pte.ppn);
        cur_pte = lvl_1[VPN1(running_vma)];
        if(!PTE_VALID(cur_pte)){
            return;
        }
        lvl_0 = pageptr(cur_pte.ppn);
        
        lvl_0[VPN0(running_vma)].flags = rwxug_flags;

        running_vma += PAGE_SIZE;
    }

    return;
}

// void unmap_and_free_range(void *vp, size_t size)
// Inputs: const void *vp - virtual memory address
//         size_t size - size of range to unmap
// Outputs: Nothing
// Description: Unmaps a range of pages starting at provided virtual memory address and frees the pages. 
//              Rounds up size to be a multiple of PAGE_SIZE
// Side Effects: None
void unmap_and_free_range(void *vp, size_t size) {
    // FIXME
    
    if(!vp || size <= 0){ // input checks 
        return;
    }

    // init
    size_t num_pages = (size + PAGE_SIZE-1)/PAGE_SIZE; // rounded up
    uintptr_t running_vma = (uintptr_t)vp; // get changeable vp
    mtag_t cur = active_space_mtag(); // get current memory space tag (SATP value)
    struct pte * lvl_2 = mtag_to_ptab(cur); // get pointer to start of level 2 (root) table
    struct pte * lvl_1;
    struct pte * lvl_0;
    struct pte * last_page;
    unsigned vpn2;
    unsigned vpn1;

    if(!((running_vma % PAGE_SIZE) == 0)){ // vma must be PAGE_SIZE increment 
        return;
    }

    if(!lvl_2){ // return if table DNE
        return;
    }


    // Main idea: free the leaf corresponding to address, then free entire table if past end 
    for(unsigned page = 0; page < num_pages; page ++){

        // In this section, we strictly go down to the leaf -
        // lvl_2 -> lvl_1 -> lvl_0 -> leaf
        // and free the leaf 
        vpn1 = VPN1(running_vma);
        vpn2 = VPN2(running_vma);
        struct pte cur_pte = lvl_2[VPN2(running_vma)];
        if(!PTE_VALID(cur_pte)){
            continue;
        }
        lvl_1 = pageptr(cur_pte.ppn); // pointer to level 1 table
        cur_pte = lvl_1[VPN1(running_vma)];
        if(!PTE_VALID(cur_pte)){
            running_vma += PAGE_SIZE;
            continue;
        }
        lvl_0 = pageptr(cur_pte.ppn); // pointer to level 0 table
        cur_pte = lvl_0[VPN0(running_vma)];
        if(!PTE_VALID(cur_pte)){
            running_vma += PAGE_SIZE;
            continue;
        }
        last_page = pageptr(cur_pte.ppn); // pointer to leaf page 

        lvl_0[VPN0(running_vma)] = null_pte();
        free_phys_page(last_page);

        running_vma += PAGE_SIZE;

        // Now we need to free some tables (maybe)
        // Idea: If we have gone to a new table, the VPN (index into table) changes
        // so, we check if VPN has changed for the level 1 and 2 tables
        // If it has, free the table itself :)
        if(VPN1(running_vma) != vpn1){
            if(!(PTE_GLOBAL(lvl_1[vpn1]))){
                free_phys_page(pageptr(lvl_1[vpn1].ppn));
                lvl_1[vpn1] = null_pte();
            }
        }
        if(VPN2(running_vma) != vpn2){
            if(!(PTE_GLOBAL(lvl_2[vpn2]))){
                free_phys_page(pageptr(lvl_2[vpn2].ppn));
                lvl_2[vpn2] = null_pte();
            }
        }
    }

    // Free tables at the end (if needed)
    if(VPN1(running_vma) != vpn1){
            if(!(PTE_GLOBAL(lvl_1[vpn1]))){
                free_phys_page(pageptr(lvl_1[vpn1].ppn));
                lvl_1[vpn1] = null_pte();
            }
        }
        if(VPN2(running_vma) != vpn2){
            if(!(PTE_GLOBAL(lvl_2[vpn2]))){
                free_phys_page(pageptr(lvl_2[vpn2].ppn));
                lvl_2[vpn2] = null_pte();
            }
        }

    sfence_vma(); // changed virtual memory, need to flush

    return;
}

// int validate_vptr(const void *vp, size_t len, int rwxug_flags)
// Inputs: const void *vp - virtual memory address
//         size_t len - size (in bytes) of range
//         int rwxug_flags - flags to check 
// Outputs: 0 on success; error on malformed pointer, unmapped page, or mismatching flags
// Description: Checks that vp is wellformed, then checks that the range is mapped to the correct flags 
// Side Effects: None
int validate_vptr(const void *vp, size_t len, int rwxu_flags) {
    // FIXME

    if(vp == NULL || len < 0){ // check inputs 
        return -EINVAL;
    }

    // init
    size_t num_pages = (len + PAGE_SIZE-1)/PAGE_SIZE; // rounded up
    uintptr_t running_vma = (uintptr_t)vp; // get changeable vp
    mtag_t cur = active_space_mtag(); // get current memory space tag (SATP value)
    struct pte * lvl_2 = mtag_to_ptab(cur); // get pointer to start of level 2 (root) table
    struct pte * lvl_1;
    struct pte * lvl_0;
    
    if(!wellformed(running_vma)){ // check that VMA is valid
        return -EINVAL;
    }

    if(running_vma + len < running_vma || running_vma + len > UMEM_END_VMA){ // check if there is wraparound
        return -EINVAL;
    }

    // Start at lvl 2 -> iterate down -> check that each entry is valid on the way down
    // -> find page in physical memory -> check if page's flags match input -> return errors if needed 
    for(unsigned page = 0; page < num_pages; page ++){

        struct pte cur_pte = lvl_2[VPN2(running_vma)];
        if(!PTE_VALID(cur_pte) || PTE_LEAF(cur_pte)){
            return -EBADFMT;
        }
        lvl_1 = pageptr(cur_pte.ppn); // pointer to level 1 table
        cur_pte = lvl_1[VPN1(running_vma)];
        if(!PTE_VALID(cur_pte) || PTE_LEAF(cur_pte)){
            return -EBADFMT;
        }
        lvl_0 = pageptr(cur_pte.ppn); // pointer to level 0 table
        cur_pte = lvl_0[VPN0(running_vma)];
        if(!PTE_VALID(cur_pte)){
            return -EBADFMT;
        }

        if((cur_pte.flags & rwxu_flags) != rwxu_flags){ // check flags
            return -EBADFMT;
        }

        running_vma += PAGE_SIZE;
    }
   
    return 0;
}

// int validate_vstr(const char *vs, int rug_flags)
// Inputs: const void *vs - virtual memory address of string 
//         int rug_flags - flags to check 
// Outputs: 0 on success; error on malformed pointer, unmapped page, or mismatching flags
// Description: Checks that vs is wellformed, then checks that the given string is valid
// Side Effects: None
int validate_vstr(const char *vs, int rug_flags) {
    // FIXME

    if(vs == NULL){ // check inputs 
        return -EINVAL;
    }

    // init
    uintptr_t running_vma = (uintptr_t)vs; // get changeable vp
    mtag_t cur = active_space_mtag(); // get current memory space tag (SATP value)
    struct pte * lvl_2 = mtag_to_ptab(cur); // get pointer to start of level 2 (root) table
    struct pte * lvl_1;
    struct pte * lvl_0;
    char * last_page;
    char val_in_page;

    if(!wellformed(running_vma)){ // check that VMA is valid
        return -EINVAL;
    }

    // Descend the table page level by page level -> check for invalid ptes -> 
    // when leaf page is reached, check flags -> descend into the physical page ->
    // check if the character is terminator -> if it is, break -> if not, keep iterating 
    while(1){

        if(running_vma > UMEM_END_VMA){ // end of memeory check 
            return -ENOMEM;
        }

        struct pte cur_pte = lvl_2[VPN2(running_vma)];
        if(!PTE_VALID(cur_pte) || PTE_LEAF(cur_pte)){
            return -EBADFMT;
        }
        lvl_1 = pageptr(cur_pte.ppn); // pointer to level 1 table
        cur_pte = lvl_1[VPN1(running_vma)];
        if(!PTE_VALID(cur_pte) || PTE_LEAF(cur_pte)){
            return -EBADFMT;
        }
        lvl_0 = pageptr(cur_pte.ppn); // pointer to level 0 table
        cur_pte = lvl_0[VPN0(running_vma)];
        if(!PTE_VALID(cur_pte)){
            return -EBADFMT;
        }

        // if(cur_pte.flags != rug_flags){ // check flags
        //     return -EBADFMT;
        // }

        last_page = pageptr(cur_pte.ppn); // pointer to phys page
        val_in_page = last_page[running_vma & 0xFFF];

        if(val_in_page == '\0'){ // check if null terminator 
            break;
        }

        running_vma += 1; // next BYTE ??
    }

    return 0;
}

// void *alloc_phys_page(void)
// Inputs: None
// Outputs: Pointer to allocated page
// Description: Goes through the free chunk list and finds and returns a page 
// Side Effects: None
void *alloc_phys_page(void) {
    // FIXME
    return alloc_phys_pages(1);
}

// void free_phys_page(void *pp)
// Inputs: void *pp - Pointer to page to free 
// Outputs: None
// Description: Returns a physical page to the free chunk list 
// Side Effects: None
void free_phys_page(void *pp) {
    // FIXME
    return free_phys_pages(pp, 1);
}

// void *alloc_phys_pages(unsigned int cnt)
// Inputs: unsigned int cnt - # of pages to allocate 
// Outputs: Pointer to allocated pages 
// Description: Goes through the free chunk list and finds and returns a chunk of pages 
// Side Effects: None
void *alloc_phys_pages(unsigned int cnt) {
    // FIXME

    if(cnt <= 0){ // check input
        return NULL;
    }

    // initializations
    struct page_chunk * cur;
    struct page_chunk * prev;
    struct page_chunk * least_prev;
    struct page_chunk * least_page;
    least_prev = NULL;
    prev = NULL;
    cur = free_chunk_list;
    least_page = NULL;

    // iterate through page chunk list and find the page chunk with the least # of entries
    while(cur != NULL){
        if(least_page == NULL && cur->pagecnt >= cnt){
            least_page = cur;
            least_prev = prev;
        }
        else if(cur->pagecnt < least_page->pagecnt && cur->pagecnt >= cnt){
            least_page = cur;
            least_prev = prev;
        }
        prev = cur;
        cur = cur->next;
    }

    // if we didn't find a page chunk that has enough space, return NULL
    if(least_page == NULL){
        return NULL;
    }

    // split off the page chunk in memeory 
    // if page chunk is head of list, move head to correct position and return head 
    // if not head, remove page chunk from list 
    if(least_page == free_chunk_list){
        struct page_chunk * head_next = free_chunk_list->next;
        free_chunk_list = (struct page_chunk*)((char *)free_chunk_list + PAGE_SIZE*cnt); // cast to get correct ptr arithmetic 
        free_chunk_list->pagecnt = least_page->pagecnt - cnt; // update metadata 
        free_chunk_list->next = head_next;
        least_page->next = NULL;
        least_page->pagecnt = cnt;
        return least_page;
    }
    else if(least_page->pagecnt > cnt){
        struct page_chunk * new_chunk = (struct page_chunk*)((char*)least_page + PAGE_SIZE*cnt); // split off new chunk
        least_prev->next = new_chunk; // update metadata 
        new_chunk->next = least_page->next;
        new_chunk->pagecnt = least_page->pagecnt - cnt;
        least_page->next = NULL;
        least_page->pagecnt = cnt;
        return least_page;
    }
    else{
        least_prev->next = least_page->next;
        least_page->next = NULL;
        least_page->pagecnt = cnt;
        return least_page;
    }

    return NULL;
}

// void free_phys_pages(void *pp, unsigned int cnt)
// Inputs: void *pp - Pointer to page to free 
//         unsigned int cnt - Number of pages to free
// Outputs: None
// Description: Returns a chunk of physical pages to the free chunk list 
// Side Effects: None
void free_phys_pages(void *pp, unsigned int cnt) {
    // FIXME

    if(pp == NULL || cnt <= 0){ // check input
        return;
    }

    struct page_chunk * add_this = (struct page_chunk *)pp; // cast physical pointer to paage chunk (metadata + free memory)
    
    add_this->pagecnt = cnt; // update the page chunk 
    add_this->next = free_chunk_list; 
    
    free_chunk_list = add_this; // place chunk at head of list 
    
    return;
}

// unsigned long free_phys_page_count(void)
// Inputs: None
// Outputs: Number of free pages in the list 
// Description: Loops through the free chunk list and counts the number of pages 
// Side Effects: None
unsigned long free_phys_page_count(void) {
    // FIXME
    
    //init
    int total = 0;
    struct page_chunk * cur = free_chunk_list;

    // iterate through free_chunk_list and count each pagecnt 
    while(cur != NULL){
        total += cur->pagecnt;
        cur = cur->next;
    }

    return total;
}

// int handle_umode_page_fault(struct trap_frame *tfr, uintptr_t vma)
// Inputs: struct trap_frame *tfr - trap frame (unused)
//         uintptr_t vma - address where fault occured 
// Outputs: 1 if map was successful, 0 otherwise 
// Description: Calls map_page to map a page to vma (lazy allocation)
// Side Effects: None
int handle_umode_page_fault(struct trap_frame *tfr, uintptr_t vma) {
    // FIXME

    if(!wellformed(vma)){ // check input
        return 0;
    }

    void *pp = alloc_phys_page(); // get a physical page 

    if(pp == NULL){ // if allocation fails, return 
        return 0;
    }

    int flags = PTE_R | PTE_U | PTE_W | PTE_V; // set read, write, global, and user flags 

    void *retval = map_page(vma, pp, flags); // map a page to vma 

    if(retval == NULL){ // if page wasn't mapped 
        return 0;
    }

    return 1; 
}

/**
 * @brief Reads satp to retrieve tag for active memory space
 * @return Tag for active memory space
 */
mtag_t active_space_mtag(void) { return csrr_satp(); }

/**
 * @brief Constructs tag from page table address and address space identifier
 * @param ptab Pointer to page table to use in tag
 * @param asid Address space identifier to use in tag
 * @return Memory tag formed from paging mode, page table address, and ASID
 */
static inline mtag_t ptab_to_mtag(struct pte *ptab, unsigned int asid) {
    return (((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
            ((unsigned long)asid << RISCV_SATP_ASID_shift) | pagenum(ptab) << RISCV_SATP_PPN_shift);
}

/**
 * @brief Retrives a page table address from a tag
 * @param mtag Tag to extract page table address from
 * @return Pointer to page table retrieved from tag
 */
static inline struct pte *mtag_to_ptab(mtag_t mtag) { return (struct pte *)((mtag << 20) >> 8); }

/**
 * @brief Returns the address of the page table corresponding to the active memory space
 * @return Pointer to page table extracted from active memory space tag
 */
static inline struct pte *active_space_ptab(void) { return mtag_to_ptab(active_space_mtag()); }

/**
 * @brief Constructs a physical pointer from a physical page number
 * @param n Physical page number to derive physical pointer from
 * @return Pointer to memory corresponding to physical page
 */
static inline void *pageptr(uintptr_t n) { return (void *)(n << PAGE_ORDER); }

/**
 * @brief Constructs a physical page number from a pointer
 * @param p Pointer to derive physical page number from
 * @return Physical page number corresponding to pointer
 */
static inline unsigned long pagenum(const void *p) { return (unsigned long)p >> PAGE_ORDER; }

/**
 * @brief Checks if bits 63:38 of passed virtual memory address are all 1 or all 0
 * @param vma Virtual memory address to check well-formedness of
 * @return 1 if pointer is well-formed, 0 otherwise
 */
static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits + 1));
}

/**
 * @brief Constructs a page table entry corresponding to a leaf
 * @details For our purposes, a leaf PTE has the A, D, and V flags set
 * @param pp Physical address to set physical page number of PTE from
 * @param rwxug_flags Flags to set on PTE
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags) {
    return (struct pte){.flags = rwxug_flags | PTE_A | PTE_D | PTE_V, .ppn = pagenum(pp)};
}

/**
 * @brief Constructs a page table entry corresponding to a page table
 * @param pt Physical address to set physical page number of PTE from
 * @param g_flag Flags to set on PTE (should either be G flag or nothing)
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag) {
    return (struct pte){.flags = g_flag | PTE_V, .ppn = pagenum(pt)};
}

/**
 * @brief Returns an empty pte
 * @return An empty pte
 */
static inline struct pte null_pte(void) { return (struct pte){}; }

struct page_chunk * list_head(){
    return free_chunk_list;
}