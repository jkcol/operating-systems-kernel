/*! @file ktfs.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‍‍‍‌​​‌​⁠⁠‌
    @brief KTFS Implementation.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include <string.h>
#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "ktfs.h"
#include <stdint.h>
#include "cache.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "filesys.h"
#include "fsimpl.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"
#include "uioimpl.h"



// INTERNAL TYPE DEFINITIONS
//


/// @brief Superblock struct for the Keegan Teal Filesystem

/**
 * struct ktfs
 * Input: none
 * Output: none
 * Description: Superblock struct for the Keegan Teal Filesystem
 * Side Effects: none
 */
struct ktfs {
    struct filesystem fs; // filesystem interface
    struct cache *cache; // pointer to cache struct
    struct ktfs_superblock sb; // superblock
    // struct filepoint * fplist;
};

/// @brief File struct for a file in the Keegan Teal Filesystem
/**
 * struct ktfs_file
 * Input: none
 * Output: none
 * Description: file struct for a file in the Keegan Teal Filesystem
 * Side Effects: none
 */
struct ktfs_file {
    struct uio base; // uio interface
    struct ktfs *ktfs; // pointer to ktfs struct
    struct ktfs_inode inode; // inode struct
    uint16_t inode_num; // inode number
    unsigned long pos; // current position in file
    char filename[KTFS_MAX_FILENAME_LEN + 1]; // filename
};

/// @brief Listing struct for listing files in the Keegan Teal Filesystem
/**
 * struct ktfs_listing
 * Input: none
 * Output: none
 * Description: listing struct for listing files in the Keegan Teal Filesystem
 * Side Effects: none
 */
struct ktfs_listing {
    struct uio base; // uio interface
    struct ktfs *ktfs; // pointer to ktfs struct
    unsigned long total; // current number of dentries
    unsigned long pos; // current position in dentry
};

// struct filepoint{
//     struct filepoint* next;
//     struct ktfs * ktfs;
//     char name[];
// };


// INTERNAL FUNCTION DECLARATIONS
//

// main functions for ktfs
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr);
void ktfs_close(struct uio* uio);
int ktfs_cntl(struct uio* uio, int cmd, void* arg);
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len);
long ktfs_store(struct uio* uio, const void* buf, unsigned long len);
int ktfs_create(struct filesystem* fs, const char* name);
int ktfs_delete(struct filesystem* fs, const char* name);
void ktfs_flush(struct filesystem* fs);

// listing functions for ktfs -> not used for cp1
void ktfs_listing_close(struct uio* uio);
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz);

//helper functions
static int read_inode(struct ktfs *ktfs, uint16_t inode_num, struct ktfs_inode *inode);
static int read_from_inode(struct ktfs *ktfs, struct ktfs_inode *inode, unsigned long pos, void *buf, unsigned long len);
static uint32_t get_data_block_num(struct ktfs *ktfs, struct ktfs_inode *inode, unsigned long logical_block);
static uint16_t find_file_root_dir(struct ktfs *ktfs, const char *name);
static int find_free_inode_num(struct ktfs * ktfs);
static int f_setend(struct ktfs_file * uio, void * arg);
static int find_free_dblk_num(struct ktfs * ktfs);
static uint32_t set_inode_block_num(struct ktfs *ktfs, struct ktfs_inode *inode, unsigned long logical_block, uint32_t new_idx);
static int clear_data_bitmap(struct ktfs * ktfs, int block_idx);
static int clear_entire_inode(struct ktfs * ktfs, struct ktfs_inode * inode);

/**
 * static const struct uio_intf ktfs_file_uio_intf
 * Input: none
 * Output: none
 * Description: uio interface for ktfs file
 * Side Effects: none
 */
static const struct uio_intf ktfs_file_uio_intf = { // mainly for ktfs_open!
    .close = ktfs_close,
    .read = ktfs_fetch,
    .cntl = ktfs_cntl,
    .write = ktfs_store
};
static const struct uio_intf ktfs_file_uio_intf_2 = { // mainly for ktfs_open!
    .close = ktfs_listing_close,
    .read = ktfs_listing_read,
    .cntl = ktfs_cntl,
    .write = ktfs_store
};

/**
 * @brief Mounts the file system with associated backing cache
 * @param cache Pointer to cache struct for the file system
 * @return 0 if mount successful, negative error code if error
 */

/**
 * int mount_ktfs(const char* name, struct cache* cache)
 * Input: const char* name - name of filesystem to mount, struct cache* cache - pointer to cache struct for filesystem <- backing device
 * Output: 0 if mount successful, negative error code if error
 * Description: Mounts the fifle system with associated backing device
 * Side Effect: None
 */

/*
mount_ktfs()
    ─ kcalloc()
    ─ cache_get_block()
    ─ memcpy()
    ─ cache_release_block()
    ─ attach_filesystem()
*/
int mount_ktfs(const char* name, struct cache* cache) { // follow uart attach's implementation
    struct ktfs *ktfs;
    void *superblock_ptr;
    int ret;
    if (name == NULL || cache == NULL) {
        return -EINVAL; // invalid arguments
    }
    ktfs = kcalloc(1, sizeof(struct ktfs)); // allocate memory for ktfs struct
    if (ktfs == NULL) {
        return -ENOMEM;
    }

    ktfs->cache = cache;
    ret = cache_get_block(cache, 0, &superblock_ptr);
    if (ret < 0) {
        kfree(ktfs);
        return ret;
    }
    memcpy(&ktfs->sb, superblock_ptr, sizeof(struct ktfs_superblock)); // -> copy superblock from block 0 into ktfs struct
    cache_release_block(cache, superblock_ptr, 0); // release superblock block -> marking as not dirty
    ktfs->fs.open = ktfs_open;
    ktfs->fs.flush = ktfs_flush;
    ktfs->fs.create = ktfs_create;
    ktfs->fs.delete = ktfs_delete;

    ret = attach_filesystem(name, &ktfs->fs);
    if (ret < 0) {
        kfree(ktfs);
        return ret;
    }

    // ktfs->fplist = NULL;
    
    return 0;
}

/**
 * @brief Opens a file or ls (listing) with the given name and returns a pointer to the uio through
 * the double pointer
 * @param name The name of the file to open or "\" for listing (CP3)
 * @param uioptr Will return a pointer to a file or ls (list) uio pointer through this double
 * pointer
 * @return 0 if open successful, negative error code if error
 */

/**
 * int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) 
 * Input: struct filesystem* fs - pointer to filesystem struct, const char* name - name of file to open, struct uio** uioptr - double pointer to uio struct
 * Output: 0 if open successful, negative error code if error
 * Description: opens a file or ls with the given name and returns a pointer to the uio through the double pointer
 * Side Effects: none
 */

