#include <string.h>
#include <stdint.h>

#include "testsuite_1.h"
#include "error.h"
#include "console.h"
#include "elf.h"
#include "device.h"
#include "filesys.h"
#include "dev/ramdisk.h"
#include "memory.h"
#include "process.h"
#include "riscv.h"
#include "thread.h"
#include "cache.h"
#include "trap.h"
#include "uio.h"
#include "cache.h"
#include "device.h"
#include "uioimpl.h"
#include "../usr/syscall.h"


#define DEVMNTNAME "dev"
#define CACHE_NUM_BLOCKS 64
#define MAX_TEST_FILE_SIZE (1024*1024)
#define BLOCK_SIZE 512

int test_find_storage() {
    struct storage* hd;
    hd = find_storage("vioblk", 0);
    if (hd == NULL) {
        kprintf("Storage device not found\n");
        return -1;
    } else {
        kprintf("Storage device found\n");
    }
    return 0;
}

int test_simple_storage_read() {
    struct storage* hd;
    int retval;
    hd = find_storage("vioblk", 0);
    if (hd == NULL) {
        kprintf("Storage device not found\n");
        return -1;
    } else {
        kprintf("Storage device found\n");
    }

    // retval = storage_open(hd);
    // if (retval != 0) {
    //     kprintf("failed to open storage\n");
    //     return -1;
    // } else {
    //     kprintf("opened storage\n");
    // }

    char buf[512];

    retval = storage_fetch(hd, 0, buf, 512);
    if (retval != 512) {
        kprintf("failed to fetch from storage\n");
        return -1;
    } else {
        kprintf("fetched from storage %d bytes\n", retval);
    }
    for (int i = 0; i < 512; ++i) {
        kprintf("buf[%d] = %x\n", i, buf[i]);
    }
    return 0;
}

int test_simple_storage_write() {
    struct storage* hd;
    int retval;
    hd = find_storage("vioblk", 0);
    if (hd == NULL) {
        kprintf("Storage device not found\n");
        return -1;
    } else {
        kprintf("Storage device found\n");
    }

    retval = storage_open(hd);
    if (retval != 0) {
        kprintf("failed to open storage\n");
        return -1;
    } else {
        kprintf("opened storage\n");
    }

    char wdata[512];
    for (int i = 0; i < 512; ++i) {
        wdata[i] = 511 - i;
    }

    retval = storage_store(hd, 0, wdata, 512);
    if (retval != 512 ) {
        kprintf("failed to write to storage\n");
        return -1;
    }

    char rdata[512];

    retval = storage_fetch(hd, 0, rdata, 512);
    if (retval != 512) {
        kprintf("failed to fetch from storage\n");
    } else {
        kprintf("fetched from storage %d bytes\n", retval);
    }
    for (int i = 0; i < 512; ++i) {
        kprintf("rdata[%d] = %d\n", i, rdata[i]);
    }
    return 0;
}

int test_simple_ramdisk_uio_read() {
    struct uio* ruio;
    ramdisk_attach();
    open_file(DEVMNTNAME, "ramdisk0", &ruio);
    char buf[50];
    int retval = uio_read(ruio, buf, 50);
    if (retval != 50) {
        return -1;
    }
    for (int i = 0; i < 50; ++i) {
        kprintf("buf[%d] = %x\n", i, buf[i]);
    }
    return 0;
}

int test_uio_control_ramdisk_read() {
    struct uio* ruio;
    int retval;
    ramdisk_attach();
    open_file(DEVMNTNAME, "ramdisk0", &ruio);
    char buf[50];
    unsigned long long pos = 5;
    retval = uio_cntl(ruio, FCNTL_SETPOS, &pos);
    if (retval != 0) {
        kprintf("Failed to set pos of ramdisk\n");
        return -1;
    }
    unsigned long long disksz;
    retval = uio_cntl(ruio, FCNTL_GETEND, &disksz);
    if (retval != 0) {
        kprintf("Failed to get end of disk\n");
        return -1;
    }
    kprintf("disksz = %u\n", disksz);
    retval = uio_read(ruio, buf, 10);
    for (int i = 0; i < 10; ++i) {
        kprintf("buf[%d] = %x\n", i, buf[i]);
    }

    retval = uio_cntl(ruio, FCNTL_GETPOS, &pos);
    if (retval != 0) {
        kprintf("Failed to get position of ramdisk\n");
        return -1;
    }
    kprintf("Position of ramdisk uio is %u\n", pos);
    return 0;
}

