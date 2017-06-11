/*
 * Preloader for ld.so
 *
 * Copyright (C) 1995,96,97,98,99,2000,2001,2002 Free Software Foundation, Inc.
 * Copyright (C) 2004 Mike McCormack for CodeWeavers
 * Copyright (C) 2004 Alexandre Julliard
 * Copyright (C) 2017 Michael Müller
 * Copyright (C) 2017 Sebastian Lackner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * Design notes
 *
 * The goal of this program is to be a workaround for exec-shield, as used
 *  by the Linux kernel distributed with Fedora Core and other distros.
 *
 * To do this, we implement our own shared object loader that reserves memory
 * that is important to Wine, and then loads the main binary and its ELF
 * interpreter.
 *
 * We will try to set up the stack and memory area so that the program that
 * loads after us (eg. the wine binary) never knows we were here, except that
 * areas of memory it needs are already magically reserved.
 *
 * The following memory areas are important to Wine:
 *  0x00000000 - 0x00110000  the DOS area
 *  0x80000000 - 0x81000000  the shared heap
 *  ???        - ???         the PE binary load address (usually starting at 0x00400000)
 *
 * If this program is used as the shared object loader, the only difference
 * that the loaded programs should see is that this loader will be mapped
 * into memory when it starts.
 */

/*
 * References (things I consulted to understand how ELF loading works):
 *
 * glibc 2.3.2   elf/dl-load.c
 *  http://www.gnu.org/directory/glibc.html
 *
 * Linux 2.6.4   fs/binfmt_elf.c
 *  ftp://ftp.kernel.org/pub/linux/kernel/v2.6/linux-2.6.4.tar.bz2
 *
 * Userland exec, by <grugq@hcunix.net>
 *  http://cert.uni-stuttgart.de/archive/bugtraq/2004/01/msg00002.html
 *
 * The ELF specification:
 *  http://www.linuxbase.org/spec/booksets/LSB-Embedded/LSB-Embedded/book387.html
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#include <fcntl.h>
#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif
#ifdef HAVE_SYS_SYSCALL_H
# include <sys/syscall.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_ELF_H
# include <elf.h>
#endif
#ifdef HAVE_LINK_H
# include <link.h>
#endif
#ifdef HAVE_SYS_LINK_H
# include <sys/link.h>
#endif
#ifdef HAVE_MACH_O_LOADER_H
#include <mach/thread_status.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#endif

#include "main.h"

#ifndef MAP_COPY
#define MAP_COPY MAP_PRIVATE
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

static struct wine_preload_info preload_info[] =
{
#ifdef __i386__
    { (void *)0x00000000, 0x00001000 },  /* first page */
    { (void *)0x00001000, 0x0000f000 },  /* low 64k */
    { (void *)0x00010000, 0x00100000 },  /* DOS area */
    { (void *)0x00110000, 0x67ef0000 },  /* low memory area */
    { (void *)0x7f000000, 0x03000000 },  /* top-down allocations + shared heap + virtual heap */
#else
    { (void *)0x000000010000, 0x00100000 },  /* DOS area */
    { (void *)0x000000110000, 0x67ef0000 },  /* low memory area */
    { (void *)0x00007ff00000, 0x000f0000 },  /* shared user data */
#ifdef __APPLE__
    /* address below exceeds maximum allowed user space address in macOS */
    { (void *)0x7fff40000000, 0x01ff0000 },  /* top-down allocations + virtual heap */
#else
    { (void *)0x7ffffe000000, 0x01ff0000 },  /* top-down allocations + virtual heap */
#endif
#endif
    { 0, 0 },                            /* PE exe range set with WINEPRELOADRESERVE */
    { 0, 0 }                             /* end of list */
};

#ifdef __APPLE__

#ifndef LC_MAIN
#define LC_MAIN 0x80000028
struct entry_point_command
{
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;
    uint64_t stacksize;
};
#endif

#else  /* __APPLE__ */

/* ELF definitions */
#define ELF_PREFERRED_ADDRESS(loader, maplength, mapstartpref) (mapstartpref)
#define ELF_FIXED_ADDRESS(loader, mapstart) ((void) 0)

#define MAP_BASE_ADDR(l)     0

/* debugging */
#undef DUMP_SEGMENTS
#undef DUMP_AUX_INFO
#undef DUMP_SYMS
#undef DUMP_MAPS

/* older systems may not define these */
#ifndef PT_TLS
#define PT_TLS 7
#endif

#ifndef AT_SYSINFO
#define AT_SYSINFO 32
#endif
#ifndef AT_SYSINFO_EHDR
#define AT_SYSINFO_EHDR 33
#endif

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif

static size_t page_size, page_mask;
static char *preloader_start, *preloader_end;

struct wld_link_map {
    ElfW(Addr) l_addr;
    ElfW(Dyn) *l_ld;
    ElfW(Phdr)*l_phdr;
    ElfW(Addr) l_entry;
    ElfW(Half) l_ldnum;
    ElfW(Half) l_phnum;
    ElfW(Addr) l_map_start, l_map_end;
    ElfW(Addr) l_interp;
};

struct wld_auxv
{
    ElfW(Addr) a_type;
    union
    {
        ElfW(Addr) a_val;
    } a_un;
};

#endif /* __APPLE__ */

/*
 * The __bb_init_func is an empty function only called when file is
 * compiled with gcc flags "-fprofile-arcs -ftest-coverage".  This
 * function is normally provided by libc's startup files, but since we
 * build the preloader with "-nostartfiles -nodefaultlibs", we have to
 * provide our own (empty) version, otherwise linker fails.
 */
void __bb_init_func(void) { return; }

/* similar to the above but for -fstack-protector */
void *__stack_chk_guard = 0;
void __stack_chk_fail_local(void) { return; }
void __stack_chk_fail(void) { return; }

#ifdef __APPLE__
#ifdef __i386__

static const size_t page_size = 0x1000;
static const size_t page_mask = 0xfff;
#define TARGET_CPU_TYPE         CPU_TYPE_X86
#define TARGET_MH_MAGIC         MH_MAGIC
#define TARGET_SEGMENT_COMMAND  LC_SEGMENT
#define target_mach_header      mach_header
#define target_segment_command  segment_command
#define target_thread_state_t   i386_thread_state_t
#ifdef __DARWIN_UNIX03
#define target_thread_ip(x)     (x)->__eip
#else
#define target_thread_ip(x)     (x)->eip
#endif

#define SYSCALL_FUNC( name, nr ) \
    __ASM_GLOBAL_FUNC( name, \
                       "\tmovl $" #nr ",%eax\n" \
                       "\tint $0x80\n" \
                       "\tjnb 1f\n" \
                       "\tmovl $-1,%eax\n" \
                       "1:\tret\n" )

#define SYSCALL_NOERR( name, nr ) \
    __ASM_GLOBAL_FUNC( name, \
                       "\tmovl $" #nr ",%eax\n" \
                       "\tint $0x80\n" \
                       "\tret\n" )