/*
ktfs_open()
        - read_inode()
        - kcalloc()
        - uio_init1()
    - find_file_root_dir()
        - read_inode()
        - read_from_inode()
            - get_data_block_num()
    - read_inode()
    - kcalloc()
    - memcpy()
    - strncpy()
    - uio_init1()
*/
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) {
    struct ktfs *ktfs = (struct ktfs *)fs; // cast filesystem to ktfs
    struct ktfs_file *file;
    uint16_t inode_num;
    int ret;
    if (uioptr == NULL) {
        return -EINVAL; // invalid arguments
    }

    // listing object 
    if(name == NULL || *name == '\0'){
        struct ktfs_listing * ls;
        ls = kcalloc(1, sizeof(*ls));
        ls->ktfs = ktfs;
        ls->pos = 0;
        struct ktfs_inode root_inode;
        // read root dir inode into root inode 
        ret = read_inode(ktfs, 0, &root_inode);
        ls->total = root_inode.size / KTFS_DENSZ;
        *uioptr = uio_init1(&ls->base, &ktfs_file_uio_intf_2);
        return 0;
    }

    inode_num = find_file_root_dir(ktfs, name); // find inode number of file
    if (inode_num == INVALID_INODE) {
        return -ENOENT; // file not found
    }
    file = kcalloc(1, sizeof(struct ktfs_file)); // allocate memory for ktfs_file struct
    if (file == NULL) {
        return -ENOMEM; // memory allocation failed
    }
    ret = read_inode(ktfs, inode_num, &file->inode); // read inode from inode table
    if (ret < 0) {
        kfree(file);
        return ret;
    }
    // initialize ktfs_file struct
    file->ktfs = ktfs;
    file->inode_num = inode_num;
    file->pos = 0;
    strncpy(file->filename, name, KTFS_MAX_FILENAME_LEN);
    file->filename[KTFS_MAX_FILENAME_LEN] = '\0'; // null terminate
    *uioptr = uio_init1(&file->base, &ktfs_file_uio_intf); // initialize uio struct

    // // add opened file to open file list
    // struct filepoint * fp = kmalloc(sizeof(struct filepoint));
    // fp->next = ktfs->fplist;
    // fp->ktfs = ktfs;
    // strncpy(fp->name, name, sizeof(*name));
    // ktfs->fplist = fp;

    return 0;
}

/**
 * @brief Closes the file that is represented by the uio struct
 * @param uio The file io to be closed
 * @return None
 */

/**
 * void ktfs_close(struct uio* uio)
 * Input: struct uio* uio - pointer to uio struct
 * Output: none
 * Description: closes the file that is represented by the uio struct
 * Side Effects: none
 */

 /*
ktfs_close()
    - kfree()
*/
void ktfs_close(struct uio* uio) {
    struct ktfs_file *file = (struct ktfs_file *)uio; // cast uio to ktfs_file

    // struct filepoint * prev = NULL;
    // struct filepoint * cur = file->ktfs->fplist;

    // // find file in fplist
    // // if file name matches, free the fplist entry and remove from list
    // while(cur != NULL){
    //     if(strcmp(file->filename, cur->name) == 0){
    //         if(cur == file->ktfs->fplist){
    //             file->ktfs->fplist = cur->next;
    //             kfree(cur);
    //             break;
    //         }
    //         prev->next = cur->next;
    //         kfree(cur);
    //         break;
    //     }
    //     prev = cur;
    //     cur = cur->next;
    // }

    kfree(file); // free memory
}

/**
 * @brief Reads data from file attached to uio into provided argument buffer
 * @param uio uio of file to be read
 * @param buf Buffer to be filled
 * @param len Number of bytes to read
 * @return Number of bytes read if successful, negative error code if error
 */

/**
 * long ktfs_fetch(struct uio* uio, void* buf, unsigned long len)
 * Input: struct uio* uio - pointer to uio struct, void* buf - buffer, unsigned long len - number of bytes to read
 * Output: long - number of bytes read if successful, negative error code if error
 * Description: reads data from file attached to uio into provided argument buffer
 * Side Effects: none
 */

 /*
ktfs_fetch()
    - read_from_inode()
        - get_data_block_num()
            - cache_get_block()
            - cache_release_block()
*/
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len) { // similar to other feetch and recv implementations
    if(!uio || !buf || len < 0){ // return if args are invalid <--- not sure about this but added anyways
        return -EINVAL;
    }
    struct ktfs_file *file = (struct ktfs_file *)uio; // cast uio to ktfs_file
    long bytes_read = read_from_inode(file->ktfs, &file->inode, file->pos, buf, len); // read from inode
    if (bytes_read < 0) {
        return -EIO; // return error code
    }
    file-> pos += bytes_read; // update position
    return bytes_read; // return number of bytes read
}

/**
 * @brief Write data from the provided argument buffer into file attached to uio
 * @param uio The file to be written to
 * @param buf The buffer to be read from
 * @param len Number of bytes to write from the buffer to the file
 * @return Number of bytes written from the buffer to the file system if sucessful, negative error
 * code if error
 */

/**
 * 
 * 
 * 
 */
// Not implemented for CP1
long ktfs_store(struct uio* uio, const void* buf, unsigned long len) {
    // FIXME
    if(!uio || !buf){ // check args 
        return -EINVAL;
    }
    if(len == 0){
        return 0;
    }

    int ret;
    void * block_ptr;
    struct ktfs_file * file = (struct ktfs_file*)uio;

    // if we want to write past end of file, extend file 
    if(len + file->pos > file->inode.size){
        unsigned long arg = len + file->pos;
        ret = ktfs_cntl(uio, FCNTL_SETEND, &arg);
        if(ret < 0){
            return -EBADFMT;
        }
        file->inode.size = arg;
    }

    int bytes_written = 0;

    // follows the same logic as read, but copy buf into cache ptr:
    // split the write into block sized chunks -> get current data block number ->
    // read data block contents into block_ptr from cache -> copy contents from buf into cache block ptr
    // -> mark cache block as dirty 
    while(bytes_written < len) { // while we still have bytes to read
        unsigned long current_pos = file->pos + bytes_written;
        unsigned long logical_block = current_pos / KTFS_BLKSZ;
        unsigned long block_offset = current_pos % KTFS_BLKSZ;

        unsigned long bytes_remaining = len - bytes_written; // bytes to read from current block
        unsigned long bytes_in_block = KTFS_BLKSZ - block_offset; // bytes available in current block
        unsigned long to_write = (bytes_remaining < bytes_in_block) ? bytes_remaining : bytes_in_block; // bytes to read from current block
        uint32_t physical_block = get_data_block_num(file->ktfs, &file->inode, logical_block); // actual physical block number in memory

        if (physical_block == 0) {
            return -EIO; 
        }
        ret = cache_get_block(file->ktfs->cache, (unsigned long long)physical_block * KTFS_BLKSZ, &block_ptr);
        if (ret < 0) {
            return ret;
        }

        memcpy((char *)block_ptr + block_offset ,(char *)buf + bytes_written, to_write); // copy data to file

        cache_release_block(file->ktfs->cache, block_ptr, 1); // dirty
        bytes_written += to_write;
    }

    if(bytes_written < 0){ // return error if we wrote nothing 
        return -EIO;
    }

    file->pos += bytes_written; // advance position in file to where we stopped writing to

    cache_flush(file->ktfs->cache);

    return bytes_written;
}

