/*! @file process.c
    @brief user process
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

/*!
 * @brief Enables trace messages for process.c
 */
#include <stdint.h>
#include <string.h>
// #include <sys/errno.h>
#ifdef PROCESS_TRACE
#define TRACE
#endif

/*!
 * @brief Enables debug messages for process.c
 */
#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "process.h"
#include "string.h"
#include "uioimpl.h"

#include "conf.h"
#include "elf.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"
#include "trap.h"
#include "uio.h"
#include "console.h"

// COMPILE-TIME PARAMETERS
//

/*!
 * @brief Maximum number of processes
 */
#ifndef NPROC
#define NPROC 16
#endif

// INTERNAL FUNCTION DECLARATIONS
//

static int build_stack(void* stack, int argc, char** argv);

static void fork_func(struct condition* forked, struct trap_frame* tfr);

// INTERNAL GLOBAL VARIABLES
//

#define TP ((struct thread*)__builtin_thread_pointer())

/*!
 * @brief The main user process struct
 */
static struct process main_proc;

static struct process* proctab[NPROC] = {&main_proc};

struct thread_stack_anchor {
    struct thread * ktp;
    void * kgp;
};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
    assert(memory_initialized && heap_initialized);
    assert(!procmgr_initialized);

    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    procmgr_initialized = 1;
}

int process_exec(struct uio* exefile, int argc, char** argv) {
    // FIXME
    // Inputs: struct uio* exefile - pointer to uio object of executable
    //          int argc - number of arguments (strings)
    //          char** argv - pointer to array of char pointers (which point to strings of arguments)
    // Outputs: ideally, should not return. returns 0 on error. 
    // Description: Switches between current process and given process. Creates a new stack, loads uio object using ELF, 
    //              sets up trap frame & jumps to user space
    // Side Effects: resets active memory space

    if (exefile == NULL) {return -EINVAL; }

    void* pg = alloc_phys_page();   // kernel/physical address 
    memset(pg, 0, PAGE_SIZE);
    // copy stack and argc and argv strings onto physical page... 
    int size = build_stack(pg, argc, argv);     // bytes that array + strings + padding take up on the phys page 
    // uintptr_t kernel_sp = (uintptr_t)pg + size; 

    // clear active memory space 
    reset_active_mspace();


    // load elf program
    void (*entry)(struct uio *);
    int ret = elf_load(exefile, (void (**)(void))&entry);
    if (ret != 0) { return -EINVAL;  }


    // map stack/arg page onto new memory space 
    uintptr_t vma = (uintptr_t) (UMEM_END_VMA - PAGE_SIZE);
    map_page(vma, pg, PTE_U | PTE_W | PTE_R | PTE_V);

    uintptr_t user_sp = UMEM_END_VMA - size;    // location of stack 

    char** user_newargs = (char**)(UMEM_END_VMA - size); // location of newarg pointers

    // setup trap frame
    struct trap_frame *tfr = kmalloc(sizeof(struct trap_frame));
    memset(tfr, 0, sizeof(*tfr));
    // set components of trap frame 
    tfr->a0 = argc; 
    tfr->a1 = (long)user_newargs;
    tfr->sp = (void*)user_sp;
    tfr->sepc = entry; 
    tfr->tp = TP;

    //  It is up to the process exec function to fill in the proper values for SPP and SPIE
    // so that sret can properly jump to a user-space function.

    // set sstatus SIE to SPIE 
    // set sstatus SPIE to 1
    
    // set sstatus SPP to detemine privilege after sret 
    // sstatus SPP will be set to the user-mode (sstatus_spp = 0)

    tfr->sstatus = csrr_sstatus();
    tfr->sstatus &= ~RISCV_SSTATUS_SPP;
    tfr->sstatus |= RISCV_SSTATUS_SPIE;

    // a1 is pointer to thread stack anchor - sizeof(trap frame)
    void* sscratch = running_thread_stack_base() - sizeof(struct trap_frame);

    // jump to trap frame 
    trap_frame_jump(tfr, sscratch); 
    
    return 0; 

}