__ASM_GLOBAL_FUNC( start,
                   __ASM_CFI("\t.cfi_undefined %eip\n")
                   /* The first 16 bytes are used as a function signature on i386 */
                   "\t.byte 0x6a,0x00\n"            /* pushl $0 */
                   "\t.byte 0x89,0xe5\n"            /* movl %esp,%ebp */
                   "\t.byte 0x83,0xe4,0xf0\n"       /* andl $-16,%esp */
                   "\t.byte 0x83,0xec,0x10\n"       /* subl $16,%esp */
                   "\t.byte 0x8b,0x5d,0x04\n"       /* movl 4(%ebp),%ebx */
                   "\t.byte 0x89,0x5c,0x24,0x00\n"  /* movl %ebx,0(%esp) */

                   "\tleal 4(%ebp),%eax\n"
                   "\tmovl %eax,0(%esp)\n"          /* stack */
                   "\tleal 8(%esp),%eax\n"
                   "\tmovl %eax,4(%esp)\n"          /* &is_unix_thread */
                   "\tmovl $0,(%eax)\n"
                   "\tcall _wld_start\n"

                   "\tmovl 4(%ebp),%edi\n"
                   "\tdecl %edi\n"                  /* argc */
                   "\tleal 12(%ebp),%esi\n"         /* argv */
                   "\tleal 4(%esi,%edi,4),%edx\n"   /* env */
                   "\tmovl %edx,%ecx\n"             /* apple data */
                   "1:\tmovl (%ecx),%ebx\n"
                   "\tadd $4,%ecx\n"
                   "\torl %ebx,%ebx\n"
                   "\tjnz 1b\n"

                   "\tcmpl $0,8(%esp)\n"
                   "\tjne 2f\n"

                   /* LC_MAIN */
                   "\tmovl %edi,0(%esp)\n"          /* argc */
                   "\tmovl %esi,4(%esp)\n"          /* argv */
                   "\tmovl %edx,8(%esp)\n"          /* env */
                   "\tmovl %ecx,12(%esp)\n"         /* apple data */
                   "\tcall *%eax\n"
                   "\tmovl %eax,(%esp)\n"
                   "\tcall _wld_exit\n"
                   "\thlt\n"

                   /* LC_UNIXTHREAD */
                   "2:\tmovl (%ecx),%ebx\n"
                   "\tadd $4,%ecx\n"
                   "\torl %ebx,%ebx\n"
                   "\tjnz 2b\n"

                   "\tsubl %ebp,%ecx\n"
                   "\tsubl $8,%ecx\n"
                   "\tleal 4(%ebp),%esp\n"
                   "\tsubl %ecx,%esp\n"

                   "\tmovl %edi,(%esp)\n"           /* argc */
                   "\tleal 4(%esp),%edi\n"
                   "\tshrl $2,%ecx\n"
                   "\tcld\n"
                   "\trep; movsd\n"                 /* argv, ... */

                   "\tmovl $0,%ebp\n"
                   "\tjmpl *%eax\n" )

#elif defined(__x86_64__)

static const size_t page_size = 0x1000;
static const size_t page_mask = 0xfff;
#define TARGET_CPU_TYPE         CPU_TYPE_X86_64
#define TARGET_MH_MAGIC         MH_MAGIC_64
#define TARGET_SEGMENT_COMMAND  LC_SEGMENT_64
#define target_mach_header      mach_header_64
#define target_segment_command  segment_command_64
#define target_thread_state_t   x86_thread_state64_t
#ifdef __DARWIN_UNIX03
#define target_thread_ip(x)     (x)->__rip
#else
#define target_thread_ip(x)     (x)->rip
#endif

#define SYSCALL_FUNC( name, nr ) \
    __ASM_GLOBAL_FUNC( name, \
                       "\tmovq %rcx, %r10\n" \
                       "\tmovq $(" #nr "|0x2000000),%rax\n" \
                       "\tsyscall\n" \
                       "\tjnb 1f\n" \
                       "\tmovq $-1,%rax\n" \
                       "1:\tret\n" )

#define SYSCALL_NOERR( name, nr ) \
    __ASM_GLOBAL_FUNC( name, \
                       "\tmovq %rcx, %r10\n" \
                       "\tmovq $(" #nr "|0x2000000),%rax\n" \
                       "\tsyscall\n" \
                       "\tret\n" )

__ASM_GLOBAL_FUNC( start,
                   __ASM_CFI("\t.cfi_undefined %rip\n")
                   "\tpushq $0\n"
                   "\tmovq %rsp,%rbp\n"
                   "\tandq $-16,%rsp\n"
                   "\tsubq $16,%rsp\n"

                   "\tleaq 8(%rbp),%rdi\n"          /* stack */
                   "\tmovq %rsp,%rsi\n"             /* &is_unix_thread */
                   "\tmovq $0,(%rsi)\n"
                   "\tcall _wld_start\n"

                   "\tmovq 8(%rbp),%rdi\n"
                   "\tdec %rdi\n"                   /* argc */
                   "\tleaq 24(%rbp),%rsi\n"         /* argv */
                   "\tleaq 8(%rsi,%rdi,8),%rdx\n"   /* env */
                   "\tmovq %rdx,%rcx\n"             /* apple data */
                   "1:\tmovq (%rcx),%r8\n"
                   "\taddq $8,%rcx\n"
                   "\torq %r8,%r8\n"
                   "\tjnz 1b\n"

                   "\tcmpl $0,0(%rsp)\n"
                   "\tjne 2f\n"

                   /* LC_MAIN */
                   "\taddq $16,%rsp\n"
                   "\tcall *%rax\n"
                   "\tmovq %rax,%rdi\n"
                   "\tcall _wld_exit\n"
                   "\thlt\n"

                   /* LC_UNIXTHREAD */
                   "2:\tmovq (%rcx),%r8\n"
                   "\taddq $8,%rcx\n"
                   "\torq %r8,%r8\n"
                   "\tjnz 2b\n"

                   "\tsubq %rbp,%rcx\n"
                   "\tsubq $16,%rcx\n"
                   "\tleaq 8(%rbp),%rsp\n"
                   "\tsubq %rcx,%rsp\n"

                   "\tmovq %rdi,(%rsp)\n"           /* argc */
                   "\tleaq 8(%rsp),%rdi\n"
                   "\tshrq $3,%rcx\n"
                   "\tcld\n"
                   "\trep; movsq\n"                 /* argv, ... */

                   "\tmovq $0,%rbp\n"
                   "\tjmpq *%rax\n" )

#else
#error preloader not implemented for this CPU
#endif

void wld_exit( int code ) __attribute__((noreturn));
SYSCALL_NOERR( wld_exit, 1 /* SYS_exit */ );

ssize_t wld_write( int fd, const void *buffer, size_t len );
SYSCALL_FUNC( wld_write, 4 /* SYS_write */ );

void *wld_mmap( void *start, size_t len, int prot, int flags, int fd, off_t offset );
SYSCALL_FUNC( wld_mmap, 197 /* SYS_mmap */ );

void *wld_munmap( void *start, size_t len );
SYSCALL_FUNC( wld_munmap, 73 /* SYS_munmap */ );

int wld_mincore( void *addr, size_t length, unsigned char *vec );
SYSCALL_FUNC( wld_mincore, 78 /* SYS_mincore */ );

#else  /* __APPLE__ */
#ifdef __i386__

/* data for setting up the glibc-style thread-local storage in %gs */

static int thread_data[256];

struct
{
    /* this is the kernel modify_ldt struct */
    unsigned int  entry_number;
    unsigned long base_addr;
    unsigned int  limit;
    unsigned int  seg_32bit : 1;
    unsigned int  contents : 2;
    unsigned int  read_exec_only : 1;
    unsigned int  limit_in_pages : 1;
    unsigned int  seg_not_present : 1;
    unsigned int  usable : 1;
    unsigned int  garbage : 25;
} thread_ldt = { -1, (unsigned long)thread_data, 0xfffff, 1, 0, 0, 1, 0, 1, 0 };


/*
 * The _start function is the entry and exit point of this program
 *
 *  It calls wld_start, passing a pointer to the args it receives
 *  then jumps to the address wld_start returns.
 */