/**
 * @brief Create a new file in the file system
 * @param fs The file system in which to create the file
 * @param name The name of the file
 * @return 0 if successful, negative error code if error
 */

/**
 * 
 * 
 * 
 */
// Not implemented for CP1
int ktfs_create(struct filesystem* fs, const char* name) {
    // FIXME
    if(!fs || !name){
        return -EIO;
    }
    struct ktfs * ktfs = (struct ktfs*)fs;
    int ret;

    // check if file already exists
    if(find_file_root_dir(ktfs, name) != INVALID_INODE){
        return -EEXIST;
    }

    struct ktfs_inode root_inode;
    
    // read root dir inode into root inode 
    ret = read_inode(ktfs, 0, &root_inode);
    if (ret < 0) {
        return -INVALID_INODE; // error reading root inode
    }

    if(root_inode.size >= KTFS_MAX_FILE_SIZE){ // return an error if we have max amount of dentries 
        return -EMFILE;
    }

    // create a new blank inode
    struct ktfs_inode new_inode;
    memset(&new_inode, 0, sizeof(struct ktfs_inode));
    new_inode.size = 0;

    // get number of free inode, find which inode block it is in, and find its offset within the block
    int free_i = find_free_inode_num(ktfs);
    if(free_i < 0){
        return free_i;
    }

    uint32_t num_in_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    int free_i_blknum = free_i / num_in_blk;
    int inode_offset = free_i % num_in_blk;

    void * block_ptr;

    // get the inode block from memory
    ret = cache_get_block(ktfs->cache, (1 + ktfs->sb.inode_bitmap_block_count + ktfs->sb.bitmap_block_count + free_i_blknum) * KTFS_BLKSZ, &block_ptr);

    if(ret < 0){
        return -EBADFMT;
    }
    
    ((struct ktfs_inode*)block_ptr)[inode_offset] = new_inode; // set the inode block at inode_offset to the newly allocated inode

    cache_release_block(ktfs->cache, block_ptr, 1);

    // create a new dentry!
    struct ktfs_dir_entry new_dentry;
    new_dentry.inode = free_i;
    memset(new_dentry.name, 0, KTFS_MAX_FILENAME_LEN + sizeof(uint8_t));
    strncpy(new_dentry.name, name, KTFS_MAX_FILENAME_LEN);

    // create a file to pass into ktfs_store, so we can create a new directory entry in memory
    struct ktfs_file new_dentry_file;
    new_dentry_file.ktfs = ktfs;
    new_dentry_file.inode = root_inode;
    new_dentry_file.inode_num = 0;
    new_dentry_file.pos = root_inode.size; // store new dentry at end of dentry list
 
    ret = ktfs_store((struct uio *)&new_dentry_file, &new_dentry, sizeof(struct ktfs_dir_entry)); // store new dentry

    if(ret < 0){
        return ret;
    }

    root_inode = new_dentry_file.inode;

    // update the root inode in memory
    ret = cache_get_block(ktfs->cache, (1 + ktfs->sb.inode_bitmap_block_count + ktfs->sb.bitmap_block_count) * KTFS_BLKSZ, &block_ptr);
    if(ret < 0){
        return -EBADFMT;
    }
    
    ((struct ktfs_inode *)block_ptr)[0] = root_inode;
    cache_release_block(ktfs->cache, block_ptr, 1);
    
    cache_flush(ktfs->cache);

    return 0;
}

/**
 * @brief Deletes a certain file from the file system with the given name
 * @param fs The file system to delete the file from
 * @param name The name of the file to be deleted
 * @return 0 if successful, negative error code if error
 */

/**
 *
 *
 *
 */
// Not implemented for CP1
int ktfs_delete(struct filesystem* fs, const char* name) {
    // FIXME
    if(!fs || !name){
        return -EIO;
    }
    struct ktfs * ktfs = (struct ktfs*)fs;
    struct ktfs_inode root_inode;  
    void * block_ptr_1; 
    void * block_ptr_2;

    int ret = read_inode(ktfs, 0, &root_inode);
    if(ret < 0){
        return ret;
    }

    // first, we want to remove the directory entry
    // we can do this by copying the last dentry, deleting the last dentry, 
    // and putting it in the deleted file's place (contiguous)
    // then, we need to update inode bitmap and data map 

    int total_dentries = root_inode.size/sizeof(struct ktfs_dir_entry);
    int dentry_idx = -1;
    int dentry_inode = -1;
    struct ktfs_dir_entry comp_dentry;

    // iterate through dentries to find one that has the same name as parameter
    for(int i = 0; i < total_dentries; i++){
        ret = read_from_inode(ktfs, &root_inode, i*sizeof(struct ktfs_dir_entry), &comp_dentry, sizeof(struct ktfs_dir_entry));

        if(strcmp(name, comp_dentry.name) == 0){
            dentry_idx = i;
            dentry_inode = comp_dentry.inode;
            break;
        }
    }

    if(dentry_idx < 0 || dentry_inode < 0){
        return -ENOENT;
    }

    struct ktfs_inode to_del;
    ret = read_inode(ktfs, dentry_inode, &to_del);
    if(ret < 0){
        return ret;
    }
    clear_entire_inode(ktfs, &to_del);

    // we need to swap last dentry with dentry we want to delete 
    if(dentry_idx != (total_dentries-1)){
        struct ktfs_dir_entry end;
        int end_idx = total_dentries - 1;

        // get end directory entry data
        ret = read_from_inode(ktfs, &root_inode, end_idx * sizeof(struct ktfs_dir_entry), &end, sizeof(struct ktfs_dir_entry));
        if(ret < 0){
            return ret;
        }

        //calculate block index and offset 
        int del_dentry_blk = (dentry_idx * sizeof(struct ktfs_dir_entry)) / KTFS_BLKSZ;
        int del_dentry_offset = (dentry_idx * sizeof(struct ktfs_dir_entry)) % KTFS_BLKSZ;

        // get the actual data block number
        int del_dentry_db = get_data_block_num(ktfs, &root_inode, del_dentry_blk);
        if(del_dentry_db < 0){
            return del_dentry_db;
        }
        
        // retrieve the current block data of the dentry
        ret = cache_get_block(ktfs->cache, del_dentry_db * KTFS_BLKSZ, &block_ptr_1);
        if(ret < 0){
            return ret;
        }
        
        struct ktfs_dir_entry * overwrite = (struct ktfs_dir_entry *)((void*)block_ptr_1 + del_dentry_offset); 
        *overwrite = end; // overwrite the to delete file data with last dentry file data

        cache_release_block(ktfs->cache, block_ptr_1, 1);
    }   

    root_inode.size -= sizeof(struct ktfs_dir_entry); // update size

    cache_get_block(ktfs->cache, (1 + ktfs->sb.inode_bitmap_block_count + ktfs->sb.bitmap_block_count) * KTFS_BLKSZ, &block_ptr_1);
    if(ret < 0){
        return ret;
    }

    ((struct ktfs_inode*)block_ptr_1)[0] = root_inode; // update the root inode (we changed the size)
    cache_release_block(ktfs->cache, block_ptr_1, 1);

    // calculate the bitmap block for inode, as well as the byte offset and bit offset
    int in_bit_blk = dentry_inode / (KTFS_BLKSZ * 8);
    int byte = ((dentry_inode % (KTFS_BLKSZ * 8))) / 8;
    int bit = dentry_inode % 8;

    uint8_t mask = (1 << (7-bit)); // create mask for byte 

    ret = cache_get_block(ktfs->cache, (1 + in_bit_blk) * KTFS_BLKSZ, &block_ptr_1); // get bitmap
    if(ret < 0){
        return ret;
    }

    uint8_t *bitmap = (uint8_t *)block_ptr_1;

    bitmap[byte] &= ~mask; // apply mask to set inode in use to 0

    cache_release_block(ktfs->cache, block_ptr_1, 1);

    cache_flush(ktfs->cache);

    return 0;
}

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions
 * @details Any commands such as (FCNTL_GETEND, FCNTL_GETPOS, ...) should pass back through the arg
 * variable. Do not directly return the value.
 * @details FCNTL_GETEND should pass back the size of the file in bytes through the arg variable.
 * @details FCNTL_SETEND should set the size of the file to the value passed in through arg.
 * @details FCNTL_GETPOS should pass back the current position of the file pointer in bytes through
 * the arg variable.
 * @details FCNTL_SETPOS should set the current position of the file pointer to the value passed in
 * through arg.
 * @param uio the uio object of the file to perform the control function
 * @param cmd the operation to execute. KTFS should support FCNTL_GETEND, FCNTL_SETEND (CP2),
 * FCNTL_GETPOS, FCNTL_SETPOS.
 * @param arg the argument to pass in, may be different for different control functions
 * @return 0 if successful, negative error code if error
 */

