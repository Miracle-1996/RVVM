/*
riscv_mmu.c - RISC-V Memory Mapping Unit
Copyright (C) 2021  LekKit <github.com/LekKit>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "riscv_mmu.h"
#include "bit_ops.h"
#include "atomics.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define SV32_VPN_BITS     10
#define SV32_VPN_MASK     0x3FF
#define SV32_PHYS_BITS    34
#define SV32_LEVELS       2

#define SV64_VPN_BITS     9
#define SV64_VPN_MASK     0x1FF
#define SV64_PHYS_BITS    56
#define SV64_PHYS_MASK    bit_mask(SV64_PHYS_BITS)
#define SV39_LEVELS       3
#define SV48_LEVELS       4
#define SV57_LEVELS       5

// Should be moved to riscv_csr.h
#define CSR_STATUS_MPRV_BIT  17
#define CSR_STATUS_MPRV_MASK (1 << CSR_STATUS_MPRV_BIT)
#define CSR_STATUS_MXR_BIT   19
#define CSR_STATUS_MXR_MASK  (1 << CSR_STATUS_MXR_BIT)

#define CSR_STATUS_MPP(x) bit_cut(x, 11, 2)

#define CSR_SATP_MODE_PHYS   0
#define CSR_SATP_MODE_SV32   1
#define CSR_SATP_MODE_SV39   8
#define CSR_SATP_MODE_SV48   9
#define CSR_SATP_MODE_SV57   10


bool riscv_init_ram(rvvm_ram_t* mem, paddr_t begin, paddr_t size)
{
    // Memory boundaries should be always aligned to page size
    if ((begin & PAGE_MASK) || (size & PAGE_MASK)) {
        rvvm_error("Memory boundaries misaligned: 0x%08"PRIxXLEN" - 0x%08"PRIxXLEN, begin, begin+size);
        return false;
    }
    vmptr_t data = calloc(size, 1);
    if (!data) {
        rvvm_error("Memory allocation failure");
        return false;
    }
    mem->data = data;
    mem->begin = begin;
    mem->size = size;
    return true;
}

void riscv_free_ram(rvvm_ram_t* mem)
{
    free(mem->data);
    // Prevent accidental access
    mem->data = NULL;
    mem->begin = 0;
    mem->size = 0;
}

void riscv_tlb_flush(rvvm_hart_t* vm)
{
    // Any lookup to nonzero page fails as VPN is zero
    memset(vm->tlb, 0, sizeof(vm->tlb));
    // For zero page, place nonzero VPN
    vm->tlb[0].r = -1;
    vm->tlb[0].w = -1;
    vm->tlb[0].e = -1;
}

void riscv_tlb_flush_page(rvvm_hart_t* vm, vaddr_t addr)
{
    vaddr_t vpn = (addr >> PAGE_SHIFT);
    // VPN is off by 1, thus invalidating the entry
    vm->tlb[vpn & TLB_MASK].r = vpn - 1;
    vm->tlb[vpn & TLB_MASK].w = vpn - 1;
    vm->tlb[vpn & TLB_MASK].e = vpn - 1;
}

static void riscv_tlb_put(rvvm_hart_t* vm, vaddr_t vaddr, paddr_t paddr, uint8_t op)
{
    vaddr_t vpn = vaddr >> PAGE_SHIFT;
    vaddr &= PAGE_PNMASK;
    paddr &= PAGE_PNMASK;
    vmptr_t ptr = riscv_phys_translate(vm, paddr);
    
    if (ptr) {
        rvvm_tlb_entry_t* entry = &vm->tlb[vpn & TLB_MASK];
        /*
        * Add only requested access bits for correct access/dirty flags
        * implementation. Assume the software does not clear A/D bits without
        * calling SFENCE.VMA
        */
        switch (op) {
            case MMU_READ:
                entry->r = vpn;
                // If same tlb entry contains different VPNs,
                // they should be invalidated
                if (entry->w != vpn) entry->w = vpn - 1;
                if (entry->e != vpn) entry->e = vpn - 1;
                break;
            case MMU_WRITE:
                entry->r = vpn;
                entry->w = vpn;
                if (entry->e != vpn) entry->e = vpn - 1;
                break;
            case MMU_EXEC:
                if (entry->r != vpn) entry->r = vpn - 1;
                if (entry->w != vpn) entry->w = vpn - 1;
                entry->e = vpn;
                break;
            default:
                // (???) lets just complain and flush the entry
                rvvm_error("Unknown MMU op in riscv_tlb_put");
                entry->r = vpn - 1;
                entry->w = vpn - 1;
                entry->e = vpn - 1;
                break;
        }

        entry->ptr = ptr - TLB_VADDR(vaddr);
    }
}