int test_elf_load_with_ramdisk_uio() {
    struct uio* ruio;
    struct uio* termio;
    int retval;
    ramdisk_attach();
    retval = open_file(DEVMNTNAME, "ramdisk0", &ruio);

    if(retval < 0){
        kprintf("failed to open ramdisk, %d", retval);
        return -1;
    }

    retval = open_file(DEVMNTNAME, "uart1", &termio);

    if(retval < 0){
        kprintf("failed to open uart, %d", retval);
        return -1;
    }

    void (*entry_ptr)(struct uio*);
    retval = elf_load(ruio, (void*)&entry_ptr);
    if (retval < 0) {
        kprintf("elf load failed with retval %d\n", retval);
        return -1;
    }
    retval = spawn_thread("hellothr", (void *)entry_ptr, termio);
    if (retval < 0) {
        kprintf("spawn thread failed with retval %d\n", retval);
        return -1;
    }
    thread_join(retval);
    return 0;
}

int test_cache_get_and_release_block() {
    struct storage* disk;
    struct cache* cptr;
    int ret;

    ramdisk_attach();
    disk = find_storage("ramdisk", 0);

    if(!disk) {
        kprintf("disk not found\n");
        return -1;
    }

    ret = storage_open(disk);

    if(ret < 0){
        kprintf("storage open failed: %d\n", ret);
        return -1;
    }

    ret = create_cache(disk, &cptr);

    if(ret < 0){
        kprintf("create cache failed: %d\n", ret);
        return -1;
    }

    char* buf;
    cache_get_block(cptr, 0, (void*)&buf);
    for (int i = 0; i < 512; ++i) {
        kprintf("buf[%d] = %x\n", i, buf[i]);
    }
    cache_release_block(cptr, buf, 0);
}

int test_hello() {
    struct uio * ruio;
     struct uio * termio;
    void (*entry)(struct uio *);
    int retval;

    ramdisk_attach();

    kprintf("opening file\n");

    retval = open_file("dev", "ramdisk0", &ruio);
    if(retval < 0){
        kprintf("ramdisk hello load failed, retval=%d\n", retval);
        return -1;
    }

    retval = open_file("dev", "uart1", &termio);
    if(retval < 0){
        kprintf("failed to open uart, retval = %d\n", retval);
        return -1;
    }

    kprintf("loading elf\n");

    retval = elf_load(ruio, (void (**)(void))&entry);
    if(retval < 0){
        kprintf("failed to load ELF, retval = %d\n", retval);
        return -1;
    }

    kprintf("spawning thread\n");

    retval = spawn_thread("hello_thread", (void *)entry, termio);;
    if(retval < 0){
        kprintf("spawn thread failed\n");
        return -1;
    }

    kprintf("joining thread\n");

    thread_join(retval);

    kprintf("finished thread_join\n");

    return 0;
}


int test_trek() {
    struct uio * ruio;
    struct uio * termio;
    void (*entry)(struct uio *);
    int retval;

    ramdisk_attach();

    retval = open_file("dev", "ramdisk0", &ruio);
    if(retval < 0){
        kprintf("ramdisk trek load failed, retval=%d\n", retval);
        return -1;
    }

    retval = open_file("dev", "uart1", &termio);
    if(retval < 0){
        kprintf("failed to open uart, retval = %d\n", retval);
        return -1;
    }

    retval = elf_load(ruio, (void (**)(void))&entry);
    if(retval < 0){
        kprintf("failed to load ELF, retval = %d\n", retval);
        return -1;
    }

    retval = spawn_thread("trek_thread", (void *)entry, termio);
    if(retval < 0){
        kprintf("spawn thread failed\n");
        return -1;
    }

    thread_join(retval);

    return 0;
}