/**
 * int ktfs_cntl(struct uio* uio, int cmd, void* arg)
 * Input: struct uio* uio - pointer to uio struct, int cmd - command to execute, void* arg - argument that passes in
 * Output: 0 if successful, negative error code if error
 * Description: given a file io object, a specific command, and possibly some arguments, execute the corresponding functions
 * Side Effects: none
 */

/*
ktfs_cntl()
    - [no function calls, just switches on cmd]
*/
int ktfs_cntl(struct uio* uio, int cmd, void* arg) {
    struct ktfs_file *file = (struct ktfs_file *)uio; // casting
    if (arg == NULL) {
        return -EINVAL; // invalid argument
    }
    switch (cmd) {
        case FCNTL_GETEND:
            *(unsigned long *)arg = file->inode.size; // pass back size of file in bytes
            return 0;
        case FCNTL_GETPOS:
            *(unsigned long *)arg = file->pos; // pass back current position of file pointer in bytes
            return 0;
        case FCNTL_SETPOS: {
            file->pos = *(unsigned long *)arg; // set current position of file pointer
            return 0;
        }
        case FCNTL_SETEND: {
            int ret = f_setend(file, arg);
            file->inode.size = *(unsigned long*)arg;
            return ret;
        }
        default:
            return -EINVAL; // invalid command
    }
}

/**
 * @brief Flushes the cache to the backing device
 * @return 0 if flush successful, negative error code if error
 */

/**
 * void ktfs_flush(struct filesystem* fs)
 * Input: struct filesystem* fs - pointer to filesystem struct
 * Output: none
 * Description: flushes the cache to the backing device
 * Side Effects: none
 */


/*
ktfs_flush()
    - cache_flush()
*/
void ktfs_flush(struct filesystem* fs) { // easy one!
    struct ktfs *ktfs = (struct ktfs *)fs; // cast filesystem to ktfs
    cache_flush(ktfs->cache); // flush the cache to the backing device
}

/**
 * @brief Closes the listing device represented by the uio pointer
 * @param uio The uio pointer of ls
 * @return None
 */

/**
 * 
 * 
 * 
 */
// Not implemented for CP1
void ktfs_listing_close(struct uio* uio) {
    // FIXME
    struct ktfs_listing* const ls = (struct ktfs_listing*)uio;
    kfree(ls);
    return;
}

/**
 * @brief Reads all of the files names in the file system using ls and copies them into the
 * providied buffer
 * @param uio The uio pointer of ls
 * @param buf The buffer to copy the file names to
 * @param bufsz The size of the buffer
 * @return The size written to the buffer
 */

/**
 * 
 * 
 * 
 */

// Not implemented for CP1
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // FIXME
    if(!uio || !buf || bufsz < 0){
        return -EIO;
    }

    if(bufsz == 0){
        return 0;
    }

    // init
    struct ktfs_listing* const ls = (struct ktfs_listing*)uio;
    struct ktfs_inode root_inode;
    struct ktfs_dir_entry dentry;
    int ret;
    int bwrit = 0;

    // get root inodde so we can get dentry info
    ret = read_inode(ls->ktfs, 0, &root_inode);
    if(ret < 0){
        return ret;
    }

    unsigned long entry_num = root_inode.size / KTFS_DENSZ; // get number of dentries

    while(ls->pos < entry_num){
        ret = read_from_inode(ls->ktfs, &root_inode, ls->pos * KTFS_DENSZ, &dentry, KTFS_DENSZ);
        if(ret < 0){
            return ret;
        }

        if(dentry.inode == 0){
            ls->pos++;
            continue;
        }

        size_t len = strlen(dentry.name) + 1; // add 1 to length of name for null

        if(bwrit + len > bufsz){ // if we write past end of buffer, break
            break;
        }

        memcpy(buf + bwrit, dentry.name, len); // copy over name
        bwrit += len;// increment bytes written
        ls->pos++; // increment position
    }

    return bwrit;
}




// INTERNAL HELPER FUNCTION DEFINITIONS


/**
 * static int read_inode(struct ktfs *ktfs, uint16_t inode_num, struct ktfs_inode *inode)
 * Input: struct ktfs *ktfs - pointer to ktfs struct, uint16_t inode_num - inode number, struct ktfs_inode *inode - pointer to inode struct
 * Output: int - 0 if successful, negative error code if error
 * Description: reads inode from inode table and fills inode struct
 * Side Effects: none
 */

/*
read_inode()
    - cache_get_block()
    - memcpy()
    - cache_release_block()
*/

/*
block 0: Superblock
blocks 1 to (1 + inode_bitmap_block_count - 1): Inode bitmaps
blocks after that: Data block bitmaps
blocks after that: INODE BLOCKS <-- tryna find this
blocks after that: Data blocks
*/