// Virtual memory addressing mode (SV32)
static bool riscv_mmu_translate_sv32(rvvm_hart_t* vm, vaddr_t vaddr, paddr_t* paddr, uint8_t access)
{
    // Pagetable is always aligned to PAGE_SIZE
    paddr_t pagetable = vm->root_page_table;
    paddr_t pte, pgt_off;
    vmptr_t pte_addr;
    bitcnt_t bit_off = SV32_VPN_BITS + PAGE_SHIFT;

    for (size_t i=0; i<SV32_LEVELS; ++i) {
        pgt_off = ((vaddr >> bit_off) & SV32_VPN_MASK) << 2;
        pte_addr = riscv_phys_translate(vm, pagetable + pgt_off);
        if (pte_addr) {
            pte = read_uint32_le(pte_addr);
            if (pte & MMU_VALID_PTE) {
                if (pte & MMU_LEAF_PTE) {
                    // PGT entry is a leaf, check access bits & translate
                    if (pte & access) {
                        vaddr_t vmask = bit_mask(bit_off);
                        paddr_t pmask = bit_mask(SV32_PHYS_BITS - bit_off) << bit_off;
                        paddr_t pte_flags = pte | MMU_PAGE_ACCESSED | ((access & MMU_WRITE) << 5);
                        paddr_t pte_shift = pte << 2;
                        // Check that PPN[i-1:0] is 0, otherwise the page is misaligned
                        if (unlikely(pte_shift & (vmask & pmask)))
                            return false;
                        // Atomically update A/D flags
                        if (pte != pte_flags) atomic_cas_uint32_le(pte_addr, pte, pte_flags);
                        // Combine ppn & vpn & pgoff
                        *paddr = (pte_shift & pmask) | (vaddr & vmask);
                        return true;
                    }
                } else if ((pte & MMU_WRITE) == 0) {
                    // PGT entry is a pointer to next pagetable
                    pagetable = (pte >> 10) << PAGE_SHIFT;
                    bit_off -= SV32_VPN_BITS;
                    continue;
                }
            }
        }
        // No valid address translation can be done (invalid PTE or protection fault)
        return false;
    }
    return false;
}

#ifdef USE_RV64

