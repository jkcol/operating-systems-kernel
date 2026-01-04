/*! @file elf.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‍‍‍‌​​‌​⁠⁠‌
    @brief ELF file loader
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#include "heap.h"
#include "intr.h"
#include "thread.h"
#include <sys/cdefs.h>
#ifdef ELF_TRACE
#define TRACE
#endif

#ifdef ELF_DEBUG
#define DEBUG
#endif

#include "elf.h"

#include <stdint.h>

#include "conf.h"
#include "error.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "uio.h"
#include "console.h"

// Offsets into e_ident

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9

// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE 0
#define EV_CURRENT 1

// ELF header e_type values

enum elf_et { ET_NONE = 0, ET_REL, ET_EXEC, ET_DYN, ET_CORE };

/*! @struct elf64_ehdr
    @brief ELF header struct
*/
struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

/*! @enum elf_pt
    @brief Program header p_type values
*/
enum elf_pt { PT_NULL = 0, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_SHLIB, PT_PHDR, PT_TLS };

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/*! @struct elf64_phdr
    @brief Program header struct
*/
struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define EM_RISCV 243
/**
 * \brief Validates and loads an ELF file into memory.
 *
 * This function validates an ELF file, then loads its contents into memory,
 * returning the start of the entry point through \p eptr.
 *
 * The loader processes only program header entries of type `PT_LOAD`. The layouts
 * of structures and magic values can be found in the Linux ELF header file
 * `<uapi/linux/elf.h>`
 * The implementation should ensure that all loaded sections of the program are
 * mapped within the memory range `0x80100000` to `0x81000000`.
 *
 * Let's do some reading! The following documentation will be very helpful!
 * [Helpful doc](https://linux.die.net/man/5/elf)
 * Good luck!
 * [Educational video](https://www.youtube.com/watch?v=dQw4w9WgXcQ)
 *
 * \param[in]  uio  Pointer to an user I/O corresponding to the ELF file.
 * \param[out] eptr   Double pointer used to return the ELF file's entry point.
 *
 * \return 0 on success, or a negative error code on failure.
 */

// int elf_load(struct uio* uio, void (**eptr)(void))
// Inputs: struct uio* - pointer to file-like object 
//         void (**eptr)(void) - entry function pointer 
// Outputs: 0 on success, error on fail
// Description: checks if ELF file is valid, then loads data from ELF file to memory and 
//              puts entry function in eptr
// Side Effects: None
int elf_load(struct uio* uio, void (**eptr)(void)) {
    // FIXME
    int pie;

    if(!uio || !eptr){ // if invalid args, return error
        return -EINVAL;
    }

    struct elf64_ehdr elf_header; // initialize buffer to hold header file 

    int flags = PTE_U | PTE_X | PTE_R | PTE_W;


    pie = disable_interrupts();

    long numread = uio_read(uio, &elf_header, sizeof(elf_header)); // read uio file into buffer

    if(numread < 0 || (size_t)numread != sizeof(elf_header)){ // return io error if read was unsuccessful
        restore_interrupts(pie);
        return -EIO;
    }

    // kprintf("first 4 bytes: %02x %02x %02x %02x\n",
    //     elf_header.e_ident[0], 
    //     elf_header.e_ident[1],
    //     elf_header.e_ident[2],
    //     elf_header.e_ident[3]); // DEBUG

    if(elf_header.e_ident[0] != 0x7f || elf_header.e_ident[1] != 'E' || elf_header.e_ident[2] != 'L' || 
        elf_header.e_ident[3] != 'F'){ // return error if it isn't an ELF file 
        restore_interrupts(pie);    
        return -EBADFMT;
    }   

    if(elf_header.e_ident[EI_CLASS] != ELFCLASS64){ // return error if class isn't 64-bit 
        restore_interrupts(pie);
        return -ENOTSUP;
    }

    if(elf_header.e_ident[EI_DATA] != ELFDATA2LSB){ // risc-V is little endian, return error if not little endian
        restore_interrupts(pie);
        return -ENOTSUP;
    }

    if(elf_header.e_ident[EI_VERSION] != EV_CURRENT){ // if version is not current, it is invalid, return error
        restore_interrupts(pie);
        return -ENOTSUP; 
    }

    if(elf_header.e_type == ET_NONE){ // return error if unknown type
        restore_interrupts(pie);
        return -ENOTSUP;
    }   

    if(elf_header.e_machine != EM_RISCV){ // return error if not risc-V machine
        restore_interrupts(pie); 
        return -ENOTSUP;
    }

    if(elf_header.e_version == EV_NONE){ // if not current version, return error
        restore_interrupts(pie);
        return -ENOTSUP;
    }

    for(int i = 0; i < elf_header.e_phnum; i++){ // loop through program headers 
        struct elf64_phdr program_header; // init prgram header struct 
        unsigned long long prog_head_pos = elf_header.e_phoff + (unsigned long long)elf_header.e_phentsize*(unsigned long long)i;
        if(uio_cntl(uio, FCNTL_SETPOS, &prog_head_pos) < 0){ // set the file ptr to the start of the correct table
            restore_interrupts(pie);
            return -EIO;
        }

        numread = uio_read(uio, &program_header, sizeof(program_header));

        if(numread < 0 || (size_t)numread != sizeof(program_header)){ // return io error if read was unsuccessful
            restore_interrupts(pie);
            return -EIO;
        }

        if(program_header.p_type != PT_LOAD){ // return error if not loader type
            continue; 
        }

        if(program_header.p_filesz > program_header.p_memsz){ // return error if file is larger than memory
            restore_interrupts(pie);
            return -EBADFMT;
        }

        // kprintf("checking address...\n");

        if(program_header.p_vaddr < UMEM_START_VMA){ // check the bounds for the read
            restore_interrupts(pie);
            return -ENOTSUP;
        }

        if((program_header.p_vaddr + program_header.p_memsz) > UMEM_END_VMA){
            restore_interrupts(pie);
            return -ENOTSUP;
        }

        // kprintf("passed address...\n");

        // int spec_flag = program_header.p_flags;

        uintptr_t start = __align_down(program_header.p_vaddr, PAGE_SIZE);
        uintptr_t end = __align_up(program_header.p_vaddr + program_header.p_memsz, PAGE_SIZE);

        size_t alloc_sz = end - start;
        size_t pages = alloc_sz/PAGE_SIZE;

        void * map = alloc_phys_pages(pages); // allocate and map virtual memory 
        map_range(start, alloc_sz, map, flags);

        // kprintf("allocated pages...\n");

        unsigned long long mem_seg = program_header.p_offset;
        
        if(uio_cntl(uio, FCNTL_SETPOS, &mem_seg) < 0){ // set file position to read data from file
            restore_interrupts(pie);
            return -EIO;
        }

        if(program_header.p_filesz > 0){
            numread = uio_read(uio, (void *)(uintptr_t)program_header.p_vaddr, (unsigned long)program_header.p_filesz);

            if(numread < 0 || (unsigned long)numread != (unsigned long)program_header.p_filesz){ // return io error if read was unsuccessful
                restore_interrupts(pie);
                return -EIO;
            }
        }

        // kprintf("read to vaddr...\n");

        if(program_header.p_memsz > program_header.p_filesz){ // if memory block is larger than file, write zeros 
            memset((void*)((uintptr_t)program_header.p_vaddr + (uintptr_t)program_header.p_filesz), 0, (size_t)(program_header.p_memsz - program_header.p_filesz));
        }
    }

    *eptr = (void (*)(void))(uintptr_t)elf_header.e_entry; // set eptr to entry
    restore_interrupts(pie);
    return 0;
}