//PURPOSE: get file's metada using ID number
static int read_inode(struct ktfs *ktfs, uint16_t inode_num, struct ktfs_inode *inode) { // reads from virtio block device using cache
    void * block_ptr;
    int ret;
    uint32_t inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ; // should be 16
    uint32_t inode_block_index = inode_num / inodes_per_block;  // 0-based
    uint32_t inode_offset = inode_num % inodes_per_block; // 0-15
    uint32_t first_inode_block = 1 + ktfs->sb.inode_bitmap_block_count + ktfs->sb.bitmap_block_count; // block index of first inode block
    uint32_t block_num = first_inode_block + inode_block_index; // block number where inode is located
    ret = cache_get_block(ktfs->cache, (unsigned long long)block_num * KTFS_BLKSZ, &block_ptr);
    if (ret < 0) {
        return ret;
    }
    struct ktfs_inode *inode_array = (struct ktfs_inode *)block_ptr; // cast block pointer to inode array
    memcpy(inode, &inode_array[inode_offset], sizeof(struct ktfs_inode)); // copy inode data into inode struct
    cache_release_block(ktfs->cache, block_ptr, 0); // not dirty
    return 0;
}

/**
 * static uint32_t get_data_block_num(struct ktfs *ktfs, struct ktfs_inode *inode, unsigned long logical_block)
 * Input: struct ktfs *ktfs - pointer to ktfs struct, struct ktfs_inode *inode - pointer to inode struct, unsigned long logical_block - logical block number
 * Output: uint32_t - data block number
 * Description: gets data block number from inode for given logical block number
 * Side Effects: none
 */

/*
get_data_block_num()
    - cache_get_block()
    - cache_release_block()
*/

//PURPOSE: translate a logical block number into a physical block number
static uint32_t get_data_block_num(struct ktfs *ktfs, struct ktfs_inode *inode, unsigned long logical_block) {
    void *block_ptr;
    int ret;
    uint32_t idx; // index in data-area
    uint32_t phys; // physical block number
    uint32_t *block_array; // array of block indices
    uint32_t ptrs_per_block = KTFS_BLKSZ / sizeof(uint32_t); // number of block pointers per block -> 128
    uint32_t first_data_block = 1 + ktfs->sb.inode_bitmap_block_count + ktfs->sb.bitmap_block_count + ktfs->sb.inode_block_count;

    // direct blokcs
    if (logical_block < KTFS_NUM_DIRECT_DATA_BLOCKS) { // 0-3 direct blocks
        idx = inode->block[logical_block];
        if (idx == 0) {
            return first_data_block;
        }
        phys = first_data_block + idx; // physical block number
        if (phys >= ktfs->sb.block_count) return 0;
        return phys;
    }

    // single indirect
    logical_block -= KTFS_NUM_DIRECT_DATA_BLOCKS; // adjust logical block number
    if (logical_block < ptrs_per_block) {
        if (inode->indirect == 0) return 0;
        uint32_t ind_phys = first_data_block + inode->indirect;
        if (ind_phys >= ktfs->sb.block_count) return 0;

        ret = cache_get_block(ktfs->cache, (unsigned long long)ind_phys * KTFS_BLKSZ, &block_ptr);
        if (ret < 0) return 0;
        // read indirect-block-of-indices
        block_array = (uint32_t *)block_ptr; // array of data-area indices to data blocks
        idx = block_array[logical_block];
        cache_release_block(ktfs->cache, block_ptr, 0);

        if (idx == 0) return 0;
        phys = first_data_block + idx;
        if (phys >= ktfs->sb.block_count) return 0;
        return phys;
    }

    // double indirect
    logical_block -= ptrs_per_block;
    uint32_t dindirect_span = ptrs_per_block * ptrs_per_block; // number of logical blocks per double indirect block
    uint32_t dindirect_index = logical_block / dindirect_span; // double indirect block index
    if (dindirect_index >= KTFS_NUM_DINDIRECT_BLOCKS) return 0;

    uint32_t remainder = logical_block % dindirect_span; // remainder within double indirect block
    uint32_t indirect_index = remainder / ptrs_per_block; // indirect block index
    uint32_t direct_index = remainder % ptrs_per_block; // direct block index

    uint32_t dind_idx = inode->dindirect[dindirect_index]; // index in data-area
    if (dind_idx == 0) return 0;
    uint32_t dind_phys = first_data_block + dind_idx;
    if (dind_phys >= ktfs->sb.block_count) return 0;

    // read indirect-block-of-indices
    ret = cache_get_block(ktfs->cache, (unsigned long long)dind_phys * KTFS_BLKSZ, &block_ptr);
    if (ret < 0) return 0;

    // read indirect-block-of-indices
    block_array = (uint32_t *)block_ptr;
    uint32_t ind_idx = block_array[indirect_index];
    cache_release_block(ktfs->cache, block_ptr, 0);
    if (ind_idx == 0) return 0;

    // read direct-block-of-indices
    uint32_t ind_phys = first_data_block + ind_idx;
    if (ind_phys >= ktfs->sb.block_count) return 0;

    // read direct-block-of-indices
    ret = cache_get_block(ktfs->cache, (unsigned long long)ind_phys * KTFS_BLKSZ, &block_ptr);
    if (ret < 0) return 0;
    block_array = (uint32_t *)block_ptr;
    idx = block_array[direct_index];
    cache_release_block(ktfs->cache, block_ptr, 0);

    // final data block index
    if (idx == 0) return 0;
    phys = first_data_block + idx;
    if (phys >= ktfs->sb.block_count) return 0;
    return phys;
}


// static uint32_t get_data_block_num(struct ktfs *ktfs, struct ktfs_inode *inode, unsigned long logical_block) {
//     void *block_ptr;
//     int ret;
//     uint32_t block_num;
//     uint32_t *block_array;
//     uint32_t ptrs_per_block = KTFS_BLKSZ / sizeof(uint32_t); // number of block pointers per block -> 128
//     if (logical_block < KTFS_NUM_DIRECT_DATA_BLOCKS) { // 0-3 direct blocks
//         block_num = inode->block[logical_block];

//         if (block_num == 0) {
//             uint32_t first_data_block = 1 + ktfs->sb.inode_bitmap_block_count + 
//                                      ktfs->sb.bitmap_block_count + 
//                                      ktfs->sb.inode_block_count;
//             return first_data_block;
//         }
//         return block_num;
//     }
//     logical_block -= KTFS_NUM_DIRECT_DATA_BLOCKS; // adjust logical block number
//     if (logical_block < ptrs_per_block) { // single indirect block
//         if (inode->indirect == 0) {
//             return 0;
//         }
//         ret = cache_get_block(ktfs->cache, (unsigned long long)inode->indirect * KTFS_BLKSZ, &block_ptr);
//         if (ret < 0) {
//             return 0;
//         }
//         block_array = (uint32_t *)block_ptr; // array of direct block numbers
//         block_num = block_array[logical_block]; // get direct block number
//         if (block_num == 0 || block_num > ktfs->sb.block_count) {
//             cache_release_block(ktfs->cache, block_ptr, 0);
//             return 0;
//         }
//         cache_release_block(ktfs->cache, block_ptr, 0); // not dirty
//         return block_num;
//     }
//     logical_block -= ptrs_per_block;  // Now within doubly-indirect region