// Virtual memory addressing mode (RV64 MMU template)
static bool riscv_mmu_translate_rv64(rvvm_hart_t* vm, vaddr_t vaddr, paddr_t* paddr, uint8_t access, uint8_t sv_levels)
{
    // Pagetable is always aligned to PAGE_SIZE
    paddr_t pagetable = vm->root_page_table;
    paddr_t pte, pgt_off;
    vmptr_t pte_addr;
    bitcnt_t bit_off = (sv_levels * SV64_VPN_BITS) + PAGE_SIZE - SV64_VPN_BITS;
    
    if (unlikely(vaddr != (vaddr_t)sign_extend(vaddr, bit_off+SV64_VPN_BITS)))
        return false;

    for (size_t i=0; i<sv_levels; ++i) {
        pgt_off = ((vaddr >> bit_off) & SV64_VPN_MASK) << 3;
        pte_addr = riscv_phys_translate(vm, pagetable + pgt_off);
        if (pte_addr) {
            pte = read_uint64_le(pte_addr);
            if (pte & MMU_VALID_PTE) {
                if (pte & MMU_LEAF_PTE) {
                    // PGT entry is a leaf, check access bits & translate
                    if (pte & access) {
                        vaddr_t vmask = bit_mask(bit_off);
                        paddr_t pmask = bit_mask(SV64_PHYS_BITS - bit_off) << bit_off;
                        paddr_t pte_flags = pte | MMU_PAGE_ACCESSED | ((access & MMU_WRITE) << 5);
                        paddr_t pte_shift = pte << 2;
                        // Check that PPN[i-1:0] is 0, otherwise the page is misaligned
                        if (unlikely(pte_shift & (vmask & pmask)))
                            return false;
                        // Atomically update A/D flags
                        atomic_cas_uint64_le(pte_addr, pte, pte_flags);
                        // Combine ppn & vpn & pgoff
                        *paddr = (pte_shift & pmask) | (vaddr & vmask);
                        return true;
                    }
                } else if ((pte & MMU_WRITE) == 0) {
                    // PGT entry is a pointer to next pagetable
                    pagetable = ((pte >> 10) << PAGE_SHIFT) & SV64_PHYS_MASK;
                    bit_off -= SV64_VPN_BITS;
                    continue;
                }
            }
        }
        // No valid address translation can be done (invalid PTE or protection fault)
        return false;
    }
    return false;
}

#endif

static inline bool riscv_mmu_translate(rvvm_hart_t* vm, vaddr_t vaddr, paddr_t* paddr, uint8_t access)
{
    uint8_t priv = vm->priv_mode;
    // If MPRV is enabled, and we aren't fetching an instruction,
    // change effective privilege mode to STATUS.MPP
    if ((vm->csr.status & CSR_STATUS_MPRV_MASK) && (access != MMU_EXEC)) {
        priv = CSR_STATUS_MPP(vm->csr.status);
    }
    // If MXR is enabled, reads from pages marked as executable-only should succeed
    if ((vm->csr.status & CSR_STATUS_MXR_MASK) && (access == MMU_READ)) {
        access = MMU_EXEC;
    }
    if (priv <= PRIVILEGE_SUPERVISOR) {
        switch (vm->mmu_mode) {
            case CSR_SATP_MODE_PHYS:
                *paddr = vaddr;
                return true;
            case CSR_SATP_MODE_SV32:
                return riscv_mmu_translate_sv32(vm, vaddr, paddr, access);
#ifdef USE_RV64
            case CSR_SATP_MODE_SV39:
                return riscv_mmu_translate_rv64(vm, vaddr, paddr, access, SV39_LEVELS);
            case CSR_SATP_MODE_SV48:
                return riscv_mmu_translate_rv64(vm, vaddr, paddr, access, SV48_LEVELS);
            case CSR_SATP_MODE_SV57:
                return riscv_mmu_translate_rv64(vm, vaddr, paddr, access, SV57_LEVELS);
#endif
            default:
                // satp is a WARL field
                rvvm_error("Unknown MMU mode in riscv_mmu_translate");
                return false;
        }
    } else {
        *paddr = vaddr;
        return true;
    }
}

void riscv_mmio_read_unaligned(const rvvm_mmio_dev_t* mmio, void* dest, uint8_t size, paddr_t offset)
{
    assert(mmio->max_op_size >= mmio->min_op_size);
    if (size < mmio->min_op_size || (offset & (mmio->min_op_size - 1))) {
        // Operation size smaller than possible or address misaligned
        // Read bigger chunk, then use only part of it
        paddr_t aligned_offset = offset & ~(paddr_t)(mmio->min_op_size - 1);
        uint8_t offset_diff = offset - aligned_offset;
        uint8_t new_size = mmio->min_op_size;
        uint8_t tmp[16];
        while (new_size < size + offset_diff) new_size <<= 1;
        riscv_mmio_read_unaligned(mmio, tmp, mmio->min_op_size, aligned_offset);
        memcpy(dest, tmp + offset_diff, size);
        return;
    }
    if (size > mmio->max_op_size) {
        // Max operation size exceeded, cut into smaller parts
        uint8_t size_half = size >> 1;
        riscv_mmio_read_unaligned(mmio, dest, size_half, offset);
        riscv_mmio_read_unaligned(mmio, ((vmptr_t)dest) + size_half, size_half, offset + size_half);
        return;
    }
    mmio->read(mmio->data, dest, size, offset);
}