void _start(void);
extern char _end[];
__ASM_GLOBAL_FUNC(_start,
                  __ASM_CFI("\t.cfi_undefined %eip\n")
                  "\tmovl $243,%eax\n"        /* SYS_set_thread_area */
                  "\tmovl $thread_ldt,%ebx\n"
                  "\tint $0x80\n"             /* allocate gs segment */
                  "\torl %eax,%eax\n"
                  "\tjl 1f\n"
                  "\tmovl thread_ldt,%eax\n"  /* thread_ldt.entry_number */
                  "\tshl $3,%eax\n"
                  "\torl $3,%eax\n"
                  "\tmov %ax,%gs\n"
                  "\tmov %ax,%fs\n"           /* set %fs too so libwine can retrieve it later on */
                  "1:\tmovl %esp,%eax\n"
                  "\tleal -136(%esp),%esp\n"  /* allocate some space for extra aux values */
                  "\tpushl %eax\n"            /* orig stack pointer */
                  "\tpushl %esp\n"            /* ptr to orig stack pointer */
                  "\tcall wld_start\n"
                  "\tpopl %ecx\n"             /* remove ptr to stack pointer */
                  "\tpopl %esp\n"             /* new stack pointer */
                  "\tpush %eax\n"             /* ELF interpreter entry point */
                  "\txor %eax,%eax\n"
                  "\txor %ecx,%ecx\n"
                  "\txor %edx,%edx\n"
                  "\tmov %ax,%gs\n"           /* clear %gs again */
                  "\tret\n")

/* wrappers for Linux system calls */

#define SYSCALL_RET(ret) (((ret) < 0 && (ret) > -4096) ? -1 : (ret))

static inline __attribute__((noreturn)) void wld_exit( int code )
{
    for (;;)  /* avoid warning */
        __asm__ __volatile__( "pushl %%ebx; movl %1,%%ebx; int $0x80; popl %%ebx"
                              : : "a" (1 /* SYS_exit */), "r" (code) );
}

static inline int wld_open( const char *name, int flags )
{
    int ret;
    __asm__ __volatile__( "pushl %%ebx; movl %2,%%ebx; int $0x80; popl %%ebx"
                          : "=a" (ret) : "0" (5 /* SYS_open */), "r" (name), "c" (flags) );
    return SYSCALL_RET(ret);
}

static inline int wld_close( int fd )
{
    int ret;
    __asm__ __volatile__( "pushl %%ebx; movl %2,%%ebx; int $0x80; popl %%ebx"
                          : "=a" (ret) : "0" (6 /* SYS_close */), "r" (fd) );
    return SYSCALL_RET(ret);
}

static inline ssize_t wld_read( int fd, void *buffer, size_t len )
{
    int ret;
    __asm__ __volatile__( "pushl %%ebx; movl %2,%%ebx; int $0x80; popl %%ebx"
                          : "=a" (ret)
                          : "0" (3 /* SYS_read */), "r" (fd), "c" (buffer), "d" (len)
                          : "memory" );
    return SYSCALL_RET(ret);
}

static inline ssize_t wld_write( int fd, const void *buffer, size_t len )
{
    int ret;
    __asm__ __volatile__( "pushl %%ebx; movl %2,%%ebx; int $0x80; popl %%ebx"
                          : "=a" (ret) : "0" (4 /* SYS_write */), "r" (fd), "c" (buffer), "d" (len) );
    return SYSCALL_RET(ret);
}

static inline int wld_mprotect( const void *addr, size_t len, int prot )
{
    int ret;
    __asm__ __volatile__( "pushl %%ebx; movl %2,%%ebx; int $0x80; popl %%ebx"
                          : "=a" (ret) : "0" (125 /* SYS_mprotect */), "r" (addr), "c" (len), "d" (prot) );
    return SYSCALL_RET(ret);
}

void *wld_mmap( void *start, size_t len, int prot, int flags, int fd, unsigned int offset );
__ASM_GLOBAL_FUNC(wld_mmap,
                  "\tpushl %ebp\n"
                  __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t")
                  "\tpushl %ebx\n"
                  __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t")
                  "\tpushl %esi\n"
                  __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t")
                  "\tpushl %edi\n"
                  __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t")
                  "\tmovl $192,%eax\n"      /* SYS_mmap2 */
                  "\tmovl 20(%esp),%ebx\n"  /* start */
                  "\tmovl 24(%esp),%ecx\n"  /* len */
                  "\tmovl 28(%esp),%edx\n"  /* prot */
                  "\tmovl 32(%esp),%esi\n"  /* flags */
                  "\tmovl 36(%esp),%edi\n"  /* fd */
                  "\tmovl 40(%esp),%ebp\n"  /* offset */
                  "\tshrl $12,%ebp\n"
                  "\tint $0x80\n"
                  "\tcmpl $-4096,%eax\n"
                  "\tjbe 2f\n"
                  "\tcmpl $-38,%eax\n"      /* ENOSYS */
                  "\tjne 1f\n"
                  "\tmovl $90,%eax\n"       /* SYS_mmap */
                  "\tleal 20(%esp),%ebx\n"
                  "\tint $0x80\n"
                  "\tcmpl $-4096,%eax\n"
                  "\tjbe 2f\n"
                  "1:\tmovl $-1,%eax\n"
                  "2:\tpopl %edi\n"
                  __ASM_CFI(".cfi_adjust_cfa_offset -4\n\t")
                  "\tpopl %esi\n"
                  __ASM_CFI(".cfi_adjust_cfa_offset -4\n\t")
                  "\tpopl %ebx\n"
                  __ASM_CFI(".cfi_adjust_cfa_offset -4\n\t")
                  "\tpopl %ebp\n"
                  __ASM_CFI(".cfi_adjust_cfa_offset -4\n\t")
                  "\tret\n" )

static inline uid_t wld_getuid(void)
{
    uid_t ret;
    __asm__( "int $0x80" : "=a" (ret) : "0" (24 /* SYS_getuid */) );
    return ret;
}

static inline uid_t wld_geteuid(void)
{
    uid_t ret;
    __asm__( "int $0x80" : "=a" (ret) : "0" (49 /* SYS_geteuid */) );
    return ret;
}

static inline gid_t wld_getgid(void)
{
    gid_t ret;
    __asm__( "int $0x80" : "=a" (ret) : "0" (47 /* SYS_getgid */) );
    return ret;
}

static inline gid_t wld_getegid(void)
{
    gid_t ret;
    __asm__( "int $0x80" : "=a" (ret) : "0" (50 /* SYS_getegid */) );
    return ret;
}

static inline int wld_prctl( int code, long arg )
{
    int ret;
    __asm__ __volatile__( "pushl %%ebx; movl %2,%%ebx; int $0x80; popl %%ebx"
                          : "=a" (ret) : "0" (172 /* SYS_prctl */), "r" (code), "c" (arg) );
    return SYSCALL_RET(ret);
}

#elif defined(__x86_64__)

void *thread_data[256];

/*
 * The _start function is the entry and exit point of this program
 *
 *  It calls wld_start, passing a pointer to the args it receives
 *  then jumps to the address wld_start returns.
 */
void _start(void);
extern char _end[];
__ASM_GLOBAL_FUNC(_start,
                  __ASM_CFI(".cfi_undefined %rip\n\t")
                  "movq %rsp,%rax\n\t"
                  "leaq -144(%rsp),%rsp\n\t" /* allocate some space for extra aux values */
                  "movq %rax,(%rsp)\n\t"     /* orig stack pointer */
                  "movq $thread_data,%rsi\n\t"
                  "movq $0x1002,%rdi\n\t"    /* ARCH_SET_FS */
                  "movq $158,%rax\n\t"       /* SYS_arch_prctl */
                  "syscall\n\t"
                  "movq %rsp,%rdi\n\t"       /* ptr to orig stack pointer */
                  "call wld_start\n\t"
                  "movq (%rsp),%rsp\n\t"     /* new stack pointer */
                  "pushq %rax\n\t"           /* ELF interpreter entry point */
                  "xorq %rax,%rax\n\t"
                  "xorq %rcx,%rcx\n\t"
                  "xorq %rdx,%rdx\n\t"
                  "xorq %rsi,%rsi\n\t"
                  "xorq %rdi,%rdi\n\t"
                  "xorq %r8,%r8\n\t"
                  "xorq %r9,%r9\n\t"
                  "xorq %r10,%r10\n\t"
                  "xorq %r11,%r11\n\t"
                  "ret")