int test_large_file_read(void) {
    struct uio *u;
    int ret;

    ret = open_file("c", "maxfile", &u);
    if (ret < 0) {
        kprintf("failed to open_file maxfile (%d)\n", ret);
        return -1;
    }

    char buf[512];
    unsigned long long total = 0;

    while (1) {
        ret = uio_read(u, buf, sizeof(buf));
        if (ret < 0) {
            kprintf("failed to read uio (%d)\n", ret);
            return -1;
        }
        if (ret == 0) break;

        for (int i = 0; i < ret; i++) {
            const char expected[] = "KTFS!";
            if (buf[i] != expected[(total + i) % 5]) {
                kprintf("data mismatch at offset %llu\n", total + i);
                return -1;
            }
        }

        total += ret;
    }

    kprintf("success \n", total);
    return 0;
}

int virtio_random_read(){
    struct storage* hd;
    int retval;

    hd = find_storage("vioblk", 0);
    if (!hd) {
        kprintf("Storage device not found\n");
        return -1;
    }

    char buf[512];

    for(int blk = 400; blk < 500; blk = blk+10){
        retval = storage_fetch(hd, blk*512, buf, 512);
        if (retval != 512) {
            kprintf("failed to fetch from storage\n");
            return -1;
        } else {
            kprintf("fetched from storage %d bytes\n", retval);
        }
        for (int i = 0; i < 10; ++i) {
            kprintf("buf[%d] = %x\n", i, buf[i]);
        }
    }


    return 0;
}

int test_ktfs_write_to_disk() {
    char *mountname = "c";
    char *filename_1 = "empty_file_1";
    char *filename_2 = "empty_file_2";
    char *filename_3 = "empty_file_3";
    char *filename_4 = "empty_file_4";
    char *filename_5 = "empty_file_5";
    struct uio *uio;
    struct storage *store;
    int ret;

    // init to write buf
    char write_buf_1[512];
    char write_buf_2[512];
    char write_buf_3[512];
    char write_buf_4[512];
    char write_buf_5[512];
    char read_buf_1[512];
    
    memset(write_buf_1, 0, 512);
    strncpy(write_buf_1, "Test to see if this stays in the actual storage file!", 54);
    memset(write_buf_2, 0, 512);
    strncpy(write_buf_2, "Hello oooooooooooooooooooooo you are the goat", 46);
    memset(write_buf_3, 0, 512);
    strncpy(write_buf_3, "six seven!", 11);
    memset(write_buf_4, 5, 512);
    memset(write_buf_5, 0, 512);
    strncpy(write_buf_5, "Overwrote last dentry!!", 500);

    memset(read_buf_1, 0, 512);

    kprintf("creating files...\n");

    // create empty files
    ret = create_file(mountname, filename_1);
    if(ret < 0){
        return ret;
    }
    ret = create_file(mountname, filename_2);
    if(ret < 0){
        return ret;
    }
    ret = create_file(mountname, filename_3);
    if(ret < 0){
        return ret;
    }
    ret = create_file(mountname, filename_4);
    if(ret < 0){
        return ret;
    }
    
    
    ret = open_file(mountname, filename_1, &uio);
    if(ret < 0){
        return ret;
    }
    kprintf("writing to filename 1...\n");
    uio->intf->write(uio, write_buf_1, 512);
    uio->intf->close(uio);
   
    ret = open_file(mountname, filename_2, &uio);
    if(ret < 0){
        return ret;
    }
    kprintf("writing to filename 2...\n");
    uio->intf->write(uio, write_buf_2, 512);
    uio->intf->close(uio);
   
    ret = open_file(mountname, filename_3, &uio);
    if(ret < 0){
        return ret;
    }
    kprintf("writing to filename 3...\n");
    uio->intf->write(uio, write_buf_3, 512);
    uio->intf->close(uio);
    
    ret = open_file(mountname, filename_4, &uio);
    if(ret < 0){
        return ret;
    }
    kprintf("writing to filename 4...\n");
    uio->intf->write(uio, write_buf_4, 512);
    uio->intf->close(uio);
    
    ret = delete_file(mountname, filename_3);
    if(ret < 0){
        return ret;
    }

    kprintf("past delete file...\n");

    ret = create_file(mountname, filename_5);
    if(ret < 0){
        return ret;
    }

    kprintf("past creating new file...\n");

    ret = open_file(mountname, filename_5, &uio);
    if(ret < 0){
        return ret;
    }
    kprintf("writing to filename 5...\n");
    uio->intf->write(uio, write_buf_5, 512);
    uio->intf->close(uio);

    kprintf("done writing...\n");

    open_file(mountname, NULL, &uio);
    uio->intf->read(uio, read_buf_1, 512);
    uio->intf->close(uio);

    int i = 0;
    while(read_buf_1[i] != '0'){
        kprintf("%c", read_buf_1[i]);
        i++;
    }

    return 0;
}

