/*! @file syscall.c
    @brief system call handlers
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "memory.h"
#include "misc.h"
#include "process.h"
#include "scnum.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "uio.h"


#define KPATH_MAX 1024


// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame *tfr);  // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame *tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char **argv);
static int sysfork(const struct trap_frame *tfr);
static int syswait(int tid);
static int sysprint(const char *msg);
static int sysusleep(unsigned long us);

static int sysfsdelete(const char *path);
static int sysfscreate(const char *path);

static int sysopen(int fd, const char *path);
static int sysclose(int fd);
static long sysread(int fd, void *buf, size_t bufsz);
static long syswrite(int fd, const void *buf, size_t len);
static int sysfcntl(int fd, int cmd, void *arg);
static int syspipe(int *wfdptr, int *rfdptr);
static int sysuiodup(int oldfd, int newfd);

// INTERNAL HELPER FUNCTION DECLARATIONS
//
static inline struct uio *fd_get(struct process *p, int fd);
static inline int fd_alloc(struct process *p, struct uio *u, int *outfd);
static inline int fd_set(struct process *p, int fd, struct uio *u);
static inline void fd_dealloc(struct process *p, int fd);


// EXPORTED FUNCTION DEFINITIONS
//
// these functions are needed because we need to manage file descriptors (fds) for each process.



// struct uio * fd_get(struct process *p, int fd)
// Inputs: process pointer p, file descriptor fd
// Outputs: returns pointer to uio struct if found, NULL if not found or invalid inputs
// Description: helper function to get uio struct from process's fd table
// Side Effects: None
// this is needed because each process has its own file descriptor table, so we need to get the uio struct from the table.
static inline struct uio *fd_get(struct process *p, int fd) {
    if (!p || fd < 0 || fd >= PROCESS_UIOMAX) return NULL;
    return p->uiotab[fd];
}

// finds first free slot, stores u there, write index to *outfd
// static inline int fd_alloc(struct process *p, struct uio *u, int *outfd)
// Inputs: process pointer p, uio struct pointer u, output fd pointer outfd
// Outputs: 0 on success, -EMFILE if no free slots, -EINVAL on invalid inputs
// Description: helper function to allocate a new fd in process's fd table
// Side Effects: modifies process's fd table and outfd
// this is needed because when a process opens a file, we need to allocate a new file descriptor for it.
static inline int fd_alloc(struct process *p, struct uio *u, int *outfd) {
    if (!p || !u || !outfd) return -EINVAL;
    for (int i = 0; i < PROCESS_UIOMAX; i++) {
        if (p->uiotab[i] == NULL) {
            p->uiotab[i] = u;
            *outfd = i;
            return 0;
        }
    }
    return -EMFILE;
}

// int fd_set(struct process *p, int fd, struct uio *u)
// Inputs: process pointer p, file descriptor fd, uio struct pointer u
// Outputs: 0 on success, -EBADFD on invalid inputs, -EBUSY if fd already in use
// Description: helper function to set uio struct at specific fd in process's fd table
// Side Effects: modifies process's fd table
// this is needed because fd_set is used when duplicating file descriptors or setting specific fds.
static inline int fd_set(struct process *p, int fd, struct uio *u) {
    if (!p || !u || fd < 0 || fd >= PROCESS_UIOMAX) return -EBADFD;
    if (p->uiotab[fd] != NULL) return -EBUSY;
    p->uiotab[fd] = u;
    return 0;
}

// void fd_dealloc(struct process *p, int fd)
// Inputs: process pointer p, file descriptor fd
// Outputs: None
// Description: helper function to deallocate fd in process's fd table
// Side Effects: modifies process's fd table
// this is needed because when a process closes a file, we need to deallocate the file descriptor.
static inline void fd_dealloc(struct process *p, int fd) {
    if (p && fd >= 0 && fd < PROCESS_UIOMAX) p->uiotab[fd] = NULL;
}



// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Initiates syscall present in trap frame struct and stores the return address into the sepc
 * @details sepc will be used to return back to program execution after interrupt is handled and
 * sret is called
 * @param tfr pointer to trap frame struct
 * @return void
 */