#define SYSCALL_FUNC( name, nr ) \
    __ASM_GLOBAL_FUNC( name, \
                       "movq $" #nr ",%rax\n\t" \
                       "movq %rcx,%r10\n\t" \
                       "syscall\n\t" \
                       "leaq 4096(%rax),%rcx\n\t" \
                       "movq $-1,%rdx\n\t" \
                       "cmp $4096,%rcx\n\t" \
                       "cmovb %rdx,%rax\n\t" \
                       "ret" )

#define SYSCALL_NOERR( name, nr ) \
    __ASM_GLOBAL_FUNC( name, \
                       "movq $" #nr ",%rax\n\t" \
                       "syscall\n\t" \
                       "ret" )

void wld_exit( int code ) __attribute__((noreturn));
SYSCALL_NOERR( wld_exit, 60 /* SYS_exit */ );

ssize_t wld_read( int fd, void *buffer, size_t len );
SYSCALL_FUNC( wld_read, 0 /* SYS_read */ );

ssize_t wld_write( int fd, const void *buffer, size_t len );
SYSCALL_FUNC( wld_write, 1 /* SYS_write */ );

int wld_open( const char *name, int flags );
SYSCALL_FUNC( wld_open, 2 /* SYS_open */ );

int wld_close( int fd );
SYSCALL_FUNC( wld_close, 3 /* SYS_close */ );

void *wld_mmap( void *start, size_t len, int prot, int flags, int fd, off_t offset );
SYSCALL_FUNC( wld_mmap, 9 /* SYS_mmap */ );

int wld_mprotect( const void *addr, size_t len, int prot );
SYSCALL_FUNC( wld_mprotect, 10 /* SYS_mprotect */ );

int wld_prctl( int code, long arg );
SYSCALL_FUNC( wld_prctl, 157 /* SYS_prctl */ );

uid_t wld_getuid(void);
SYSCALL_NOERR( wld_getuid, 102 /* SYS_getuid */ );

gid_t wld_getgid(void);
SYSCALL_NOERR( wld_getgid, 104 /* SYS_getgid */ );

uid_t wld_geteuid(void);
SYSCALL_NOERR( wld_geteuid, 107 /* SYS_geteuid */ );

gid_t wld_getegid(void);
SYSCALL_NOERR( wld_getegid, 108 /* SYS_getegid */ );

#else
#error preloader not implemented for this CPU
#endif
#endif /* __APPLE__ */

/* replacement for libc functions */

static inline int wld_strcmp( const char *str1, const char *str2 )
{
    while (*str1 && (*str1 == *str2)) { str1++; str2++; }
    return *str1 - *str2;
}

static inline int wld_strncmp( const char *str1, const char *str2, size_t len )
{
    if (len <= 0) return 0;
    while ((--len > 0) && *str1 && (*str1 == *str2)) { str1++; str2++; }
    return *str1 - *str2;
}

static inline void *wld_memset( void *dest, int val, size_t len )
{
    char *dst = dest;
    while (len--) *dst++ = val;
    return dest;
}

/*
 * wld_printf - just the basics
 *
 *  %x prints a hex number
 *  %s prints a string
 *  %p prints a pointer
 */
static int wld_vsprintf(char *buffer, const char *fmt, va_list args )
{
    static const char hex_chars[16] = "0123456789abcdef";
    const char *p = fmt;
    char *str = buffer;
    int i;

    while( *p )
    {
        if( *p == '%' )
        {
            p++;
            if( *p == 'x' )
            {
                unsigned int x = va_arg( args, unsigned int );
                for (i = 2*sizeof(x) - 1; i >= 0; i--)
                    *str++ = hex_chars[(x>>(i*4))&0xf];
            }
            else if (p[0] == 'l' && p[1] == 'x')
            {
                unsigned long x = va_arg( args, unsigned long );
                for (i = 2*sizeof(x) - 1; i >= 0; i--)
                    *str++ = hex_chars[(x>>(i*4))&0xf];
                p++;
            }
            else if( *p == 'p' )
            {
                unsigned long x = (unsigned long)va_arg( args, void * );
                for (i = 2*sizeof(x) - 1; i >= 0; i--)
                    *str++ = hex_chars[(x>>(i*4))&0xf];
            }
            else if( *p == 's' )
            {
                char *s = va_arg( args, char * );
                while(*s)
                    *str++ = *s++;
            }
            else if( *p == 0 )
                break;
            p++;
        }
        *str++ = *p++;
    }
    *str = 0;
    return str - buffer;
}

static __attribute__((format(printf,1,2))) void wld_printf(const char *fmt, ... )
{
    va_list args;
    char buffer[256];
    int len;

    va_start( args, fmt );
    len = wld_vsprintf(buffer, fmt, args );
    va_end( args );
    wld_write(2, buffer, len);
}

static __attribute__((noreturn,format(printf,1,2))) void fatal_error(const char *fmt, ... )
{
    va_list args;
    char buffer[256];
    int len;

    va_start( args, fmt );
    len = wld_vsprintf(buffer, fmt, args );
    va_end( args );
    wld_write(2, buffer, len);
    wld_exit(1);
}

#ifndef __APPLE__

#ifdef DUMP_AUX_INFO
/*
 *  Dump interesting bits of the ELF auxv_t structure that is passed
 *   as the 4th parameter to the _start function
 */
static void dump_auxiliary( struct wld_auxv *av )
{
#define NAME(at) { at, #at }
    static const struct { int val; const char *name; } names[] =
    {
        NAME(AT_BASE),
        NAME(AT_CLKTCK),
        NAME(AT_EGID),
        NAME(AT_ENTRY),
        NAME(AT_EUID),
        NAME(AT_FLAGS),
        NAME(AT_GID),
        NAME(AT_HWCAP),
        NAME(AT_PAGESZ),
        NAME(AT_PHDR),
        NAME(AT_PHENT),
        NAME(AT_PHNUM),
        NAME(AT_PLATFORM),
        NAME(AT_SYSINFO),
        NAME(AT_SYSINFO_EHDR),
        NAME(AT_UID),
        { 0, NULL }
    };
#undef NAME

    int i;

    for (  ; av->a_type != AT_NULL; av++)
    {
        for (i = 0; names[i].name; i++) if (names[i].val == av->a_type) break;
        if (names[i].name) wld_printf("%s = %lx\n", names[i].name, (unsigned long)av->a_un.a_val);
        else wld_printf( "%lx = %lx\n", (unsigned long)av->a_type, (unsigned long)av->a_un.a_val );
    }
}
#endif

/*
 * set_auxiliary_values
 *
 * Set the new auxiliary values
 */
static void set_auxiliary_values( struct wld_auxv *av, const struct wld_auxv *new_av,
                                  const struct wld_auxv *delete_av, void **stack )
{
    int i, j, av_count = 0, new_count = 0, delete_count = 0;
    char *src, *dst;

    /* count how many aux values we have already */
    while (av[av_count].a_type != AT_NULL) av_count++;

    /* delete unwanted values */
    for (j = 0; delete_av[j].a_type != AT_NULL; j++)
    {
        for (i = 0; i < av_count; i++) if (av[i].a_type == delete_av[j].a_type)
        {
            av[i].a_type = av[av_count-1].a_type;
            av[i].a_un.a_val = av[av_count-1].a_un.a_val;
            av[--av_count].a_type = AT_NULL;
            delete_count++;
            break;
        }
    }

    /* count how many values we have in new_av that aren't in av */
    for (j = 0; new_av[j].a_type != AT_NULL; j++)
    {
        for (i = 0; i < av_count; i++) if (av[i].a_type == new_av[j].a_type) break;
        if (i == av_count) new_count++;
    }

    src = (char *)*stack;
    dst = src - (new_count - delete_count) * sizeof(*av);
    dst = (char *)((unsigned long)dst & ~15);
    if (dst < src)   /* need to make room for the extra values */
    {
        int len = (char *)(av + av_count + 1) - src;
        for (i = 0; i < len; i++) dst[i] = src[i];
    }
    else if (dst > src)  /* get rid of unused values */
    {
        int len = (char *)(av + av_count + 1) - src;
        for (i = len - 1; i >= 0; i--) dst[i] = src[i];
    }
    *stack = dst;
    av = (struct wld_auxv *)((char *)av + (dst - src));

    /* now set the values */
    for (j = 0; new_av[j].a_type != AT_NULL; j++)
    {
        for (i = 0; i < av_count; i++) if (av[i].a_type == new_av[j].a_type) break;
        if (i < av_count) av[i].a_un.a_val = new_av[j].a_un.a_val;
        else
        {
            av[av_count].a_type     = new_av[j].a_type;
            av[av_count].a_un.a_val = new_av[j].a_un.a_val;
            av_count++;
        }
    }

#ifdef DUMP_AUX_INFO
    wld_printf("New auxiliary info:\n");
    dump_auxiliary( av );
#endif
}