int test_clone(){
    memory_init();

    mtag_t ret = clone_active_mspace();

    switch_mspace(ret);

    kprintf("done cloning\n");

    return 0;
}

int test_clone_comp(){
    memory_init();

    uintptr_t vma = 0x0C1100000UL;

    alloc_and_map_range(vma, PAGE_SIZE*5, (PTE_U | PTE_R | PTE_A | PTE_D | PTE_V));

    mtag_t ret_tag = clone_active_mspace();

    switch_mspace(ret_tag);

    kprintf("done cloning\n");

    return 0;
}

int test_trek_first(){
    struct uio * elf_file;
    struct uio * termio;
    int ret;

    char * argv[] = {"hello", NULL};
    int argc = 1;
    char * filename = "ramdisk0";

    ret = open_file(DEVMNTNAME, filename, &elf_file);
    if(ret < 0){
        kprintf("failed to open elf: %d\n", ret);
        return ret;
    }

    ret = open_file(DEVMNTNAME, "uart1", &termio);
    if(ret < 0){
        kprintf("failed to open uart: %d\n", ret);
        return ret;
    }

    struct process * proc = current_process();

    proc->uiotab[0] = termio;
    proc->uiotab[1] = termio;
    proc->uiotab[2] = termio;

    kprintf("launching game...\n");

    process_exec(elf_file, argc, argv);

    kprintf("something has failed miserably in process exec!\n");

    return -1;
}

int test_child(){
    kprintf("[CHILD] hello I am child func!\n");

    for(int i = 0; i < 5; i++){
        kprintf("[CHILD] %d\n", i);
    }

    kprintf("[CHILD] exiting...\n");

    process_exit();
}

int test_fork(){
    kprintf("[PARENT] fork test: \n");

    void * pp = alloc_phys_page();
    struct trap_frame tfr;
    memset(&tfr, 0, sizeof(tfr));

    tfr.sepc = test_child;
    tfr.sp = pp + PAGE_SIZE;
    tfr.sstatus = RISCV_SSTATUS_SPP | RISCV_SSTATUS_SPIE;

    kprintf("[PARENT] forking it! \n");

    int child_pid = process_fork(&tfr);

    if(child_pid < 0){
        kprintf("[PARENT] child tid negative: %d\n", child_pid);
        return -1;
    }

    kprintf("[PARENT] successfully forked, waiting on child... \n");
    int tj = thread_join(child_pid);

    if(tj < 0){
        kprintf("[PARENT] thread join failed!\n");
    }

    kprintf("[PARENT] fork passed!\n");

    free_phys_page(pp);

    return 0;
}

void run_testsuite_1() {
    int retval = -EINVAL;
    char * test_output;

    retval = test_1();
    test_output = (retval == 0) ? "test1 passed!" : "test1 failed!"; 
    kprintf("%s\n", test_output);
}

int test_1() {
    // kprintf("Running Trek... \n");
    // test_trek();
    // test_large_file_read();
    // kprintf("testing hello ...\n");
    // test_hello();
    // kprintf("test cache and release... \n");
    // test_cache_get_and_release_block();
    // kprintf("\nelf load ramdisk...\n");
    // test_elf_load_with_ramdisk_uio();
    // kprintf("\nramdisk uio read...\n");
    // test_simple_ramdisk_uio_read();
    // kprintf("\nsimple storage read...\n");
    // test_simple_storage_read();
    // kprintf("\nfind storage...\n");
    // test_find_storage();
    // virtio_random_read();
    // test_ktfs_write_to_disk();
    // test_clone();
    // test_clone_comp();
    test_trek_first();
    // test_fork();
}