//     // uint32_t dindirect_index = logical_block / (ptrs_per_block * ptrs_per_block); // double indirect block index
//     uint32_t dindirect_index = logical_block / (ptrs_per_block * ptrs_per_block);
    
//     if (dindirect_index >= KTFS_NUM_DINDIRECT_BLOCKS) {
//         return 0;
//     }

//     // uint32_t remainder = logical_block % (ptrs_per_block * ptrs_per_block);
//     // uint32_t indirect_index = remainder / ptrs_per_block;
//     // uint32_t direct_index = remainder % ptrs_per_block;
//     uint32_t remainder = logical_block % (ptrs_per_block * ptrs_per_block);
//     uint32_t indirect_index = remainder / ptrs_per_block;
//     uint32_t direct_index = remainder % ptrs_per_block;

    
//     // uint32_t indirect_index = (logical_block / ptrs_per_block) % ptrs_per_block;
//     // uint32_t direct_index = logical_block % ptrs_per_block;
//     ret = cache_get_block(ktfs->cache, (unsigned long long)inode->dindirect[dindirect_index] * KTFS_BLKSZ, &block_ptr);
//     if (ret < 0) {
//         return 0;
//     }
//     block_array = (uint32_t *)block_ptr; // array of indirect block numbers
//     uint32_t indirect_block_num = block_array[indirect_index]; // get indirect block number
//     cache_release_block(ktfs->cache, block_ptr, 0); // not dirty
//     if (indirect_block_num == 0 || indirect_block_num > ktfs->sb.block_count) {
//         return 0;
//     }
//     ret = cache_get_block(ktfs->cache, (unsigned long long)indirect_block_num * KTFS_BLKSZ, &block_ptr);
//     if (ret < 0) {
//         return 0;
//     }
//     block_array = (uint32_t *)block_ptr; // array of direct block numbers
//     block_num = block_array[direct_index]; // get direct block number
//     cache_release_block(ktfs->cache, block_ptr, 0); // not dirty
//     if (block_num == 0 || block_num > ktfs->sb.block_count) {
//         return 0;
//     }
//     return block_num;
// }




/**
 * static int read_from_inode(struct ktfs *ktfs, struct ktfs_inode *inode, unsigned long pos, void *buf, unsigned long len)
 * Input: struct ktfs *ktfs - pointer to ktfs struct, struct ktfs_inode *inode - pointer to inode struct, unsigned long pos - position to read from, void *buf - buffer, unsigned long len - length of data
 * Output: int - 0 if successful, negative error code if error
 * Description: reads data from inode starting at pos into buf for len bytes
 * Side Effects: none
 */

/*
read_from_inode()
    - get_data_block_num()
    - cache_get_block()
    - memcpy()
    - cache_release_block()
*/

//PURPOSE: read actual content from file given inode
static int read_from_inode(struct ktfs *ktfs, struct ktfs_inode *inode, unsigned long pos, void *buf, unsigned long len) { // similar to uart.c's read implementation
    void *block_ptr;
    int ret;
    unsigned long bytes_read = 0;
    if (pos >= inode->size) {
        return 0; // nothing to read
    }
    if (pos + len > inode->size) {
        len = inode->size - pos; // adjust length to not exceed file size
    }
    while(bytes_read < len) { // while we still have bytes to read
        unsigned long current_pos = pos + bytes_read;
        unsigned long logical_block = current_pos / KTFS_BLKSZ;
        unsigned long block_offset = current_pos % KTFS_BLKSZ;

        unsigned long bytes_remaining = len - bytes_read; // bytes to read from current block
        unsigned long bytes_in_block = KTFS_BLKSZ - block_offset; // bytes available in current block
        unsigned long to_read = (bytes_remaining < bytes_in_block) ? bytes_remaining : bytes_in_block; // bytes to read from current block
        uint32_t physical_block = get_data_block_num(ktfs, inode, logical_block); // actual physical block number in memory
        if (physical_block == 0) {
            return -EIO; 
        }
        ret = cache_get_block(ktfs->cache, (unsigned long long)physical_block * KTFS_BLKSZ, &block_ptr);
        if (ret < 0) {
            return ret;
        }
        memcpy((char *)buf + bytes_read, (char *)block_ptr + block_offset, to_read); // copy data to buffer
        cache_release_block(ktfs->cache, block_ptr, 0); // not dirty
        bytes_read += to_read;
    }
    return bytes_read;
}

/**
 * static uint16_t find_file_root_dir(struct ktfs *ktfs, const char *name)
 * Input: struct ktfs *ktfs - pointer to ktfs struct, const char *name - name of file
 * Output: uint16_t - inode number of file if found, negative error code if error
 * Description: finds file in root directory and returns inode number
 * Side Effects: none
 */

/*
find_file_root_dir()
    - read_inode()
    - read_from_inode()
    - strcmp()
*/

//PURPOSE: find a file's inode number given its name in the root directory -> very important in ktfs_open
static uint16_t find_file_root_dir(struct ktfs *ktfs, const char *name) {
    struct ktfs_inode root_inode;
    int ret;
    
    ret = read_inode(ktfs, ktfs->sb.root_directory_inode, &root_inode);
    if (ret < 0) {
        return INVALID_INODE; // error reading root inode
    }
    unsigned long num_entries = root_inode.size / KTFS_DENSZ;

    for (unsigned long i = 0; i < num_entries; i++) { // iterate through directory entries
        struct ktfs_dir_entry dentry;
        unsigned long pos = i * KTFS_DENSZ;
        ret = read_from_inode(ktfs, &root_inode, pos, &dentry, sizeof(struct ktfs_dir_entry)); // read directory entry
        if (ret < 0) {
            return INVALID_INODE; // error reading directory entry
        }
        if (strcmp(dentry.name, name) == 0) {
            return dentry.inode; // file found
        }
    }
    return INVALID_INODE; // file not found
}

// this will return an int that corresponds to the first free inode number, if the first free inode is the 0th inode
// in the 2nd free inode block, the inode number would be 32
static int find_free_inode_num(struct ktfs * ktfs){
    void * block_ptr;
    int inode_num = 0;
    int ret;

    // iterate through each inode bitmap
    for(int block = 1; block < (1+ktfs->sb.inode_bitmap_block_count); block++){
        ret = cache_get_block(ktfs->cache, block * KTFS_BLKSZ, &block_ptr); // get bitmap from cache
        if(ret < 0){
            return -ENOENT;
        }

        uint8_t *bitmap = (uint8_t*)(block_ptr);
       
        for(int i = 0; i < KTFS_BLKSZ; i++){ // iterate through each byte
            uint8_t byte = bitmap[i];

            // check if there is a zero in byte
            if(byte != 0xFF){
                for(int bit = 0; bit < 8; bit++){ // scan through each bit

                    int cur_inode = inode_num + i*8 + bit;

                    if(cur_inode == 0){
                        continue;
                    }

                    if((byte & (1 << (7-bit))) == 0){ // if one of the bits is zero, return inode number and mark inode in use
                        bitmap[i] |= (1<<(7-bit));
                        cache_release_block(ktfs->cache, block_ptr, 1);
                        return inode_num + (i*8) + bit;
                    }
                }
            }
        }
        cache_release_block(ktfs->cache, block_ptr, 0);
        inode_num += KTFS_BLKSZ * 8;
    }

    return -ENOINODEBLKS;
}