/*
 * get_auxiliary
 *
 * Get a field of the auxiliary structure
 */
static int get_auxiliary( struct wld_auxv *av, int type, int def_val )
{
  for ( ; av->a_type != AT_NULL; av++)
      if( av->a_type == type ) return av->a_un.a_val;
  return def_val;
}

/*
 * map_so_lib
 *
 * modelled after _dl_map_object_from_fd() from glibc-2.3.1/elf/dl-load.c
 *
 * This function maps the segments from an ELF object, and optionally
 *  stores information about the mapping into the auxv_t structure.
 */
static void map_so_lib( const char *name, struct wld_link_map *l)
{
    int fd;
    unsigned char buf[0x800];
    ElfW(Ehdr) *header = (ElfW(Ehdr)*)buf;
    ElfW(Phdr) *phdr, *ph;
    /* Scan the program header table, collecting its load commands.  */
    struct loadcmd
      {
        ElfW(Addr) mapstart, mapend, dataend, allocend;
        off_t mapoff;
        int prot;
      } loadcmds[16], *c;
    size_t nloadcmds = 0, maplength;

    fd = wld_open( name, O_RDONLY );
    if (fd == -1) fatal_error("%s: could not open\n", name );

    if (wld_read( fd, buf, sizeof(buf) ) != sizeof(buf))
        fatal_error("%s: failed to read ELF header\n", name);

    phdr = (void*) (((unsigned char*)buf) + header->e_phoff);

    if( ( header->e_ident[0] != 0x7f ) ||
        ( header->e_ident[1] != 'E' ) ||
        ( header->e_ident[2] != 'L' ) ||
        ( header->e_ident[3] != 'F' ) )
        fatal_error( "%s: not an ELF binary... don't know how to load it\n", name );

#ifdef __i386__
    if( header->e_machine != EM_386 )
        fatal_error("%s: not an i386 ELF binary... don't know how to load it\n", name );
#elif defined(__x86_64__)
    if( header->e_machine != EM_X86_64 )
        fatal_error("%s: not an x86-64 ELF binary... don't know how to load it\n", name );
#endif

    if (header->e_phnum > sizeof(loadcmds)/sizeof(loadcmds[0]))
        fatal_error( "%s: oops... not enough space for load commands\n", name );

    maplength = header->e_phnum * sizeof (ElfW(Phdr));
    if (header->e_phoff + maplength > sizeof(buf))
        fatal_error( "%s: oops... not enough space for ELF headers\n", name );

    l->l_ld = 0;
    l->l_addr = 0;
    l->l_phdr = 0;
    l->l_phnum = header->e_phnum;
    l->l_entry = header->e_entry;
    l->l_interp = 0;

    for (ph = phdr; ph < &phdr[l->l_phnum]; ++ph)
    {

#ifdef DUMP_SEGMENTS
      wld_printf( "ph = %p\n", ph );
      wld_printf( " p_type   = %lx\n", (unsigned long)ph->p_type );
      wld_printf( " p_flags  = %lx\n", (unsigned long)ph->p_flags );
      wld_printf( " p_offset = %lx\n", (unsigned long)ph->p_offset );
      wld_printf( " p_vaddr  = %lx\n", (unsigned long)ph->p_vaddr );
      wld_printf( " p_paddr  = %lx\n", (unsigned long)ph->p_paddr );
      wld_printf( " p_filesz = %lx\n", (unsigned long)ph->p_filesz );
      wld_printf( " p_memsz  = %lx\n", (unsigned long)ph->p_memsz );
      wld_printf( " p_align  = %lx\n", (unsigned long)ph->p_align );
#endif

      switch (ph->p_type)
        {
          /* These entries tell us where to find things once the file's
             segments are mapped in.  We record the addresses it says
             verbatim, and later correct for the run-time load address.  */
        case PT_DYNAMIC:
          l->l_ld = (void *) ph->p_vaddr;
          l->l_ldnum = ph->p_memsz / sizeof (Elf32_Dyn);
          break;

        case PT_PHDR:
          l->l_phdr = (void *) ph->p_vaddr;
          break;

        case PT_LOAD:
          {
            if ((ph->p_align & page_mask) != 0)
              fatal_error( "%s: ELF load command alignment not page-aligned\n", name );

            if (((ph->p_vaddr - ph->p_offset) & (ph->p_align - 1)) != 0)
              fatal_error( "%s: ELF load command address/offset not properly aligned\n", name );

            c = &loadcmds[nloadcmds++];
            c->mapstart = ph->p_vaddr & ~(ph->p_align - 1);
            c->mapend = ((ph->p_vaddr + ph->p_filesz + page_mask) & ~page_mask);
            c->dataend = ph->p_vaddr + ph->p_filesz;
            c->allocend = ph->p_vaddr + ph->p_memsz;
            c->mapoff = ph->p_offset & ~(ph->p_align - 1);

            c->prot = 0;
            if (ph->p_flags & PF_R)
              c->prot |= PROT_READ;
            if (ph->p_flags & PF_W)
              c->prot |= PROT_WRITE;
            if (ph->p_flags & PF_X)
              c->prot |= PROT_EXEC;
          }
          break;

        case PT_INTERP:
          l->l_interp = ph->p_vaddr;
          break;

        case PT_TLS:
          /*
           * We don't need to set anything up because we're
           * emulating the kernel, not ld-linux.so.2
           * The ELF loader will set up the TLS data itself.
           */
        case PT_SHLIB:
        case PT_NOTE:
        default:
          break;
        }
    }

    /* Now process the load commands and map segments into memory.  */
    if (!nloadcmds)
        fatal_error( "%s: no segments to load\n", name );
    c = loadcmds;

    /* Length of the sections to be loaded.  */
    maplength = loadcmds[nloadcmds - 1].allocend - c->mapstart;

    if( header->e_type == ET_DYN )
    {
        ElfW(Addr) mappref;
        mappref = (ELF_PREFERRED_ADDRESS (loader, maplength, c->mapstart)
                   - MAP_BASE_ADDR (l));

        /* Remember which part of the address space this object uses.  */
        l->l_map_start = (ElfW(Addr)) wld_mmap ((void *) mappref, maplength,
                                              c->prot, MAP_COPY | MAP_FILE,
                                              fd, c->mapoff);
        /* wld_printf("set  : offset = %x\n", c->mapoff); */
        /* wld_printf("l->l_map_start = %x\n", l->l_map_start); */

        l->l_map_end = l->l_map_start + maplength;
        l->l_addr = l->l_map_start - c->mapstart;

        wld_mprotect ((caddr_t) (l->l_addr + c->mapend),
                    loadcmds[nloadcmds - 1].allocend - c->mapend,
                    PROT_NONE);
        goto postmap;
    }
    else
    {
        /* sanity check */
        if ((char *)c->mapstart + maplength > preloader_start &&
            (char *)c->mapstart <= preloader_end)
            fatal_error( "%s: binary overlaps preloader (%p-%p)\n",
                         name, (char *)c->mapstart, (char *)c->mapstart + maplength );

        ELF_FIXED_ADDRESS (loader, c->mapstart);
    }

    /* Remember which part of the address space this object uses.  */
    l->l_map_start = c->mapstart + l->l_addr;
    l->l_map_end = l->l_map_start + maplength;

    while (c < &loadcmds[nloadcmds])
      {
        if (c->mapend > c->mapstart)
            /* Map the segment contents from the file.  */
            wld_mmap ((void *) (l->l_addr + c->mapstart),
                        c->mapend - c->mapstart, c->prot,
                        MAP_FIXED | MAP_COPY | MAP_FILE, fd, c->mapoff);

      postmap:
        if (l->l_phdr == 0
            && (ElfW(Off)) c->mapoff <= header->e_phoff
            && ((size_t) (c->mapend - c->mapstart + c->mapoff)
                >= header->e_phoff + header->e_phnum * sizeof (ElfW(Phdr))))
          /* Found the program header in this segment.  */
          l->l_phdr = (void *)(unsigned long)(c->mapstart + header->e_phoff - c->mapoff);

        if (c->allocend > c->dataend)
          {
            /* Extra zero pages should appear at the end of this segment,
               after the data mapped from the file.   */
            ElfW(Addr) zero, zeroend, zeropage;

            zero = l->l_addr + c->dataend;
            zeroend = l->l_addr + c->allocend;
            zeropage = (zero + page_mask) & ~page_mask;

            /*
             * This is different from the dl-load load...
             *  ld-linux.so.2 relies on the whole page being zero'ed
             */
            zeroend = (zeroend + page_mask) & ~page_mask;

            if (zeroend < zeropage)
            {
              /* All the extra data is in the last page of the segment.
                 We can just zero it.  */
              zeropage = zeroend;
            }

            if (zeropage > zero)
              {
                /* Zero the final part of the last page of the segment.  */
                if ((c->prot & PROT_WRITE) == 0)
                  {
                    /* Dag nab it.  */
                    wld_mprotect ((caddr_t) (zero & ~page_mask), page_size, c->prot|PROT_WRITE);
                  }
                wld_memset ((void *) zero, '\0', zeropage - zero);
                if ((c->prot & PROT_WRITE) == 0)
                  wld_mprotect ((caddr_t) (zero & ~page_mask), page_size, c->prot);
              }

            if (zeroend > zeropage)
              {
                /* Map the remaining zero pages in from the zero fill FD.  */
                wld_mmap ((caddr_t) zeropage, zeroend - zeropage,
                                c->prot, MAP_ANON|MAP_PRIVATE|MAP_FIXED,
                                -1, 0);
              }
          }

        ++c;
      }

    if (l->l_phdr == NULL) fatal_error("no program header\n");

    l->l_phdr = (void *)((ElfW(Addr))l->l_phdr + l->l_addr);
    l->l_entry += l->l_addr;

    wld_close( fd );
}