// int process_fork(const struct trap_frame* tfr)
// Inputs: const struct trap_frame* tfr - pointer to trap frame of syscall 
// Outputs: 0 for child and tid for parent 
// Description: this function creates an identical process to the current process
// Side Effects: none
int process_fork(const struct trap_frame* tfr) {
    // FIXME

    // We want to create an identical process
    // First, allocate space for a new process
    // Next, copy the mspace of the current process and place new mtag into child mtag
    // Then, copy over uio objects
    struct process * cur_proc = current_process(); // get current process struct 

    struct process * child_process = kmalloc(sizeof(struct process)); // allocate space
    child_process->mtag = clone_active_mspace(); // clone
    
    for(int i = 0; i < PROCESS_UIOMAX; i++){ // copy uio
        struct uio * obj = cur_proc->uiotab[i];
        child_process->uiotab[i] = obj;
        if(obj){
            obj->refcnt++;
        }
    }

    // create a condition for the parent to wait on
    struct condition child_done;
    condition_init(&child_done, "parent_wait");

    // spawn the child thread
    int thread_tid = spawn_thread("fork_child", (void (*)(void))fork_func, &child_done, tfr);

    if(thread_tid < 0){ // check to make sure spawn or something in thread failed
        kprintf("error with child\n");
        kfree(child_process);
        return thread_tid;
    }

    // link the child process with the new thread 
    proctab[thread_tid] = child_process;
    thread_set_process(thread_tid, child_process);

    // parent sleep until child is done (very important child runs BEOFRE parent!)
    condition_wait(&child_done);

    return thread_tid;
}

/** \brief
 *
 *
 *  Discard memory space, close your associated uio, free the memory you're supposed to free.
 *
 *
 */
void process_exit(void) {
    // FIXME
    // Inputs: N/a
    // Outputs: N/a
    // Description: exits current process. clears associated memory space
    // Side Effects: clears memory space, frees physical pages, dereferences (closes) I/O objects
    struct process* curr_proc = running_thread_process();

    // traverse through uiotab to close uio objects
    for (int i=0; i<PROCESS_UIOMAX; i++){
        struct uio* obj = curr_proc->uiotab[i];
        if (obj != NULL){ 
            uio_close(obj);
            curr_proc->uiotab[i] = NULL;
        }
    }

    // clear memory space, free & unmap pages
    discard_active_mspace();

    // clear page table entry
    for (int i=0; i<NPROC; i++){
        if (proctab[i] != NULL && proctab[i] == curr_proc){
            proctab[i] = NULL;
        }
    }

    // clear running thread
    running_thread_exit();

}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * \brief Builds the initial user stack for a new process.
 *
 * Builds the stack for a new process, including the argument vector (\p argv)
 * and the strings it points to. Note that \p argv must contain \p argc + 1
 * elements (the last one is a NULL pointer).
 *
 * Remember to round the final stack size up to a multiple of 16 bytes
 * (RISC-V ABI requirement).
 *
 * \param[in,out] stack  Pointer to the stack page (destination buffer).
 * \param[in]     argc   Number of arguments in \p argv.
 * \param[in]     argv   Array of argument pointers; length is \p argc+1 and
 *                       \p argv[argc] must be NULL.
 *
 * \return Size of the stack page on success; negative error code on failure.
 */
int build_stack(void* stack, int argc, char** argv) {
    size_t stksz, argsz;
    uintptr_t* newargv;
    char* p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc) return -ENOMEM;

    stksz = (argc + 1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i]) + 1;
        if (PAGE_SIZE - stksz < argsz) return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert(stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char*)(newargv + argc + 1);

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
        argsz = strlen(argv[i]) + 1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}

/**
 * \brief Function to be executed by the child process after fork.
 * This is a very beautiful function.
 * Tell the parent process that it is done with the trap frame, then jumps to user space (hint:
 * which function should we use?)
 *
 * \param[in] done  Pointer to a condition variable to signal parent
 * \param[in] tfr   Pointer to a trap frame
 *
 * \return NONE (very important, this is a hint)
 */
void fork_func(struct condition* done, struct trap_frame* tfr) {
    // FIXME
    
    // broadcast to parent waiting on child
    condition_broadcast(done);

    // set RA to zero since we are CHILD
    tfr->a0 = 0;

    // set tp to thread pointer
    tfr->tp = TP;

    // jump to user space with a non-null sscratch
    void* sscratch = running_thread_stack_base() - sizeof(struct trap_frame); 
    trap_frame_jump(tfr, sscratch);

    return;
}