// this function extends the end of a file, and will allocate more data blocks if necessary
// returns new length upon success, negative otherwise
static int f_setend(struct ktfs_file * file, void * arg){
    unsigned long new_len = *(unsigned long *)arg;
    int ret;

    void * block_pointer;

    if(new_len <= file->inode.size){
        file->inode.size = new_len;
        return new_len;
    }

    // find out which range of logical blocks to allocate
    // i.e. lets say we have 2 pages and we want to extend to 4, to start will be at 2 and
    // to end will be at 4
    int toStart = (file->inode.size + KTFS_BLKSZ - 1) / KTFS_BLKSZ;
    int toEnd = (new_len + KTFS_BLKSZ -1) / KTFS_BLKSZ;

    for(int i = toStart; i < toEnd; i++){
        int free_db = find_free_dblk_num(file->ktfs); // find a free data block (we will put this in inode)
        if(free_db < 0){
            return -ENODATABLKS;
        }

        int first_data = 1 + file->ktfs->sb.bitmap_block_count + file->ktfs->sb.inode_bitmap_block_count + file->ktfs->sb.inode_block_count;

        ret = cache_get_block(file->ktfs->cache, (first_data + free_db) * KTFS_BLKSZ, &block_pointer); // get block from storage
        if(ret < 0){
            return -EBADFMT;
        }

        memset(block_pointer, 0, KTFS_BLKSZ); // clear new page
        cache_release_block(file->ktfs->cache, block_pointer, 1);

        set_inode_block_num(file->ktfs, &file->inode, i, free_db);
    }

    // update inode of recently extended file
    file->inode.size = new_len;

    uint32_t inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ; // should be 16
    uint32_t inode_block_index = file->inode_num / inodes_per_block;  // 0-based
    uint32_t inode_offset = file->inode_num % inodes_per_block; // 0-15

    ret = cache_get_block(file->ktfs->cache, (inode_block_index + 1 + file->ktfs->sb.inode_bitmap_block_count + file->ktfs->sb.bitmap_block_count) * KTFS_BLKSZ, &block_pointer);
    ((struct ktfs_inode*)block_pointer)[inode_offset] = file->inode; // put updated inode into cache 
    cache_release_block(file->ktfs->cache, block_pointer, 1);

    return 0;
}    

// this will return an int that corresponds to the first free data block
static int find_free_dblk_num(struct ktfs * ktfs){
    void * block_ptr;
    int dblk = 0;
    int ret;

    // iterate through each inode bitmap
    for(int block = 1 + ktfs->sb.inode_bitmap_block_count; block < (1+ktfs->sb.bitmap_block_count+ktfs->sb.inode_bitmap_block_count); block++){
        ret = cache_get_block(ktfs->cache, block * KTFS_BLKSZ, &block_ptr); // get bitmap from cache
        if(ret < 0){
            return -ENOENT;
        }

        uint8_t *bitmap = (uint8_t*)(block_ptr);
       
        for(int i = 0; i < KTFS_BLKSZ; i++){ // iterate through each byte
            uint8_t byte = bitmap[i];

            // check if there is a zero in byte
            if(byte != 0xFF){
                for(int bit = 0; bit < 8; bit++){ // scan through each bit

                    if(dblk == 0 && i == 0 && bit == 0){ // skip the first data block
                        continue;
                    }

                    if((byte & (1 << (7-bit))) == 0){ // if one of the bits is zero, return inode number and mark inode in use
                        bitmap[i] |= (1<<(7-bit));
                        cache_release_block(ktfs->cache, block_ptr, 1);
                        cache_flush(ktfs->cache); // flush to ensure bitmap update
                        return dblk + (i*8) + bit;
                    }
                }
            }
        }
        cache_release_block(ktfs->cache, block_ptr, 0);
        dblk += KTFS_BLKSZ * 8;
    }

    return -ENODATABLKS;
}