static unsigned int wld_elf_hash( const char *name )
{
    unsigned int hi, hash = 0;
    while (*name)
    {
        hash = (hash << 4) + (unsigned char)*name++;
        hi = hash & 0xf0000000;
        hash ^= hi;
        hash ^= hi >> 24;
    }
    return hash;
}

static unsigned int gnu_hash( const char *name )
{
    unsigned int h = 5381;
    while (*name) h = h * 33 + (unsigned char)*name++;
    return h;
}

/*
 * Find a symbol in the symbol table of the executable loaded
 */
static void *find_symbol( const struct wld_link_map *map, const char *var, int type )
{
    const ElfW(Dyn) *dyn = NULL;
    const ElfW(Phdr) *ph;
    const ElfW(Sym) *symtab = NULL;
    const Elf32_Word *hashtab = NULL;
    const Elf32_Word *gnu_hashtab = NULL;
    const char *strings = NULL;
    Elf32_Word idx;

    /* check the values */
#ifdef DUMP_SYMS
    wld_printf("%p %x\n", map->l_phdr, map->l_phnum );
#endif
    /* parse the (already loaded) ELF executable's header */
    for (ph = map->l_phdr; ph < &map->l_phdr[map->l_phnum]; ++ph)
    {
        if( PT_DYNAMIC == ph->p_type )
        {
            dyn = (void *)(ph->p_vaddr + map->l_addr);
            break;
        }
    }
    if( !dyn ) return NULL;

    while( dyn->d_tag )
    {
        if( dyn->d_tag == DT_STRTAB )
            strings = (const char*)(dyn->d_un.d_ptr + map->l_addr);
        if( dyn->d_tag == DT_SYMTAB )
            symtab = (const ElfW(Sym) *)(dyn->d_un.d_ptr + map->l_addr);
        if( dyn->d_tag == DT_HASH )
            hashtab = (const Elf32_Word *)(dyn->d_un.d_ptr + map->l_addr);
        if( dyn->d_tag == DT_GNU_HASH )
            gnu_hashtab = (const Elf32_Word *)(dyn->d_un.d_ptr + map->l_addr);
#ifdef DUMP_SYMS
        wld_printf("%lx %p\n", (unsigned long)dyn->d_tag, (void *)dyn->d_un.d_ptr );
#endif
        dyn++;
    }

    if( (!symtab) || (!strings) ) return NULL;

    if (gnu_hashtab)  /* new style hash table */
    {
        const unsigned int hash   = gnu_hash(var);
        const Elf32_Word nbuckets = gnu_hashtab[0];
        const Elf32_Word symbias  = gnu_hashtab[1];
        const Elf32_Word nwords   = gnu_hashtab[2];
        const ElfW(Addr) *bitmask = (const ElfW(Addr) *)(gnu_hashtab + 4);
        const Elf32_Word *buckets = (const Elf32_Word *)(bitmask + nwords);
        const Elf32_Word *chains  = buckets + nbuckets - symbias;

        if (!(idx = buckets[hash % nbuckets])) return NULL;
        do
        {
            if ((chains[idx] & ~1u) == (hash & ~1u) &&
                ELF32_ST_BIND(symtab[idx].st_info) == STB_GLOBAL &&
                ELF32_ST_TYPE(symtab[idx].st_info) == type &&
                !wld_strcmp( strings + symtab[idx].st_name, var ))
                goto found;
        } while (!(chains[idx++] & 1u));
    }
    else if (hashtab)  /* old style hash table */
    {
        const unsigned int hash   = wld_elf_hash(var);
        const Elf32_Word nbuckets = hashtab[0];
        const Elf32_Word *buckets = hashtab + 2;
        const Elf32_Word *chains  = buckets + nbuckets;

        for (idx = buckets[hash % nbuckets]; idx; idx = chains[idx])
        {
            if (ELF32_ST_BIND(symtab[idx].st_info) == STB_GLOBAL &&
                ELF32_ST_TYPE(symtab[idx].st_info) == type &&
                !wld_strcmp( strings + symtab[idx].st_name, var ))
                goto found;
        }
    }
    return NULL;

found:
#ifdef DUMP_SYMS
    wld_printf("Found %s -> %p\n", strings + symtab[idx].st_name, (void *)symtab[idx].st_value );
#endif
    return (void *)(symtab[idx].st_value + map->l_addr);
}

#endif /* !__APPLE__ */

/*
 *  preload_reserve
 *
 * Reserve a range specified in string format
 */