static inline void riscv_mmio_read(const rvvm_mmio_dev_t* mmio, void* dest, uint8_t size, paddr_t offset)
{
    if (unlikely(size > mmio->max_op_size || size < mmio->min_op_size || (offset & (mmio->min_op_size-1)))) {
        riscv_mmio_read_unaligned(mmio, dest, size, offset);
        return;
    }
    mmio->read(mmio->data, dest, size, offset);
}

// Receives any operation on physical address space out of RAM region
static bool riscv_mmio_op(rvvm_hart_t* vm, paddr_t addr, void* dest, size_t size, uint8_t access)
{
    /*rvvm_mmio_dev_t* dev;
    
    vector_foreach(vm->machine->mmio, i) {
        dev = &vector_at(vm->machine->mmio, i);
        if (addr >= dev->begin && addr <= dev->end) {
            if (access == MMU_WRITE) {
                if (dev->write) dev->write(dev, dest, addr - dev->begin);
            } else {
                
            }
        }
    }*/
    
    return false;
}

// Stub
static inline void riscv_jit_flush(rvvm_hart_t* vm, vaddr_t vaddr, paddr_t paddr, size_t size)
{
    UNUSED(vm);
    UNUSED(vaddr);
    UNUSED(paddr);
    UNUSED(size);
}

static bool riscv_mmu_op(rvvm_hart_t* vm, vaddr_t addr, void* dest, size_t size, uint8_t access)
{
    paddr_t paddr;
    vmptr_t ptr;
    uint32_t trap_cause;

    // Handle misalign between pages
    if (!riscv_block_in_page(addr, size)) {
        // Prevent recursive faults by checking return flag
        size_t part_size = PAGE_SIZE - (addr & PAGE_MASK);
        return riscv_mmu_op(vm, addr, dest, part_size, access) &&
               riscv_mmu_op(vm, addr + part_size, ((vmptr_t)dest) + part_size, size - part_size, access);
    }

    if (riscv_mmu_translate(vm, addr, &paddr, access)) {
        ptr = riscv_phys_translate(vm, paddr);
        if (ptr) {
            // Physical address in main memory, cache address translation
            riscv_tlb_put(vm, addr, paddr, access);
            if (access == MMU_WRITE) {
                // Clear JITted blocks & flush trace cache if necessary
                riscv_jit_flush(vm, addr, paddr, size);
                memcpy(ptr, dest, size);
            } else {
                memcpy(dest, ptr, size);
            }
            return true;
        }
        // Physical address not in memory region, check MMIO
        if (riscv_mmio_op(vm, paddr, dest, size, access)) {
            return true;
        }
        // Physical memory access fault (bad physical address)
        switch (access) {
            case MMU_WRITE:
                trap_cause = TRAP_STORE_FAULT;
                break;
            case MMU_READ:
                trap_cause = TRAP_LOAD_FAULT;
                break;
            case MMU_EXEC:
                trap_cause = TRAP_INSTR_FETCH;
                break;
            default:
                rvvm_error("Unknown MMU op in riscv_mmu_op (phys)");
                trap_cause = 0;
                break;
        }
    } else {
        // Pagefault (no translation for address or protection fault)
        switch (access) {
            case MMU_WRITE:
                trap_cause = TRAP_STORE_PAGEFAULT;
                break;
            case MMU_READ:
                trap_cause = TRAP_LOAD_PAGEFAULT;
                break;
            case MMU_EXEC:
                trap_cause = TRAP_INSTR_PAGEFAULT;
                break;
            default:
                rvvm_error("Unknown MMU op in riscv_mmu_op (page)");
                trap_cause = 0;
                break;
        }
    }
    // Trap the CPU & instruct the caller to discard operation
    riscv_trap(vm, trap_cause, addr);
    return false;
}