//void hande_syscall(struct trap_frame *tfr)
// Inputs: struct trap_frame * tfr - pointer to trap frame
// Outputs: None
// Description: initiates syscall present in trap frame struct and stores the return address into the sepc
// Side Effects: modifies trap frame struct
void handle_syscall(struct trap_frame *tfr) {
    tfr->sepc += 4;
    tfr->a0 = syscall(tfr);
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Calls specified syscall and passes arguments
 * @details Function uses register a7 to determine syscall number and arguments are passed in from
 * a0-a5 depending on the function
 * @param tfr pointer to trap frame struct
 * @return result of syscall
 */


// quite literally followed lecture slide instructions
// put a case for every single SYSCALL_ defined in scnum.h even if I don't know what they mean for now.

// int64_t syscall(const struct trap_frame * tfr)
// Inputs: struct trap_frame * tfr - pointer to trap frame
// Outputs: int64_t - result of syscall
// Description: calls specified syscall and passes arguments
// Side Effects: None
int64_t syscall(const struct trap_frame *tfr) { 
    switch(tfr->a7) {
        case SYSCALL_EXIT:
            return sysexit();
        case SYSCALL_EXEC:
            return sysexec(tfr->a0, tfr->a1, (char **)tfr->a2);
        case SYSCALL_FORK:
            return sysfork(tfr);
        case SYSCALL_WAIT:
            return syswait(tfr->a0);
        case SYSCALL_PRINT:
            return sysprint((const char *)tfr->a0);
        case SYSCALL_USLEEP:
            return sysusleep((unsigned long)tfr->a0);
        case SYSCALL_FSCREATE:
            return sysfscreate((const char *)tfr->a0);
        case SYSCALL_FSDELETE:
            return sysfsdelete((const char *)tfr->a0);
        case SYSCALL_OPEN:
            return sysopen((int)tfr->a0, (const char *)tfr->a1);
        case SYSCALL_CLOSE:
            return sysclose((int)tfr->a0);
        case SYSCALL_READ:
            return sysread((int)tfr->a0, (void *)tfr->a1, (size_t)tfr->a2);
        case SYSCALL_WRITE:
            return syswrite((int)tfr->a0, (const void *)tfr->a1, (size_t)tfr->a2);
        case SYSCALL_FCNTL:
            return sysfcntl((int)tfr->a0, (int)tfr->a1, (void *)tfr->a2);
        case SYSCALL_PIPE:
            return syspipe((int *)tfr->a0, (int *)tfr->a1);
        case SYSCALL_UIODUP:
            return sysuiodup((int)tfr->a0, (int)tfr->a1);
        default:
            return -ENOTSUP;
    } // honestly just implemented all the scnum.h cases. i don't really know what they are, but hopefully this is right. 
}

/**
 * @brief Calls process exit
 * @return void
 */

// int sysexit(void)
// Inputs: None
// Outputs: None
// Description: calls process exit
// Side Effects: terminates current process
int sysexit(void) { // according to lecture, this is all we have to do?
    process_exit();
    return 0;
}

/**
 * @brief Executes new process given a executable and arguments
 * @details Valid fd checks, get current process struct, close fd being executed, finally calls
 * process_exec with arguments and executable io "file"
 * @param fd file descripter idx
 * @param argc number of arguments in argv
 * @param argv array of arguments for multiple args
 * @return result of process_exec, else -EBADFD on invalid file descriptors
 */
// int sysexec(int fd, int argc, char ** argv)
// Inputs: int fd - file descriptor idx, int argc - number of arguments in argv, char ** argv - array of arguments
// Outputs: int - result of process_exec, else -EBADFD on invalid file descriptors
// Description: executes new process given a executable and arguments
// Side Effects: may modify current process
int sysexec(int fd, int argc, char **argv) {
    struct process *p = current_process();
    if (!p || argc < 0) return -EINVAL; // cases
    if (fd < 0 || fd >= PROCESS_UIOMAX) return -EBADFD; // invalid fd -> also add fd >= MAX_FD
    if (argv) {
        int x = validate_vptr((void *)argv, (argc + 1) * sizeof(char *), PTE_U); // validate argv ptr
        if (x) return x;
        for (int i = 0; i < argc; i++) {
            char *s = argv[i];
            if (s) { // validate each string in argv
                x = validate_vstr(s, PTE_U); // check if string is valid
                if (x) return x;
            }
        }
    }
    struct uio *u = fd_get(p, fd);  // helper function to get uip from fd
    if (!u) return -EBADFD; // invalid fd

    int result = process_exec(u, argc, argv);
    if (result == 0) {
        p->uiotab[fd] = NULL;
    }
    return result;
}

/**
 * @brief Forks a new child process using process_fork
 * @param tfr pointer to the trap frame
 * @return result of process_fork
 */

// int sysfork(const struct trap_frame * tfr)
// Inputs: struct trap_frame * tfr - pointer to trap frame
// Outputs: int - result of process_fork
// Description: forks a new child process using process_fork
// Side Effects: may modify current process
int sysfork(const struct trap_frame *tfr) {
    if(!tfr){
        return -EIO;
    }
    int ret = process_fork(tfr);
    return ret; 
}

/**
 * @brief Sleeps till a specified child process completes
 * @details Calls thread_join with the thread id the process wishes to wait for
 * @param tid thread_id
 * @return result of thread_join else invalid on invalid thread id
 */
// int syswait(int tid)
// Inputs: int tid - thread id
// Outputs: int - result of thread_join else invalid on invalid thread id
// Description: sleeps till a specified child process completes
// Side Effects: may modify current process
int syswait(int tid) { // taken directly from lecture slides!!
    if(tid >= 0) {
        return thread_join(tid);
    }
    else{
        return -EINVAL;
    }

}

/**
 * @brief Prints to console via kprintf
 * @details Validates that msg string is valid via validate_vstr and pages are mapped, calls kprintf
 * on current running process
 * @param msg string msg in userspace
 * @return 0 on sucess else error from validate_vstr
 */

// int sysprint(const char * msg)
// Inputs: const char * msg - string msg in userspace
// Outputs: int - 0 on sucess else error from validate_vstr
// Description: prints to console via kprintf
// Side Effects: may output to console
int sysprint(const char *msg) { // exactly written from lectures!!
    int result;
    trace("%s(msp=%p)",__func__, msg);
    result = validate_vstr(msg, PTE_U);
    if(result != 0) {
        return result;
    }
    kprintf("Thread <%s:%d> says: %s\n", thread_name(running_thread()), running_thread(), msg);
    return 0;
}

/**
 * @brief Sleeps process till specificed amount of time has passed
 * @details Creates alarm struct, inits struct with name usleep, which sets the current time via the
 * rd_time() function, taking values from the csr, makes frequency calcuation to determine us has
 * passed before waking process
 * @param us time in us for process to sleep
 * @return 0
 */
// int sysusleep(unsigned long us)
// Inputs: unsigned long us - time in us for process to sleep
// Outputs: int - 0
// Description: sleeps process till specificed amount of time has passed
// Side Effects: may modify current process
int sysusleep(unsigned long us) { // simple timer sleep function
    sleep_us(us);
    return 0;
}

/**
 * @brief Creates a new file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * create_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

// sysfscreate(const char * path)
// Inputs: const char * path - user provided path string
// Outputs: int - 0 on success, negative error code if error on error
// Description: creates a new file in the filesystem specified by the path
// Side Effects: may modify filesystem
int sysfscreate(const char *path) {
    int result = validate_vstr(path, PTE_U); // validate path string
    if (result) return result;

    char buf[KPATH_MAX]; // copy path string into kernel buffer
    size_t i = 0;
    for (; i < sizeof(buf) - 1; i++) { // keep i after the null terminator, loop while space remains
        char c = ((const char*)path)[i];  // copy char from user path to kernel buffer
        buf[i] = c; 
        if (!c) break; 
    }
    if (i == sizeof(buf) - 1) return -EINVAL; // path too long
    buf[i] = '\0';

    char *mp = NULL, *fn = NULL;
    result = parse_path(buf, &mp, &fn); // again not too sure since not implemented yet!!! 
    // we still have to implement the filesys
    if (result) return result;
    return create_file(mp, fn); // call create_file with mountpoint and filename
}

/**
 * @brief Deletes a file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * delete_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

// int sysfsdelete(const char * path)
// Inputs: const char * path - user provided path string
// Outputs: int - 0 on success, negative error code if error on error
// Description: deletes a file in the filesystem specified by the path
// Side Effects: may modify filesystem
int sysfsdelete(const char *path) { // similar to sysfscreate
    int r = validate_vstr(path, PTE_U); // validate path string
    if (r) return r;
    char buf[KPATH_MAX];
    size_t i = 0;
    for (; i < sizeof(buf) - 1; i++) { // copy path string into kernel buffer
        char c = ((const char*)path)[i]; 
        buf[i] = c; 
        if (!c) break; 
    } 
    if (i == sizeof(buf) - 1) return -EINVAL;
    buf[i] = '\0';
    char *mp = NULL, *fn = NULL;
    r = parse_path(buf, &mp, &fn); // still have to implement
    if (r) return r;
    return delete_file(mp, fn); // call delete_file with mountpoint and filename
}

/**
 * @brief Opens a file or device of specified fd for given process
 * @details gets current process, allocates file descriptor (if fd = -1) or uses valid file
 * descriptor given, validates and parses user provided path, calls open_file
 * @param fd file descriptor number
 * @param path User provided path string
 * @return fd number if sucessful else return error that occured -EMFILE or -EBADFD
 */

// int sysopen(int fd, const char *path)
// Inputs: int fd - file descriptor number, const char * path - user provided path string
// Outputs: int - fd number if sucessful else return error that occured -EMFILE or -EBADFD
// Description: opens a file or device of specified fd for given process
// Side Effects: may modify current process and filesystem
int sysopen(int fd, const char *path) {
    int r = validate_vstr(path, PTE_U); // validate path string
    if (r != 0) {
        return r;
    }
    char buf[KPATH_MAX];
    size_t i = 0;
    for (; i < sizeof(buf) - 1; i++) { // copy path string into kernel buffer
        char c = ((const char *)path)[i];
        buf[i] = c;
        if (!c) break;
    }
    if (i == sizeof(buf) - 1) return -EINVAL; // path too long
    buf[i] = '\0';
    char *mp = NULL, *fn = NULL;
    r = parse_path(buf, &mp, &fn); // still have to implement the filesys
    if (r) return r;
    struct uio *u = NULL;
    r = open_file(mp, fn, &u); // call open_file with mountpoint and filename
    if (r) return r;
    struct process *p = current_process();
    if (!p) { uio_close(u); return -EINVAL; } // just added check for current process
    if (fd == -1) { // allocate new fd
        r = fd_alloc(p, u, &fd);
        if (r) { uio_close(u); return r; } // allocate new fd
    } else {
        r = fd_set(p, fd, u);
        if (r) { uio_close(u); return r; } // set existing fd
    }
    return fd;
}

/**
 * @brief Closes file or device of specified fd for given process
 * @details gets current process, calls close function of the io, deallocates the file descriptor
 * @param fd file descriptor
 * @return 0 on success, error on invalid file descriptor or empty file descriptor
 */
// int sysclose(int fd)
// Inputs: int fd - file descriptor
// Outputs: int - 0 on success, error on invalid file descriptor or empty file descriptor
// Description: closes file or device of specified fd for given process
// Side Effects: may modify current process
int sysclose(int fd) {
    struct process *p = current_process(); // get current process
    if (!p) {
        return -EINVAL;
    }
    struct uio *u = fd_get(p, fd); // get uio from fd
    if (!u) {
        return -EBADFD;
    }
    uio_close(u); // close uio
    // int result = uio_close(u);
    // if (result != 0) {
    //     return result;
    // }
    fd_dealloc(p, fd); // dealloc fd
    return 0;
}

/**
 * @brief Calls read function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call ioread with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param bufsz number of bytes to be read
 * @return number of bytes read
 */

// long sysread(int fd, void *buf, size_t bufsz)
// Inputs: int fd - file descriptor number, void * buf - pointer to buffer, size_t bufsz - number of bytes to be read
// Outputs: long - number of bytes read
// Description: calls read function of file io on given buffer
// Side Effects: may modify current process
long sysread(int fd, void *buf, size_t bufsz) { // similar to syswrite
    struct process *p = current_process(); // get current process
    if (!p) {
        return -EINVAL;
    }
    struct uio *u = fd_get(p, fd); // get uio from fd
    if (!u) {
        return -EBADFD;
    }
    int result = validate_vptr(buf, bufsz, PTE_U | PTE_W); // validate buffer
    if (result != 0) { // invalid buffer
        return result;
    }
    return uio_read(u, buf, bufsz);
}

/**
 * @brief Calls write function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call iowrite with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param len number of bytes to be written
 * @return number of bytes written
 */
// long syswrite(int fd, const void *buf, size_t len)
// Inputs: int fd - file descriptor number, const void * buf - pointer to buffer, size_t len - number of bytes to be written
// Outputs: long - number of bytes written
// Description: calls write function of file io on given buffer
// Side Effects: may modify current process
long syswrite(int fd, const void *buf, size_t len) { // similar to sysread
    struct process *p = current_process();
    if (!p) {
        return -EINVAL;
    }
    struct uio *u = fd_get(p, fd);
    if (!u) {
        return -EBADFD;
    }
    int result = validate_vptr((void *)buf, len, PTE_U); // validate buffer
    if (result != 0) {
        return result;
    }
    return uio_write(u, buf, len); // call uio_write
}

/**
 * @brief Calls device input output commands for a given device instance
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * ensure that fcntl type exists, validate argument pointer, issue fcntl
 * @param fd file descriptor number
 * @param cmd selection of fcntl
 * @param arg pointer to arguments
 * @return number of bytes written
 */
// int sysfcntl(int fd, int cmd, void *arg)
// Inputs: int fd - file descriptor number, int cmd - selection of fcntl, void * arg - pointer to arguments
// Outputs: int - number of bytes written
// Description: calls device input output commands for a given device instance
// Side Effects: may modify current process
int sysfcntl(int fd, int cmd, void *arg) { // honestly unsure about this one
    struct process *p = current_process(); // get current process
    if (!p) {
        return -EINVAL;
    }
    struct uio *u = fd_get(p, fd);
    if (!u) return -EBADFD; // invalid fd
    size_t argsz = 0;
    int need_w = 0, requires_arg = 0;
    switch (cmd) { // determine arg size and if arg is required
    case FCNTL_GETEND: //if getting end pos, need to write to arg
    case FCNTL_GETPOS: argsz = sizeof(unsigned long long); need_w = 1; requires_arg = 1; break; // if getting pos, need to write to arg
    case FCNTL_SETEND: //if setting end pos, need to read from arg
    case FCNTL_SETPOS: argsz = sizeof(unsigned long long); need_w = 0; requires_arg = 1; break; // if setting pos, need to read from arg
    default: break;
    }
    if (requires_arg && !arg) return -EINVAL; // arg required but not provided
    if (argsz && arg) {
        int flags = PTE_U | (need_w ? PTE_W : 0); // determine flags for validation
        int r = validate_vptr(arg, argsz, flags);
        if (r) return r;
    }
    return uio_cntl(u, cmd, arg); // issue fcntl
}

/**
 * @brief Creates a pipe for the current process
 * @details The function retrieves the current process. If either the write or read descriptor
 * pointer stores a negative value, an unused descriptor is assigned. If both file descriptors are
 * unused and valid, the function connects them via create_pipe function.
 * @param wfdptr pointer to write file descriptor
 * @param rfdptr pointer to read file descriptor
 * @return 0 on success. Else, negative error code on invalid file descriptor, or if a file
 * descriptor is already in use, or if no descriptors are found available.
 */
// int syspipe(int * wfdptr, int * rfdptr)
// Inputs: int * wfdptr - pointer to write file descriptor, int * rfdptr - pointer to read file descriptor
// Outputs: int - 0 on success. Else, negative error code on invalid file descriptor, or if a file 
// Description: creates a pipe for the current process
// Side Effects: may modify current process
int syspipe(int *wfdptr, int *rfdptr) {
    if(!wfdptr || !rfdptr){ // input checks 
        return -EIO;
    }

    // init
    struct process * proc = current_process();
    int widx = *wfdptr;
    int ridx = *rfdptr;
    struct uio * wio;
    struct uio * rio;

    // if negative fd
    if(widx < 0){
        int flag = 0;
        for(int i = 3; i < PROCESS_UIOMAX; i++){ // find free space in io table 
            if(proc->uiotab[i] == NULL){
                widx = i; // found our free index
                flag = 1;
                break;
            }
        }
        if(flag == 0){
            return -EBADFD; // return if no space 
        }
    }
    else{
        if(widx >= PROCESS_UIOMAX || proc->uiotab[widx] != NULL){ // return if bad input or already in use
            return -EBADFD;
        }
    }

    // same as previous loop, except for reader index 
    if(ridx < 0){
        int flag = 0;
        for(int i = 3; i < PROCESS_UIOMAX; i++){
            if(proc->uiotab[i] == NULL && i != widx){
                ridx = i;
                flag = 1;
                break;
            }
        }
        if(flag != 1){
            return -EBADFD;
        }
    }
    else{
        if(ridx >= PROCESS_UIOMAX || proc->uiotab[ridx] != NULL){
            return -EBADFD;
        }
    }

    // create the pipe, which connects the two descriptors and wraps functions
    create_pipe(&wio, &rio);

    // add descriptors to table 
    proc->uiotab[ridx] = rio;
    proc->uiotab[widx] = wio;

    // return indices of read and write descriptors 
    *wfdptr = widx;
    *rfdptr = ridx;

    return 0; 
}


/**
 * @brief Duplicates a file description
 * @details Allocates a new file descriptor that refers to the same open _uio_ as the descriptor
 * _oldfd_. Increments the _refcnt_ if successful.
 * @param oldfd old file descriptor number
 * @param newfd new file descriptor number
 * @return fd number if sucessful else return error on invalid file descriptor or empty file
 * descriptor
 */
// int sysuiodup(int oldfd, int newfd)
// Inputs: int oldfd - old file descriptor number, int newfd - new file descriptor number
// Outputs: int - fd number if sucessful else return error on invalid file descriptor or empty file
// Description: duplicates a file description
// Side Effects: may modify current process
int sysuiodup(int oldfd, int newfd) {
    if (newfd == oldfd) return newfd; // first check
    struct process *p = current_process(); // get current process
    if (!p) return -EINVAL;
    struct uio *u = fd_get(p, oldfd);
    if (!u) return -EBADFD;
    int r;
    if (newfd < 0) { // allocate new fd
        r = uio_addref(u);
        if (r) return r;
        r = fd_alloc(p, u, &newfd); // allocate new fd
        if (r) { uio_close(u); return r; } 
    } else {
        if (fd_get(p, newfd)) return -EBUSY; // newfd already in use
        r = uio_addref(u);
        if (r) return r;
        r = fd_set(p, newfd, u); // set existing fd
        if (r) { uio_close(u); return r; }
    }
    return newfd;
}