static void preload_reserve( const char *str )
{
    const char *p;
    unsigned long result = 0;
    void *start = NULL, *end = NULL;
    int i, first = 1;

    for (p = str; *p; p++)
    {
        if (*p >= '0' && *p <= '9') result = result * 16 + *p - '0';
        else if (*p >= 'a' && *p <= 'f') result = result * 16 + *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') result = result * 16 + *p - 'A' + 10;
        else if (*p == '-')
        {
            if (!first) goto error;
            start = (void *)(result & ~page_mask);
            result = 0;
            first = 0;
        }
        else goto error;
    }
    if (!first) end = (void *)((result + page_mask) & ~page_mask);
    else if (result) goto error;  /* single value '0' is allowed */

    /* sanity checks */
    if (end <= start) start = end = NULL;
#ifndef __APPLE__
    else if ((char *)end > preloader_start &&
             (char *)start <= preloader_end)
    {
        wld_printf( "WINEPRELOADRESERVE range %p-%p overlaps preloader %p-%p\n",
                     start, end, preloader_start, preloader_end );
        start = end = NULL;
    }
#endif /* !__APPLE__ */

    /* check for overlap with low memory areas */
    for (i = 0; preload_info[i].size; i++)
    {
        if ((char *)preload_info[i].addr > (char *)0x00110000) break;
        if ((char *)end <= (char *)preload_info[i].addr + preload_info[i].size)
        {
            start = end = NULL;
            break;
        }
        if ((char *)start < (char *)preload_info[i].addr + preload_info[i].size)
            start = (char *)preload_info[i].addr + preload_info[i].size;
    }

    while (preload_info[i].size) i++;
    preload_info[i].addr = start;
    preload_info[i].size = (char *)end - (char *)start;
    return;

error:
    fatal_error( "invalid WINEPRELOADRESERVE value '%s'\n", str );
}

/* check if address is in one of the reserved ranges */
static inline int is_addr_reserved( const void *addr )
{
    int i;

    for (i = 0; preload_info[i].size; i++)
    {
        if ((const char *)addr >= (const char *)preload_info[i].addr &&
            (const char *)addr <  (const char *)preload_info[i].addr + preload_info[i].size)
            return 1;
    }
    return 0;
}

/* remove a range from the preload list */
static void remove_preload_range( int i )
{
    while (preload_info[i].size)
    {
        preload_info[i].addr = preload_info[i+1].addr;
        preload_info[i].size = preload_info[i+1].size;
        i++;
    }
}

#ifdef __APPLE__

#define MAKE_FUNCPTR(f) static typeof(f) * p##f = NULL
MAKE_FUNCPTR(dlopen);
MAKE_FUNCPTR(dlsym);
MAKE_FUNCPTR(_dyld_image_count);
MAKE_FUNCPTR(_dyld_get_image_header);
MAKE_FUNCPTR(_dyld_get_image_vmaddr_slide);
#undef MAKE_FUNCPTR

extern int _dyld_func_lookup( const char *dyld_func_name, void **address );

static struct target_mach_header *find_executable( intptr_t *slide )
{
    struct target_mach_header *mh;
    int i;

    /* skip our own executable */
    for (i = 1; i < p_dyld_image_count(); i++)
    {
        mh = (struct target_mach_header *)p_dyld_get_image_header( i );
        if (!mh) continue;
        if (mh->magic != TARGET_MH_MAGIC) continue;
        if (mh->cputype != TARGET_CPU_TYPE) continue;
        if (mh->filetype != MH_EXECUTE) continue;

        *slide = p_dyld_get_image_vmaddr_slide( i );
        return mh;
    }

    return NULL;
}

static void *get_entry_point( struct target_mach_header *mh, intptr_t slide, int *unix_thread )
{
    struct entry_point_command *entry;
    target_thread_state_t *state;
    struct load_command *cmd;
    int i;

    /* try LC_MAIN first */
    cmd = (struct load_command *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++)
    {
        if (cmd->cmd == LC_MAIN)
        {
            *unix_thread = FALSE;
            entry = (struct entry_point_command *)cmd;
            return (char *)mh + entry->entryoff;
        }
        cmd = (struct load_command *)((char *)cmd + cmd->cmdsize);
    }

    /* then try LC_UNIXTHREAD */
    cmd = (struct load_command *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++)
    {
        if (cmd->cmd == LC_UNIXTHREAD)
        {
            *unix_thread = TRUE;
            state = (target_thread_state_t *)((char *)cmd + 16);
            return (void *)(target_thread_ip(state) + slide);
        }
        cmd = (struct load_command *)((char *)cmd + cmd->cmdsize);
    }

    return NULL;
};

static int is_region_empty( struct wine_preload_info *info )
{
    unsigned char vec[1024];
    size_t pos, size, block = 1024 * page_size;
    int i;

    for (pos = 0; pos < info->size; pos += size)
    {
        size = (pos + block <= info->size) ? block : (info->size - pos);
        if (wld_mincore( (char *)info->addr + pos, size, vec ) == -1)
        {
            if (size <= page_size) continue;
            block = page_size; size = 0;  /* retry with smaller block size */
        }
        else
        {
            for (i = 0; i < size / page_size; i++)
                if (vec[i] & 1) return 0;
        }
    }

    return 1;
}

static int map_region( struct wine_preload_info *info )
{
    int flags = MAP_PRIVATE | MAP_ANON;
    void *ret;

    if (!info->addr) flags |= MAP_FIXED;

    for (;;)
    {
        ret = wld_mmap( info->addr, info->size, PROT_NONE, flags, -1, 0 );
        if (ret == info->addr) return 1;
        if (ret != (void *)-1) wld_munmap( ret, info->size );
        if (flags & MAP_FIXED) break;

        /* Some versions of macOS ignore the address hint passed to mmap -
         * use mincore() to check if its empty and then use MAP_FIXED */
        if (!is_region_empty( info )) break;
        flags |= MAP_FIXED;
    }

    wld_printf( "preloader: Warning: failed to reserve range %p-%p\n",
                info->addr, (char *)info->addr + info->size );
    return 0;
}

static inline void get_dyld_func( const char *name, void **func )
{
    _dyld_func_lookup( name, func );
    if (!*func) fatal_error( "Failed to get function pointer for %s\n", name );
}

#define LOAD_POSIX_DYLD_FUNC(f) get_dyld_func( "__dyld_" #f, (void **)&p##f )
#define LOAD_MACHO_DYLD_FUNC(f) get_dyld_func( "_" #f, (void **)&p##f )

void *wld_start( void *stack, int *is_unix_thread )
{
    struct wine_preload_info builtin_dlls = { (void *)0x7a000000, 0x02000000 };
    struct wine_preload_info **wine_main_preload_info;
    char **argv, **p, *reserve = NULL;
    struct target_mach_header *mh;
    void *mod, *entry;
    intptr_t slide;
    int *pargc, i;

    pargc = stack;
    argv = (char **)pargc + 1;
    if (*pargc < 2) fatal_error( "Usage: %s wine_binary [args]\n", argv[0] );

    /* skip over the parameters */
    p = argv + *pargc + 1;

    /* skip over the environment */
    while (*p)
    {
        static const char res[] = "WINEPRELOADRESERVE=";
        if (!wld_strncmp( *p, res, sizeof(res)-1 )) reserve = *p + sizeof(res) - 1;
        p++;
    }

    /* reserve memory that Wine needs */
    if (reserve) preload_reserve( reserve );
    for (i = 0; preload_info[i].size; i++)
    {
        if (!map_region( &preload_info[i] ))
        {
            remove_preload_range( i );
            i--;
        }
    }

    if (!map_region( &builtin_dlls ))
        builtin_dlls.size = 0;

    LOAD_POSIX_DYLD_FUNC( dlopen );
    LOAD_POSIX_DYLD_FUNC( dlsym );
    LOAD_MACHO_DYLD_FUNC( _dyld_image_count );
    LOAD_MACHO_DYLD_FUNC( _dyld_get_image_header );
    LOAD_MACHO_DYLD_FUNC( _dyld_get_image_vmaddr_slide );

    /* load the main binary */
    if (!(mod = pdlopen( argv[1], RTLD_NOW )))
        fatal_error( "%s: could not load binary\n", argv[1] );

    if (builtin_dlls.size)
        wld_munmap( builtin_dlls.addr, builtin_dlls.size );

    /* store pointer to the preload info into the appropriate main binary variable */
    wine_main_preload_info = pdlsym( mod, "wine_main_preload_info" );
    if (wine_main_preload_info) *wine_main_preload_info = preload_info;
    else wld_printf( "wine_main_preload_info not found\n" );

    /* there is no way to translate the dlopen handle to the mach header :-( */
    if (!(mh = find_executable( &slide )))
        fatal_error( "%s: could not find mach header\n", argv[1] );
    if (!(entry = get_entry_point( mh, slide, is_unix_thread )))
        fatal_error( "%s: could not find entry point\n", argv[1] );

    return entry;
}

