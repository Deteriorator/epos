/**
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * Copyright (C) 2013 Hong MingJian<hongmingjian@gmail.com>
 * All rights reserved.
 *
 * This file is part of the EPOS.
 *
 * Redistribution and use in source and binary forms are freely
 * permitted provided that the above copyright notice and this
 * paragraph and the following disclaimer are duplicated in all
 * such forms.
 *
 * This software is provided "AS IS" and without any express or
 * implied warranties, including, without limitation, the implied
 * warranties of merchantability and fitness for a particular
 * purpose.
 *
 */
#include "kernel.h"
#include "dosfs.h"

struct vmzone {
    uint32_t base;
    uint32_t limit;
    struct vmzone *next;
};

static struct vmzone km0;
static struct vmzone *kvmzone;

static struct vmzone um0;
static struct vmzone *uvmzone;

void init_page()
{
    km0.base = PAGE_ROUNDUP( (uint32_t)(&end) );
    km0.limit = KERN_MAX_ADDR-km0.base;
    km0.next = NULL;
    kvmzone = &km0;

    um0.base = USER_MIN_ADDR;
    um0.limit = USER_MAX_ADDR-um0.base;
    um0.next = NULL;
    uvmzone = &um0;
}

uint32_t page_alloc_in_addr(uint32_t va, int npages)
{
    uint32_t size = npages * PAGE_SIZE;
    if(npages <= 0)
        return SIZE_MAX;

    struct vmzone *p = kvmzone;
    if(va < USER_MAX_ADDR)
        p = uvmzone;

    for(; p != NULL; p = p->next) {
        if(va >= p->base &&
           va <  p->base + p->limit &&
           va + size >  p->base &&
           va + size <= p->base+p->limit) {
            break;
       }
   }

   if(p != NULL) {
       if(va == p->base) {
           p->base += size;
           p->limit -= size;
           return va;
       }

       if(va + size == p->base + p->limit) {
           p->limit -= size;
           return va;
       }

       struct vmzone *x = (struct vmzone *)kmalloc(sizeof(struct vmzone));
       x->base = va + size;
       x->limit = p->limit-(x->base-p->base);
       x->next = p->next;

       p->limit = (va - p->base);
       p->next = x;

       return va;
   }

   return SIZE_MAX;
}

uint32_t page_alloc(int npages, uint32_t user)
{
    uint32_t va = SIZE_MAX;
    uint32_t size = npages * PAGE_SIZE;
    if(npages <= 0)
        return va;

    struct vmzone *q = NULL, *p = kvmzone;
    if(user)
       p = uvmzone;

    while(p != NULL) {
        if(p->limit >= size) {
            va = p->base;
            p->base += size;
            p->limit -= size;
            if(p->limit == 0) {
                if(p != &km0 && p != &um0) {
                    if(q == NULL) {
                        if(user)
                            uvmzone = p->next;
                        else
                            kvmzone = p->next;
                    } else
                        q->next = p->next;
                    kfree(p);
                }
            }
            return va;
        }
        q = p;
        p = p->next;
    }

    return va;
}

void page_free(uint32_t va, int npages)
{
    uint32_t size = npages * PAGE_SIZE;
	if(npages <= 0)
		return;

    struct vmzone *q = NULL, *p = kvmzone;
    if(va < USER_MAX_ADDR)
        p = uvmzone;

    for(; p != NULL; q = p, p = p->next) {
        if(va >  p->base + p->limit)
            continue;
        if(va == p->base + p->limit) {
            p->limit += size;
            return;
        }

        if(va + size == p->base) {
            p->base = va;
            p->limit += size;
            return;
        }
        if(va + size < p->base)
            break;
    }
    struct vmzone *x = (struct vmzone *)kmalloc(sizeof(struct vmzone));
    x->base = va;
    x->limit = size;
    x->next = p;

    if(p != NULL) {
        if(q == NULL) {
            if(va < USER_MAX_ADDR)
                uvmzone = x;
            else
                kvmzone = x;
        } else
            q->next = x;
    } else {
        q->next = x;
    }
}

int page_check(uint32_t va)
{
    struct vmzone *p = kvmzone;
    if(va < USER_MAX_ADDR)
        p = uvmzone;

    for(; p != NULL; p = p->next) {
        if(va >= p->base &&
           va <  p->base + p->limit)
            return 0;
    }

    return 1;
}

void page_map(uint32_t vaddr, uint32_t paddr, uint32_t npages, uint32_t flags)
{
    for (; npages > 0; npages--){
        *vtopte(vaddr) = paddr | flags;
        vaddr += PAGE_SIZE;
        paddr += PAGE_SIZE;
    }
}

void page_unmap(uint32_t vaddr, uint32_t npages)
{
    for (; npages > 0; npages--){
        *vtopte(vaddr) = 0;
        vaddr += PAGE_SIZE;
    }
}

int do_page_fault(struct context *ctx, uint32_t vaddr, uint32_t code)
{
    uint32_t flags;

#if VERBOSE
    printk("PF:0x%08x(0x%01x)", vaddr, code);
#endif

    if((code&PTE_V) == 0) {
        uint32_t paddr;

        if(!page_check(vaddr)) {
            printk("PF: Invalid memory access: 0x%08x(0x%01x)\r\n", vaddr, code);
            return -1;
        }

        if (vaddr < USER_MAX_ADDR) {
            code |= PTE_U;
        }
        if((vaddr >= (uint32_t)vtopte(USER_MIN_ADDR)) &&
-          (vaddr <  (uint32_t)vtopte(USER_MAX_ADDR))) {
            code |= PTE_U;
        }

        /* Search for a free frame */
        save_flags_cli(flags);
        paddr = frame_alloc(1);
        restore_flags(flags);

        if(paddr != SIZE_MAX) {
            /* Got one :), clear its data before returning */
            *vtopte(vaddr)=paddr|PTE_V|PTE_W|(code&PTE_U);
            memset(PAGE_TRUNCATE(vaddr), 0, PAGE_SIZE);
            invlpg(vaddr);

#if VERBOSE
            printk("->0x%08x\r\n", *vtopte(vaddr));
#endif

            return 0;
        }
    }

#if VERBOSE
    printk("->????????\r\n");
#else
    printk("PF:0x%08x(0x%01x)->????????\r\n", vaddr, code);
#endif
    return -1;
}