//PURPOSE: set the data pointers inside the inode to the physical block number
static uint32_t set_inode_block_num(struct ktfs *ktfs, struct ktfs_inode *inode, unsigned long logical_block, uint32_t new_idx) {
    void *block_ptr;
    int ret;
    uint32_t idx; // index in data-area
    uint32_t phys; // physical block number
    uint32_t *block_array; // array of block indices
    uint32_t ptrs_per_block = KTFS_BLKSZ / sizeof(uint32_t); // number of block pointers per block -> 128
    uint32_t first_data_block = 1 + ktfs->sb.inode_bitmap_block_count + ktfs->sb.bitmap_block_count + ktfs->sb.inode_block_count;

    // direct blokcs
    if (logical_block < KTFS_NUM_DIRECT_DATA_BLOCKS) { // 0-3 direct blocks
        inode->block[logical_block] = new_idx;
        return new_idx;
    }

    // single indirect
    logical_block -= KTFS_NUM_DIRECT_DATA_BLOCKS; // adjust logical block number
    if (logical_block < ptrs_per_block) {
        if(inode->indirect == 0){
            int dblk = find_free_dblk_num(ktfs);
            if(dblk < 0){
                return dblk;
            }
            ret = cache_get_block(ktfs->cache, (first_data_block + dblk)*KTFS_BLKSZ, &block_ptr);
            if(ret < 0){
                return ret;
            }
            memset(block_ptr, 0, KTFS_BLKSZ);
            cache_release_block(ktfs->cache, block_ptr, 1);
            inode->indirect = dblk;
        }
        ret = cache_get_block(ktfs->cache, (first_data_block + inode->indirect) * KTFS_BLKSZ, &block_ptr);
        if(ret < 0){
            return ret;
        }
        block_array = (uint32_t *)block_ptr;
        block_array[logical_block] = new_idx;
        cache_release_block(ktfs->cache, block_ptr, 1);
        return new_idx;
    }

    // double indirect
    logical_block -= ptrs_per_block;
    uint32_t dindirect_span = ptrs_per_block * ptrs_per_block; // number of logical blocks per double indirect block
    uint32_t dindirect_index = logical_block / dindirect_span; // double indirect block index
    if (dindirect_index >= KTFS_NUM_DINDIRECT_BLOCKS) return 0;

    uint32_t remainder = logical_block % dindirect_span; // remainder within double indirect block
    uint32_t indirect_index = remainder / ptrs_per_block; // indirect block index
    uint32_t direct_index = remainder % ptrs_per_block; // direct block index

    // create indirect block if it doesn't exist
    if(inode->dindirect[dindirect_index] == 0){
        int lvl_1 = find_free_dblk_num(ktfs); // find a free block
        if(lvl_1 < 0){
            return lvl_1;
        }

        uint32_t phys_addr = first_data_block + lvl_1; // get block address

        ret = cache_get_block(ktfs->cache, phys_addr * KTFS_BLKSZ, &block_ptr); // get unused block
        if(ret < 0){
            return 0;
        }

        memset(block_ptr, 0, KTFS_BLKSZ); // clear the block
        cache_release_block(ktfs->cache, block_ptr, 1);
        inode->dindirect[dindirect_index] = lvl_1; // add new block to dindirect list
    }

    // go through doubly indirect
    uint32_t ind = inode->dindirect[dindirect_index];
    uint32_t phys_ind = ind + first_data_block;

    ret = cache_get_block(ktfs->cache, phys_ind * KTFS_BLKSZ, &block_ptr); // get indirect page 
    if(ret < 0){
        return 0;
    }

    block_array = (uint32_t *)block_ptr;
    if(block_array[indirect_index] == 0){ // allocate a page from indirect 
        int new_db = find_free_dblk_num(ktfs);  
        if(new_db < 0){
            cache_release_block(ktfs->cache, block_ptr, 0);
            return 0;
        }

        void * db_ptr;
        uint32_t phys_db = first_data_block + new_db;

        ret = cache_get_block(ktfs->cache, phys_db * KTFS_BLKSZ, &db_ptr); // get free block
        if(ret < 0){
            return 0;
        }

        memset(db_ptr, 0, KTFS_BLKSZ); // clear block's contents 
        cache_release_block(ktfs->cache, db_ptr, 1);

        block_array[indirect_index] = new_db; // add new block to inirect array

        cache_release_block(ktfs->cache, block_ptr, 1);
    }
    else{
        cache_release_block(ktfs->cache, block_ptr, 0); 
    }

    // now, actually set dindirect value
    ret = cache_get_block(ktfs->cache, phys_ind * KTFS_BLKSZ, &block_ptr); // get indirect page
    block_array = (uint32_t *)block_ptr;
    uint32_t pg_idx = block_array[indirect_index]; // get index of direct
    cache_release_block(ktfs->cache, block_ptr, 0);

    uint32_t pg_phys = first_data_block + pg_idx;

    ret = cache_get_block(ktfs->cache, pg_phys * KTFS_BLKSZ, &block_ptr); // get direct page
    if(ret < 0){
        return ret;
    }
    block_array = (uint32_t *)block_ptr;

    block_array[direct_index] = new_idx; // set index in direct page to our new index

    cache_release_block(ktfs->cache, block_ptr, 1);

    return new_idx;
}

static int clear_data_bitmap(struct ktfs * ktfs, int block_idx){
    void * block_ptr;
    int ret;

    int data_bitmap_start = 1 + ktfs->sb.inode_bitmap_block_count; // data bitmap starts after superblock and inode bitmaps
    int bit_blk = KTFS_BLKSZ * 8; // each bitmap contains this many bits

    int data_bitmap_idx = block_idx / bit_blk; // contains the offset of bitmap block from start of bitmap blocks
    int data_bitmap_off = block_idx % bit_blk; // offset of the specific block in the bitmap itself

    int bit_idx = data_bitmap_off % 8; // the specific bit in the byte that the data block is located in
    int byte_idx = data_bitmap_off / 8; // the index of the byte that contains the data block in the bitmap

    uint32_t physical_block = data_bitmap_start + data_bitmap_idx; // the offset from the start where the bitmap block is

    ret = cache_get_block(ktfs->cache, physical_block * KTFS_BLKSZ, &block_ptr); // get the bitmap
    if(ret < 0){
        return ret;
    }

    // clear the bitmap entry
    uint8_t * bitmap = (uint8_t *)block_ptr;
    uint8_t data_mask = (1 << (7-bit_idx));
    bitmap[byte_idx] &= ~data_mask;

    cache_release_block(ktfs->cache, block_ptr, 1);

    return 0;
}

static int clear_entire_inode(struct ktfs * ktfs, struct ktfs_inode * inode){
    int ret;
    void * block_ptr;
    void * blk_ptr_2;
    uint32_t first_data_idx = 1+ktfs->sb.bitmap_block_count+ktfs->sb.inode_bitmap_block_count+ktfs->sb.inode_block_count;

    // iterate through inode and clear the data bitmap
    // start with direct: easy
    for(int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++){
        if(inode->block[i] != 0){
            clear_data_bitmap(ktfs, inode->block[i]);
            inode->block[i] = 0;
        }
    }

    // 1 indirect block, go and get indirect block and iterate through 
    if(inode->indirect != 0){   
        ret = cache_get_block(ktfs->cache, (inode->indirect+first_data_idx) * KTFS_BLKSZ, &block_ptr);
        if(ret == 0){
            // get the array pointed to by indirect
            uint32_t * ind_arr = (uint32_t *)block_ptr;
            for(int i = 0; i < (KTFS_BLKSZ / sizeof(uint32_t)); i++){
                if(ind_arr[i] != 0){
                    clear_data_bitmap(ktfs, ind_arr[i]); // clear each used entry
                }
            }
            cache_release_block(ktfs->cache, block_ptr, 0);
        } 
        clear_data_bitmap(ktfs, inode->indirect); // clear ind block
        inode->indirect = 0;
    }

    // next is dindirect
    // iterate through indirect blocks
    for(int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++){
        if(inode->dindirect[i] != 0){ // if there is something in the dind block
            ret = cache_get_block(ktfs->cache, (inode->dindirect[i] + first_data_idx) * KTFS_BLKSZ, &block_ptr);
            if(ret  == 0){
                uint32_t * dind_arr = (uint32_t *)block_ptr; // get block of dindirect addresses
                for(int j = 0; j < (KTFS_BLKSZ/sizeof(uint32_t)); j++){ // iterate dindirect block
                    if(dind_arr[j] != 0){ // if indirect block exists
                        ret = cache_get_block(ktfs->cache, (dind_arr[j] + first_data_idx) * KTFS_BLKSZ, &blk_ptr_2);
                        if(ret == 0){
                            uint32_t * ind_arr = (uint32_t *)blk_ptr_2; // get block of ind addresses
                            for(int k = 0; k < (KTFS_BLKSZ/sizeof(uint32_t)); k++){ // iterate indirect block
                                if(ind_arr[k] != 0){ // if direct block exists
                                    clear_data_bitmap(ktfs, ind_arr[k]); // clear the data bitmap entry
                                }
                            }
                            cache_release_block(ktfs->cache, blk_ptr_2, 0);
                        }
                        clear_data_bitmap(ktfs, dind_arr[j]); // clear the entire indirect block
                    }
                }
                cache_release_block(ktfs->cache, block_ptr, 0);
            }
            clear_data_bitmap(ktfs, inode->dindirect[i]); // clear entire dindirect block
            inode->dindirect[i] = 0;
        }
    }

    return 0;
}