#else  /* __APPLE__ */

/*
 *  is_in_preload_range
 *
 * Check if address of the given aux value is in one of the reserved ranges
 */
static int is_in_preload_range( const struct wld_auxv *av, int type )
{
    while (av->a_type != AT_NULL)
    {
        if (av->a_type == type) return is_addr_reserved( (const void *)av->a_un.a_val );
        av++;
    }
    return 0;
}

/* set the process name if supported */
static void set_process_name( int argc, char *argv[] )
{
    int i;
    unsigned int off;
    char *p, *name, *end;

    /* set the process short name */
    for (p = name = argv[1]; *p; p++) if (p[0] == '/' && p[1]) name = p + 1;
    if (wld_prctl( 15 /* PR_SET_NAME */, (long)name ) == -1) return;

    /* find the end of the argv array and move everything down */
    end = argv[argc - 1];
    while (*end) end++;
    off = argv[1] - argv[0];
    for (p = argv[1]; p <= end; p++) *(p - off) = *p;
    wld_memset( end - off, 0, off );
    for (i = 1; i < argc; i++) argv[i] -= off;
}


/*
 *  wld_start
 *
 *  Repeat the actions the kernel would do when loading a dynamically linked .so
 *  Load the binary and then its ELF interpreter.
 *  Note, we assume that the binary is a dynamically linked ELF shared object.
 */
void* wld_start( void **stack )
{
    long i, *pargc;
    char **argv, **p;
    char *interp, *reserve = NULL;
    struct wld_auxv new_av[12], delete_av[3], *av;
    struct wld_link_map main_binary_map, ld_so_map;
    struct wine_preload_info **wine_main_preload_info;

    pargc = *stack;
    argv = (char **)pargc + 1;
    if (*pargc < 2) fatal_error( "Usage: %s wine_binary [args]\n", argv[0] );

    /* skip over the parameters */
    p = argv + *pargc + 1;

    /* skip over the environment */
    while (*p)
    {
        static const char res[] = "WINEPRELOADRESERVE=";
        if (!wld_strncmp( *p, res, sizeof(res)-1 )) reserve = *p + sizeof(res) - 1;
        p++;
    }

    av = (struct wld_auxv *)(p+1);
    page_size = get_auxiliary( av, AT_PAGESZ, 4096 );
    page_mask = page_size - 1;

    preloader_start = (char *)_start - ((unsigned long)_start & page_mask);
    preloader_end = (char *)((unsigned long)(_end + page_mask) & ~page_mask);

#ifdef DUMP_AUX_INFO
    wld_printf( "stack = %p\n", *stack );
    for( i = 0; i < *pargc; i++ ) wld_printf("argv[%lx] = %s\n", i, argv[i]);
    dump_auxiliary( av );
#endif

    /* reserve memory that Wine needs */
    if (reserve) preload_reserve( reserve );
    for (i = 0; preload_info[i].size; i++)
    {
        if ((char *)av >= (char *)preload_info[i].addr &&
            (char *)pargc <= (char *)preload_info[i].addr + preload_info[i].size)
        {
            remove_preload_range( i );
            i--;
        }
        else if (wld_mmap( preload_info[i].addr, preload_info[i].size, PROT_NONE,
                           MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0 ) == (void *)-1)
        {
            /* don't warn for low 64k */
            if (preload_info[i].addr >= (void *)0x10000)
                wld_printf( "preloader: Warning: failed to reserve range %p-%p\n",
                            preload_info[i].addr, (char *)preload_info[i].addr + preload_info[i].size );
            remove_preload_range( i );
            i--;
        }
    }

    /* add an executable page at the top of the address space to defeat
     * broken no-exec protections that play with the code selector limit */
    if (is_addr_reserved( (char *)0x80000000 - page_size ))
        wld_mprotect( (char *)0x80000000 - page_size, page_size, PROT_EXEC | PROT_READ );

    /* load the main binary */
    map_so_lib( argv[1], &main_binary_map );

    /* load the ELF interpreter */
    interp = (char *)main_binary_map.l_addr + main_binary_map.l_interp;
    map_so_lib( interp, &ld_so_map );

    /* store pointer to the preload info into the appropriate main binary variable */
    wine_main_preload_info = find_symbol( &main_binary_map, "wine_main_preload_info", STT_OBJECT );
    if (wine_main_preload_info) *wine_main_preload_info = preload_info;
    else wld_printf( "wine_main_preload_info not found\n" );

#define SET_NEW_AV(n,type,val) new_av[n].a_type = (type); new_av[n].a_un.a_val = (val);
    SET_NEW_AV( 0, AT_PHDR, (unsigned long)main_binary_map.l_phdr );
    SET_NEW_AV( 1, AT_PHENT, sizeof(ElfW(Phdr)) );
    SET_NEW_AV( 2, AT_PHNUM, main_binary_map.l_phnum );
    SET_NEW_AV( 3, AT_PAGESZ, page_size );
    SET_NEW_AV( 4, AT_BASE, ld_so_map.l_addr );
    SET_NEW_AV( 5, AT_FLAGS, get_auxiliary( av, AT_FLAGS, 0 ) );
    SET_NEW_AV( 6, AT_ENTRY, main_binary_map.l_entry );
    SET_NEW_AV( 7, AT_UID, get_auxiliary( av, AT_UID, wld_getuid() ) );
    SET_NEW_AV( 8, AT_EUID, get_auxiliary( av, AT_EUID, wld_geteuid() ) );
    SET_NEW_AV( 9, AT_GID, get_auxiliary( av, AT_GID, wld_getgid() ) );
    SET_NEW_AV(10, AT_EGID, get_auxiliary( av, AT_EGID, wld_getegid() ) );
    SET_NEW_AV(11, AT_NULL, 0 );
#undef SET_NEW_AV

    i = 0;
    /* delete sysinfo values if addresses conflict */
    if (is_in_preload_range( av, AT_SYSINFO ) || is_in_preload_range( av, AT_SYSINFO_EHDR ))
    {
        delete_av[i++].a_type = AT_SYSINFO;
        delete_av[i++].a_type = AT_SYSINFO_EHDR;
    }
    delete_av[i].a_type = AT_NULL;

    /* get rid of first argument */
    set_process_name( *pargc, argv );
    pargc[1] = pargc[0] - 1;
    *stack = pargc + 1;

    set_auxiliary_values( av, new_av, delete_av, stack );

#ifdef DUMP_AUX_INFO
    wld_printf("new stack = %p\n", *stack);
    wld_printf("jumping to %p\n", (void *)ld_so_map.l_entry);
#endif
#ifdef DUMP_MAPS
    {
        char buffer[1024];
        int len, fd = wld_open( "/proc/self/maps", O_RDONLY );
        if (fd != -1)
        {
            while ((len = wld_read( fd, buffer, sizeof(buffer) )) > 0) wld_write( 2, buffer, len );
            wld_close( fd );
        }
    }
#endif

    return (void *)ld_so_map.l_entry;
}

#endif /* __APPLE__ */
