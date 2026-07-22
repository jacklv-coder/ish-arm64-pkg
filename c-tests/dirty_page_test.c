// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asbestos/asbestos.h"
#ifdef GUEST_ARM64
#include "asbestos/frame.h"
#include "asbestos/gen.h"
#include "asbestos/guest-arm64/sysregs.h"
#endif
#include "emu/cpu.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"
#include "kernel/memory.h"
#include "util/list.h"

struct fake_mmu {
    struct mmu mmu;
    addr_t base;
    size_t mapped_size;
    unsigned out_of_range_translations;
    unsigned char bytes[4 * PAGE_SIZE];
};

static void *fake_translate(struct mmu *mmu, addr_t addr, int type) {
    (void) type;
    struct fake_mmu *fake = (struct fake_mmu *) mmu;
    size_t mapped_size = fake->mapped_size != 0 ? fake->mapped_size : sizeof(fake->bytes);
    if (addr < fake->base || addr >= fake->base + mapped_size) {
        fake->out_of_range_translations++;
        return NULL;
    }
    return fake->bytes + (addr - fake->base);
}

static void *fake_translate_write_nofault(struct mmu *mmu, addr_t addr) {
    return fake_translate(mmu, addr, MEM_WRITE);
}

static unsigned dirty_bucket(addr_t addr) {
    return (unsigned) (PAGE(TLB_PAGE(addr)) & (TLB_DIRTY_BUCKET_COUNT - 1));
}

static bool bitmap_bucket_is_set(const struct tlb *tlb, addr_t addr) {
    unsigned bucket = dirty_bucket(addr);
    return (tlb->dirty_page_buckets[bucket / 64] & (UINT64_C(1) << (bucket % 64))) != 0;
}

static bool pending_bucket_is_set(const struct tlb *tlb, addr_t addr) {
    return bitmap_bucket_is_set(tlb, addr) ||
            (tlb_has_runtime_dirty_pages(tlb) &&
             dirty_bucket(tlb->dirty_page) == dirty_bucket(addr));
}

static void assert_dirty_set_empty(const struct tlb *tlb) {
    assert(!tlb_has_runtime_dirty_pages(tlb));
    for (unsigned word = 0; word < TLB_DIRTY_BUCKET_WORDS; word++)
        assert(tlb->dirty_page_buckets[word] == 0);
}

static unsigned invalidate_generation(const struct asbestos *asbestos) {
    return atomic_load_explicit(&asbestos->invalidate_gen, memory_order_acquire);
}

static struct fiber_block *add_block(struct asbestos *asbestos, page_t page);

static void test_c_access_marking(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT64_C(0x100000),
    };
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(tlb != NULL);
    tlb->mmu = &fake.mmu;
    tlb_clear_runtime_dirty_pages(tlb);

    uint32_t value = 0;
    assert(tlb_read(tlb, fake.base + 8, &value, sizeof(value)));
    assert_dirty_set_empty(tlb);

    value = UINT32_C(0x12345678);
    assert(tlb_write(tlb, fake.base + 8, &value, sizeof(value)));
    assert(tlb->dirty_page == TLB_PAGE(fake.base));
    assert(pending_bucket_is_set(tlb, fake.base));
    assert(!bitmap_bucket_is_set(tlb, fake.base));
    tlb_clear_runtime_dirty_pages(tlb);

    // A write miss must mark only after translation succeeds.
    assert(tlb_write(tlb, fake.base + 2 * PAGE_SIZE + 16, &value, sizeof(value)));
    assert(tlb->dirty_page == TLB_PAGE(fake.base + 2 * PAGE_SIZE));
    assert(pending_bucket_is_set(tlb, fake.base + 2 * PAGE_SIZE));
    assert(!bitmap_bucket_is_set(tlb, fake.base + 2 * PAGE_SIZE));
    tlb_clear_runtime_dirty_pages(tlb);
    assert(!tlb_write(tlb, fake.base + sizeof(fake.bytes), &value, sizeof(value)));
    assert_dirty_set_empty(tlb);

    // Prime two adjacent writable entries through READ misses. Reads must not
    // mark either bucket, while the following cross-page write must retain both.
    unsigned char before[2] = {0};
    assert(tlb_read(tlb, fake.base + PAGE_SIZE - 2, before, sizeof(before)));
    assert(tlb_read(tlb, fake.base + PAGE_SIZE, before, sizeof(before)));
    assert_dirty_set_empty(tlb);
    const unsigned char cross_page[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    assert(tlb_write(tlb, fake.base + PAGE_SIZE - 2, cross_page, sizeof(cross_page)));
    assert(bitmap_bucket_is_set(tlb, fake.base));
    assert(!bitmap_bucket_is_set(tlb, fake.base + PAGE_SIZE));
    assert(pending_bucket_is_set(tlb, fake.base));
    assert(pending_bucket_is_set(tlb, fake.base + PAGE_SIZE));
    assert(tlb->dirty_page == TLB_PAGE(fake.base + PAGE_SIZE));
    assert(memcmp(fake.bytes + PAGE_SIZE - 2, cross_page, sizeof(cross_page)) == 0);

    // Revisit a prior page after two transitions. Every page left behind must
    // remain in the bitmap, while the revisited page is also the final exact
    // marker that the dispatcher will merge before draining.
    tlb_clear_runtime_dirty_pages(tlb);
    tlb_mark_dirty_page(tlb, fake.base);
    tlb_mark_dirty_page(tlb, fake.base + PAGE_SIZE);
    tlb_mark_dirty_page(tlb, fake.base + 2 * PAGE_SIZE);
    tlb_mark_dirty_page(tlb, fake.base);
    assert(tlb->dirty_page == TLB_PAGE(fake.base));
    assert(bitmap_bucket_is_set(tlb, fake.base));
    assert(bitmap_bucket_is_set(tlb, fake.base + PAGE_SIZE));
    assert(bitmap_bucket_is_set(tlb, fake.base + 2 * PAGE_SIZE));
    assert(pending_bucket_is_set(tlb, fake.base));
    assert(pending_bucket_is_set(tlb, fake.base + PAGE_SIZE));
    assert(pending_bucket_is_set(tlb, fake.base + 2 * PAGE_SIZE));

    tlb_free(tlb);
}

#ifdef GUEST_ARM64
extern void gadget_exit(void);
extern void gadget_ic_ivau(void);
extern int fiber_enter(struct fiber_block *block, struct fiber_frame *frame, struct tlb *tlb);

static void test_ic_ivau_dispatch_boundary(void) {
    assert(ARM64_CTR_EL0_VALUE == UINT32_C(0x84448004));
    assert((ARM64_CTR_EL0_VALUE & (ARM64_CTR_EL0_DIC | ARM64_CTR_EL0_IDC)) == 0);

    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT64_C(0x200000),
    };
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *writer_tlb = calloc(1, sizeof(*writer_tlb));
    struct tlb *executor_tlb = calloc(1, sizeof(*executor_tlb));
    assert(asbestos != NULL && writer_tlb != NULL && executor_tlb != NULL);
    fake.mmu.asbestos = asbestos;
    writer_tlb->mmu = &fake.mmu;
    executor_tlb->mmu = &fake.mmu;
    tlb_clear_runtime_dirty_pages(writer_tlb);
    tlb_clear_runtime_dirty_pages(executor_tlb);

    const uint32_t ic_ivau_x0 = UINT32_C(0xd50b7520);
    memcpy(fake.bytes, &ic_ivau_x0, sizeof(ic_ivau_x0));
    struct gen_state state;
    gen_start(fake.base, &state);
    assert(gen_step(&state, executor_tlb) == 0);
    assert(state.ip == fake.base + sizeof(ic_ivau_x0));
    assert(state.size >= 4);
    assert(state.block->code[state.size - 4] == (unsigned long) gadget_ic_ivau);
    assert(state.block->code[state.size - 3] == 0);
    assert(state.block->code[state.size - 2] == (unsigned long) gadget_exit);
    assert(state.block->code[state.size - 1] == state.ip);
    assert_dirty_set_empty(executor_tlb);
    gen_end(&state);

    // Model the architectural cross-thread sequence: the writer and the
    // thread executing IC IVAU have different TLBs. IC must publish Xt into
    // the executor's set so its dispatcher can invalidate shared code.
    const addr_t target = fake.base + 2 * PAGE_SIZE;
    struct fiber_block *target_block = add_block(asbestos, PAGE(target));
    tlb_mark_dirty_page(writer_tlb, target);
    assert_dirty_set_empty(executor_tlb);
    struct fiber_frame frame = {
        .cpu = {
            .mmu = &fake.mmu,
            .x0 = target,
        },
    };
    assert(fiber_enter(state.block, &frame, executor_tlb) == INT_NONE);
    assert(pending_bucket_is_set(executor_tlb, target));
    assert(!bitmap_bucket_is_set(executor_tlb, target));
    assert(executor_tlb->dirty_page == TLB_PAGE(target));
    assert(asbestos_invalidate_dirty_pages(asbestos, executor_tlb));
    assert(target_block->is_jetsam);
    assert_dirty_set_empty(executor_tlb);
    free(state.block);

    // Data-cache maintenance stays a NOP; it must not introduce an unrelated
    // dispatch boundary or dirty marker.
    const uint32_t dc_cvau_x0 = UINT32_C(0xd50b7b20);
    memcpy(fake.bytes + PAGE_SIZE, &dc_cvau_x0, sizeof(dc_cvau_x0));
    gen_start(fake.base + PAGE_SIZE, &state);
    assert(gen_step(&state, executor_tlb) == 1);
    assert(state.ip == fake.base + PAGE_SIZE + sizeof(dc_cvau_x0));
    assert_dirty_set_empty(executor_tlb);
    gen_end(&state);
    free(state.block);

    tlb_free(writer_tlb);
    tlb_free(executor_tlb);
    asbestos_free(asbestos);
}

#if defined(__aarch64__)
static uint32_t branch_immediate(addr_t source, addr_t target) {
    int64_t displacement = (int64_t) target - (int64_t) source;
    assert((displacement & 3) == 0);
    int64_t words = displacement / 4;
    assert(words >= -(INT64_C(1) << 25) && words < (INT64_C(1) << 25));
    return UINT32_C(0x14000000) | ((uint32_t) words & UINT32_C(0x03ffffff));
}

static uint32_t branch_link_immediate(addr_t source, addr_t target) {
    return branch_immediate(source, target) | UINT32_C(0x80000000);
}

static void put_insns(struct fake_mmu *fake, addr_t addr,
        const uint32_t *insns, size_t count) {
    assert(addr >= fake->base);
    size_t offset = (size_t) (addr - fake->base);
    assert(offset + count * sizeof(*insns) <= sizeof(fake->bytes));
    memcpy(fake->bytes + offset, insns, count * sizeof(*insns));
}

static struct fiber_block *find_block(struct asbestos *asbestos, addr_t addr) {
    struct list *bucket = &asbestos->hash[addr % asbestos->hash_size];
    if (list_null(bucket))
        return NULL;
    struct fiber_block *block;
    list_for_each_entry(bucket, block, chain) {
        if (block->addr == addr)
            return block;
    }
    return NULL;
}

static uint64_t run_guest_block(struct fake_mmu *fake, struct tlb *tlb,
        addr_t pc, uint64_t x1, uint64_t x2, uint64_t x3) {
    struct cpu_state cpu = {
        .mmu = &fake->mmu,
        .pc = pc,
        .x1 = x1,
        .x2 = x2,
        .x3 = x3,
    };
    assert(cpu_run_to_interrupt(&cpu, tlb) == INT_BREAKPOINT);
    return cpu.x0;
}

static void test_arm64_high_tlb_alias_boundary(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    // These adjacent high guest pages deliberately collide in the xor-folded
    // direct-mapped TLB. A64's last valid instruction remains wholly in the
    // first page; a following misaligned PC must trap without prefetching and
    // evicting that page in favor of its alias.
    const page_t first_page = UINT64_C(0x1ffffff);
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = (addr_t) first_page << PAGE_BITS,
        .mapped_size = 2 * PAGE_SIZE,
    };
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;

    const addr_t pc = fake.base + PAGE_SIZE - sizeof(uint32_t);
    const addr_t next_page = fake.base + PAGE_SIZE;
    assert(TLB_INDEX(pc) == TLB_INDEX(next_page));
    const uint32_t brk = UINT32_C(0xd4200000);
    put_insns(&fake, pc, &brk, 1);
    assert(run_guest_block(&fake, tlb, pc, 0, 0, 0) == 0);

    struct cpu_state cpu = {
        .mmu = &fake.mmu,
        .pc = pc + 1,
    };
    assert(cpu_run_to_interrupt(&cpu, tlb) == INT_GPF);
    assert(tlb->entries[TLB_INDEX(pc)].page == TLB_PAGE(pc));
    assert(fake.out_of_range_translations == 0);

    tlb_free(tlb);
    asbestos_free(asbestos);
}

static void assert_direct_chain_at(struct fiber_block *source,
        unsigned index, const struct fiber_block *target) {
    assert(source != NULL && target != NULL);
    assert(index <= 1);
    assert(source->jump_ip[index] != NULL);
    assert(*source->jump_ip[index] == (unsigned long) target->code);
    assert(*source->jump_ip[index] != source->old_jump_ip[index]);
}

static void assert_direct_chain(struct fiber_block *source,
        const struct fiber_block *target) {
    assert_direct_chain_at(source, 0, target);
}

static void test_arm64_smc_direct_chain_boundary(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT64_C(0x280000),
    };
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;

    const addr_t source = fake.base;
    const addr_t writer = fake.base + 0x100;
    const addr_t target = fake.base + PAGE_SIZE;
    const uint32_t mov_x0_1 = UINT32_C(0xd2800020);
    const uint32_t mov_x0_2 = UINT32_C(0xd2800040);
    const uint32_t brk = UINT32_C(0xd4200000);
    const uint32_t source_code[] = {branch_immediate(source, target)};
    const uint32_t target_code[] = {mov_x0_1, brk};
    const uint32_t writer_code[] = {
        UINT32_C(0xb9000022), // STR W2, [X1]
        branch_immediate(writer + sizeof(uint32_t), source),
    };
    put_insns(&fake, source, source_code,
            sizeof(source_code) / sizeof(source_code[0]));
    put_insns(&fake, target, target_code,
            sizeof(target_code) / sizeof(target_code[0]));
    put_insns(&fake, writer, writer_code,
            sizeof(writer_code) / sizeof(writer_code[0]));

    // The first run builds writer->source and source->target direct links. The
    // store is intentionally idempotent so the target's cached result is 1.
    assert(run_guest_block(&fake, tlb, writer, target, mov_x0_1, 0) == 1);
    struct fiber_block *writer_block = find_block(asbestos, writer);
    struct fiber_block *source_block = find_block(asbestos, source);
    struct fiber_block *target_block = find_block(asbestos, target);
    assert_direct_chain(writer_block, source_block);
    assert_direct_chain(source_block, target_block);

    // A later store must break writer->source before the already chained stale
    // target executes. Without the ARM64 dirty boundary this still returns 1.
    unsigned before = invalidate_generation(asbestos);
    assert(run_guest_block(&fake, tlb, writer, target, mov_x0_2, 0) == 2);
    assert(invalidate_generation(asbestos) == before + 1);
    assert_dirty_set_empty(tlb);

    tlb_free(tlb);
    asbestos_free(asbestos);
}

static void test_arm64_smc_return_cache_boundary(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT64_C(0x380000),
    };
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;

    const addr_t caller = fake.base + PAGE_SIZE - sizeof(uint32_t);
    const addr_t continuation = caller + sizeof(uint32_t);
    const addr_t callee = fake.base + 0x200;
    const uint32_t mov_x0_1 = UINT32_C(0xd2800020);
    const uint32_t mov_x0_2 = UINT32_C(0xd2800040);
    const uint32_t brk = UINT32_C(0xd4200000);
    const uint32_t caller_code[] = {
        branch_link_immediate(caller, callee),
    };
    const uint32_t callee_code[] = {
        UINT32_C(0xb9000022), // STR W2, [X1]
        UINT32_C(0xd65f03c0), // RET
    };
    const uint32_t continuation_code[] = {mov_x0_1, brk};
    put_insns(&fake, caller, caller_code,
            sizeof(caller_code) / sizeof(caller_code[0]));
    put_insns(&fake, callee, callee_code,
            sizeof(callee_code) / sizeof(callee_code[0]));
    put_insns(&fake, continuation, continuation_code,
            sizeof(continuation_code) / sizeof(continuation_code[0]));

    // BL caches both the callee and its return continuation. The first run
    // writes the existing instruction and establishes both direct pointers.
    assert(run_guest_block(&fake, tlb, caller, continuation, mov_x0_1, 0) == 1);
    struct fiber_block *caller_block = find_block(asbestos, caller);
    struct fiber_block *callee_block = find_block(asbestos, callee);
    struct fiber_block *continuation_block = find_block(asbestos, continuation);
    assert_direct_chain_at(caller_block, 1, callee_block);
    assert_direct_chain_at(caller_block, 0, continuation_block);

    // The callee now changes the return continuation. RET must leave the JIT
    // before following its cached pointer, so the dispatcher can recompile it.
    unsigned before = invalidate_generation(asbestos);
    assert(run_guest_block(&fake, tlb, caller, continuation, mov_x0_2, 0) == 2);
    assert(invalidate_generation(asbestos) == before + 1);
    assert_dirty_set_empty(tlb);

    tlb_free(tlb);
    asbestos_free(asbestos);
}

static void test_arm64_two_page_smc_direct_chain(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT64_C(0x300000),
    };
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;

    const addr_t source1 = fake.base;
    const addr_t source2 = fake.base + 0x100;
    const addr_t writer = fake.base + 0x200;
    const addr_t target1 = fake.base + PAGE_SIZE;
    const addr_t target2 = fake.base + 2 * PAGE_SIZE;
    const uint32_t mov_x0_1 = UINT32_C(0xd2800020);
    const uint32_t mov_x0_2 = UINT32_C(0xd2800040);
    const uint32_t brk = UINT32_C(0xd4200000);
    const uint32_t source1_code[] = {branch_immediate(source1, target1)};
    const uint32_t source2_code[] = {branch_immediate(source2, target2)};
    const uint32_t target_code[] = {mov_x0_1, brk};
    const uint32_t writer_code[] = {
        UINT32_C(0xb9000022), // STR W2, [X1]
        UINT32_C(0xb9000062), // STR W2, [X3]
        UINT32_C(0xd50b7b21), // DC CVAU, X1
        UINT32_C(0xd50b7b23), // DC CVAU, X3
        UINT32_C(0xd5033b9f), // DSB ISH
        UINT32_C(0xd50b7521), // IC IVAU, X1: force dispatcher boundary
        UINT32_C(0xd50b7523), // IC IVAU, X3
        UINT32_C(0xd5033b9f), // DSB ISH
        UINT32_C(0xd5033fdf), // ISB
        brk,
    };
    put_insns(&fake, source1, source1_code,
            sizeof(source1_code) / sizeof(source1_code[0]));
    put_insns(&fake, source2, source2_code,
            sizeof(source2_code) / sizeof(source2_code[0]));
    put_insns(&fake, target1, target_code,
            sizeof(target_code) / sizeof(target_code[0]));
    put_insns(&fake, target2, target_code,
            sizeof(target_code) / sizeof(target_code[0]));
    put_insns(&fake, writer, writer_code,
            sizeof(writer_code) / sizeof(writer_code[0]));

    // The first run compiles source and target and patches the source's fake
    // target to the translated target. The second run therefore executes the
    // direct chain rather than returning through the dispatcher between them.
    assert(run_guest_block(&fake, tlb, source1, 0, 0, 0) == 1);
    assert(run_guest_block(&fake, tlb, source2, 0, 0, 0) == 1);
    struct fiber_block *source1_block = find_block(asbestos, source1);
    struct fiber_block *source2_block = find_block(asbestos, source2);
    struct fiber_block *target1_block = find_block(asbestos, target1);
    struct fiber_block *target2_block = find_block(asbestos, target2);
    assert_direct_chain(source1_block, target1_block);
    assert_direct_chain(source2_block, target2_block);
    assert(run_guest_block(&fake, tlb, source1, 0, 0, 0) == 1);
    assert(run_guest_block(&fake, tlb, source2, 0, 0, 0) == 1);

    unsigned before = invalidate_generation(asbestos);
    assert(run_guest_block(&fake, tlb, writer, target1, mov_x0_2, target2) == 0);
    assert(invalidate_generation(asbestos) == before + 1);
    assert_dirty_set_empty(tlb);
    assert(memcmp(fake.bytes + (target1 - fake.base), &mov_x0_2,
            sizeof(mov_x0_2)) == 0);
    assert(memcmp(fake.bytes + (target2 - fake.base), &mov_x0_2,
            sizeof(mov_x0_2)) == 0);

    // Draining both dirty buckets disconnects both direct links before either
    // stale target can execute. A single last-page slot leaves source1 chained
    // to the old MOV X0,#1 translation and fails the first assertion below.
    assert(*source1_block->jump_ip[0] == source1_block->old_jump_ip[0]);
    assert(*source2_block->jump_ip[0] == source2_block->old_jump_ip[0]);
    assert(run_guest_block(&fake, tlb, source1, 0, 0, 0) == 2);
    assert(run_guest_block(&fake, tlb, source2, 0, 0, 0) == 2);

    tlb_free(tlb);
    asbestos_free(asbestos);
}
#endif
#endif

#ifdef GUEST_X86
static void assert_exact_trace_pages(const struct tlb *tlb,
        const page_t *expected, size_t expected_count) {
    page_t cursor = 0;
    addr_t page_addr;
    size_t index = 0;
    while (tlb_dirty_trace_next(tlb, &cursor, &page_addr)) {
        assert(index < expected_count);
        assert(PAGE(page_addr) == expected[index]);
        index++;
    }
    assert(index == expected_count);
}

static void test_exact_trace_survives_runtime_drain(void) {
    struct asbestos *asbestos = asbestos_new(NULL);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    assert(tlb_dirty_trace_attach(tlb));
    tlb_clear_runtime_dirty_pages(tlb);

    const page_t first = 3;
    const page_t second = 70;
    const page_t collision = first + TLB_DIRTY_BUCKET_COUNT;
    const page_t expected[] = {first, second, collision};
    tlb_mark_dirty_page(tlb, (addr_t) first << PAGE_BITS);
    tlb_mark_dirty_page(tlb, (addr_t) second << PAGE_BITS);
    tlb_mark_dirty_page(tlb, (addr_t) collision << PAGE_BITS);

    assert(asbestos_invalidate_dirty_pages(asbestos, tlb));
    assert_dirty_set_empty(tlb);
    assert(tlb_dirty_trace_has_pages(tlb));
    assert_exact_trace_pages(tlb, expected,
            sizeof(expected) / sizeof(expected[0]));

    // Iteration does not consume the snapshot and a failed comparator can
    // retry. Only a completely successful comparison acknowledgement clears.
    assert_exact_trace_pages(tlb, expected,
            sizeof(expected) / sizeof(expected[0]));
    tlb_dirty_trace_finish(tlb, false);
    assert_exact_trace_pages(tlb, expected,
            sizeof(expected) / sizeof(expected[0]));
    tlb_dirty_trace_finish(tlb, true);
    assert(!tlb_dirty_trace_has_pages(tlb));

    tlb_free(tlb);
    asbestos_free(asbestos);
}

static void test_x86_instruction_length_boundary(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT32_C(0x400000),
        .mapped_size = PAGE_SIZE,
    };
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;

    // Fifteen operand-size prefixes fill the architectural maximum exactly;
    // the sixteenth starts on an unmapped page. The decoder must raise #UD
    // before touching that page, even though prefixes recurse through decode.h.
    const addr_t pc = fake.base + PAGE_SIZE - 15;
    memset(fake.bytes + PAGE_SIZE - 15, 0x66, 16);
    struct cpu_state cpu = {
        .mmu = &fake.mmu,
        .eip = pc,
    };
    assert(cpu_run_to_interrupt(&cpu, tlb) == INT_UNDEFINED);
    assert(fake.out_of_range_translations == 0);

    tlb_free(tlb);
    asbestos_free(asbestos);
}

static void put_x86_jump(struct fake_mmu *fake, addr_t source, addr_t target) {
    assert(source >= fake->base);
    size_t offset = (size_t) (source - fake->base);
    assert(offset + 5 <= sizeof(fake->bytes));
    int64_t displacement = (int64_t) target - ((int64_t) source + 5);
    assert(displacement >= INT32_MIN && displacement <= INT32_MAX);
    int32_t rel32 = (int32_t) displacement;
    fake->bytes[offset] = 0xe9;
    memcpy(fake->bytes + offset + 1, &rel32, sizeof(rel32));
}

static struct fiber_block *find_x86_block(struct asbestos *asbestos, addr_t addr) {
    struct list *bucket = &asbestos->hash[addr % asbestos->hash_size];
    if (list_null(bucket))
        return NULL;
    struct fiber_block *block;
    list_for_each_entry(bucket, block, chain) {
        if (block->addr == addr)
            return block;
    }
    return NULL;
}

static void assert_x86_direct_chain(struct fiber_block *source,
        const struct fiber_block *target) {
    assert(source != NULL && target != NULL);
    assert(source->jump_ip[0] != NULL);
    assert(*source->jump_ip[0] == (unsigned long) target->code);
    assert(*source->jump_ip[0] != source->old_jump_ip[0]);
}

static uint32_t run_x86_block(struct fake_mmu *fake, struct tlb *tlb,
        addr_t eip, uint32_t eax) {
    struct cpu_state cpu = {
        .mmu = &fake->mmu,
        .eip = eip,
        .eax = eax,
    };
    assert(cpu_run_to_interrupt(&cpu, tlb) == INT_BREAKPOINT);
    return cpu.eax;
}

static void test_x86_smc_direct_chain_boundary(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT32_C(0x500000),
    };
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;

    const addr_t source = fake.base;
    const addr_t writer = fake.base + 0x100;
    const addr_t target = fake.base + PAGE_SIZE;
    put_x86_jump(&fake, source, target);

    // target: MOV EAX, imm32; INT3
    size_t target_offset = (size_t) (target - fake.base);
    const uint32_t one = 1;
    fake.bytes[target_offset] = 0xb8;
    memcpy(fake.bytes + target_offset + 1, &one, sizeof(one));
    fake.bytes[target_offset + 5] = 0xcc;

    // writer: MOV [target+1], EAX; JMP source. The second run starts with
    // writer->source and source->target both directly chained.
    size_t writer_offset = (size_t) (writer - fake.base);
    const uint32_t immediate_address = (uint32_t) (target + 1);
    fake.bytes[writer_offset] = 0xa3;
    memcpy(fake.bytes + writer_offset + 1, &immediate_address,
            sizeof(immediate_address));
    put_x86_jump(&fake, writer + 5, source);

    // On the first run target has no translated block, so draining the store
    // keeps the generation stable. The dispatcher can then establish both
    // writer->source and source->target while preserving target's old value.
    assert(run_x86_block(&fake, tlb, writer, 1) == 1);
    struct fiber_block *writer_block = find_x86_block(asbestos, writer);
    struct fiber_block *source_block = find_x86_block(asbestos, source);
    struct fiber_block *target_block = find_x86_block(asbestos, target);
    assert_x86_direct_chain(writer_block, source_block);
    assert_x86_direct_chain(source_block, target_block);

    // A dirty store must break writer->source at the central chain boundary,
    // drain target's translated block, and only then execute source. Without
    // the boundary check this returns the stale immediate (1).
    assert(tlb_dirty_trace_attach(tlb));
    unsigned before = invalidate_generation(asbestos);
    assert(run_x86_block(&fake, tlb, writer, 2) == 2);
    assert(invalidate_generation(asbestos) == before + 1);
    assert_dirty_set_empty(tlb);
    const page_t expected_trace[] = {PAGE(target)};
    assert_exact_trace_pages(tlb, expected_trace,
            sizeof(expected_trace) / sizeof(expected_trace[0]));
    tlb_dirty_trace_clear(tlb);

    tlb_free(tlb);
    asbestos_free(asbestos);
}
#endif

static void register_block_page(struct asbestos *asbestos,
        struct fiber_block *block, page_t page, int index) {
    unsigned bucket = (unsigned) (page % FIBER_PAGE_HASH_SIZE);
    list_init_add(&asbestos->page_hash[bucket].blocks[index],
            &block->page[index]);
    if (asbestos->page_hash_counts[bucket]++ == 0) {
        atomic_fetch_or_explicit(&asbestos->page_hash_occupied[bucket / 64],
                UINT64_C(1) << (bucket % 64), memory_order_release);
    }
}

static struct fiber_block *add_block_range(struct asbestos *asbestos,
        addr_t start_addr, addr_t end_addr) {
    assert(PAGE(end_addr) == PAGE(start_addr) ||
            PAGE(end_addr) == PAGE(start_addr) + 1);
    struct fiber_block *block = calloc(1, sizeof(*block));
    assert(block != NULL);
    block->addr = start_addr;
    block->end_addr = end_addr;
    list_init(&block->chain);
    list_init(&block->jetsam);
    for (int i = 0; i <= 1; i++) {
        list_init(&block->page[i]);
        list_init(&block->jumps_from[i]);
        list_init(&block->jumps_from_links[i]);
    }

    list_init_add(&asbestos->hash[block->addr % asbestos->hash_size], &block->chain);
    register_block_page(asbestos, block, PAGE(start_addr), 0);
    if (PAGE(end_addr) != PAGE(start_addr))
        register_block_page(asbestos, block, PAGE(end_addr), 1);
    asbestos->num_blocks++;
    return block;
}

static struct fiber_block *add_block(struct asbestos *asbestos, page_t page) {
    addr_t addr = (addr_t) page << PAGE_BITS;
    return add_block_range(asbestos, addr, addr);
}

static struct fiber_block *find_synthetic_block(struct asbestos *asbestos,
        addr_t addr) {
    struct list *bucket = &asbestos->hash[addr % asbestos->hash_size];
    if (list_null(bucket))
        return NULL;
    struct fiber_block *block;
    list_for_each_entry(bucket, block, chain) {
        if (block->addr == addr)
            return block;
    }
    return NULL;
}

static void test_single_dirty_page_collision_is_exact(void) {
    struct asbestos *asbestos = asbestos_new(NULL);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    tlb_clear_runtime_dirty_pages(tlb);

    const page_t code_page = 1200;
    const page_t empty_collision_page = code_page + FIBER_PAGE_HASH_SIZE;
    assert(code_page % FIBER_PAGE_HASH_SIZE ==
            empty_collision_page % FIBER_PAGE_HASH_SIZE);
    struct fiber_block *code = add_block(asbestos, code_page);
    size_t blocks_before = asbestos->num_blocks;
    unsigned generation_before = invalidate_generation(asbestos);

    // Until the first transition the marker retains an exact page. A data-only
    // page that merely hashes to a code bucket must not evict that code.
    tlb_mark_dirty_page(tlb, (addr_t) empty_collision_page << PAGE_BITS);
    assert(asbestos_invalidate_dirty_pages(asbestos, tlb));
    assert(!code->is_jetsam);
    assert(find_synthetic_block(asbestos, code->addr) == code);
    assert(asbestos->num_blocks == blocks_before);
    assert(invalidate_generation(asbestos) == generation_before);
    assert_dirty_set_empty(tlb);

    // Marking the real code page removes exactly that block and advances the
    // shared generation once. Re-draining the consumed marker is a no-op.
    tlb_mark_dirty_page(tlb, (addr_t) code_page << PAGE_BITS);
    assert(asbestos_invalidate_dirty_pages(asbestos, tlb));
    assert(code->is_jetsam);
    assert(find_synthetic_block(asbestos, code->addr) == NULL);
    assert(asbestos->num_blocks + 1 == blocks_before);
    assert(invalidate_generation(asbestos) == generation_before + 1);
    assert_dirty_set_empty(tlb);
    assert(!asbestos_invalidate_dirty_pages(asbestos, tlb));
    assert(invalidate_generation(asbestos) == generation_before + 1);

    asbestos_free(asbestos);
    free(tlb);
}

static void test_cross_page_end_invalidation_filters_collision(void) {
    struct asbestos *asbestos = asbestos_new(NULL);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    tlb_clear_runtime_dirty_pages(tlb);

    const page_t first_page = 1600;
    const page_t end_page = first_page + 1;
    const page_t remote_collision_page = end_page + FIBER_PAGE_HASH_SIZE;
    assert(end_page % FIBER_PAGE_HASH_SIZE ==
            remote_collision_page % FIBER_PAGE_HASH_SIZE);
    addr_t cross_start = ((addr_t) first_page << PAGE_BITS) + PAGE_SIZE - 2;
    addr_t cross_end = ((addr_t) end_page << PAGE_BITS) + 2;
    struct fiber_block *cross =
            add_block_range(asbestos, cross_start, cross_end);
    struct fiber_block *remote = add_block(asbestos, remote_collision_page);
    size_t blocks_before = asbestos->num_blocks;
    unsigned generation_before = invalidate_generation(asbestos);

    // The exact end page must match page[1] through end_addr. The unrelated
    // block in the same hash bucket proves the invalidator does not treat the
    // bucket itself as an exact page on this single-page path.
    tlb_mark_dirty_page(tlb, (addr_t) end_page << PAGE_BITS);
    assert(asbestos_invalidate_dirty_pages(asbestos, tlb));
    assert(cross->is_jetsam);
    assert(!remote->is_jetsam);
    assert(find_synthetic_block(asbestos, cross_start) == NULL);
    assert(find_synthetic_block(asbestos, remote->addr) == remote);
    assert(asbestos->num_blocks + 1 == blocks_before);
    assert(invalidate_generation(asbestos) == generation_before + 1);
    assert_dirty_set_empty(tlb);

    asbestos_free(asbestos);
    free(tlb);
}

static void test_mem_did_write_post_invalidation(void) {
    struct mem mem = {0};
    struct asbestos *asbestos = asbestos_new(&mem.mmu);
    assert(asbestos != NULL);
    mem.mmu.asbestos = asbestos;

    const page_t code_page = 600;
    const page_t data_page = 601;
    struct fiber_block *code = add_block(asbestos, code_page);
    unsigned before = invalidate_generation(asbestos);

    // A data-only page stays on the occupancy fast path and must not disturb
    // a translated block in another bucket or advance the generation.
    mem_did_write(&mem, (addr_t) data_page << PAGE_BITS, sizeof(uint32_t));
    assert(!code->is_jetsam);
    assert(invalidate_generation(asbestos) == before);

    // Model a compiler publishing old bytes after the pre-write invalidation:
    // the post-write edge must still remove that freshly published block.
    mem_did_write(&mem, (addr_t) code_page << PAGE_BITS, sizeof(uint32_t));
    assert(code->is_jetsam);
    assert(invalidate_generation(asbestos) == before + 1);

    const page_t cross_first_page = 700;
    const page_t cross_second_page = cross_first_page + 1;
    struct fiber_block *cross_first = add_block(asbestos, cross_first_page);
    struct fiber_block *cross_second = add_block(asbestos, cross_second_page);
    mem_did_write(&mem,
            ((addr_t) cross_first_page << PAGE_BITS) + PAGE_SIZE - 2, 4);
    assert(cross_first->is_jetsam);
    assert(cross_second->is_jetsam);

    // An impossible wrapped guest range is never allowed to become a no-op;
    // it conservatively drops every remaining translated block.
    const page_t survivor_page = 800;
    struct fiber_block *survivor = add_block(asbestos, survivor_page);
    mem_did_write(&mem, (addr_t) -1, 2);
    assert(survivor->is_jetsam);

    asbestos_free(asbestos);
}

struct drain_context {
    struct asbestos *asbestos;
    struct tlb *tlb;
    pthread_mutex_t wait_mutex;
    pthread_cond_t wait_cond;
    bool reader_would_block;
    bool finished;
};

static void confirm_dirty_reader_would_block(struct asbestos *asbestos,
        pthread_mutex_t *mutex, pthread_cond_t *cond, bool *confirmed) {
    // Probe the same underlying rwlock immediately before entering the
    // production invalidation path. The compiler writer is already held, so
    // EBUSY proves this worker has reached the lock and its next blocking read
    // cannot pass until the stale block has been published.
    assert(pthread_rwlock_tryrdlock(&asbestos->dirty_coherence_lock.l) == EBUSY);
    assert(pthread_mutex_lock(mutex) == 0);
    *confirmed = true;
    assert(pthread_cond_signal(cond) == 0);
    assert(pthread_mutex_unlock(mutex) == 0);
}

static void wait_for_dirty_reader(struct drain_context *context) {
    assert(pthread_mutex_lock(&context->wait_mutex) == 0);
    while (!context->reader_would_block)
        assert(pthread_cond_wait(&context->wait_cond, &context->wait_mutex) == 0);
    assert(pthread_mutex_unlock(&context->wait_mutex) == 0);
}

static void *drain_dirty_pages(void *opaque) {
    struct drain_context *context = opaque;
    confirm_dirty_reader_would_block(context->asbestos, &context->wait_mutex,
            &context->wait_cond, &context->reader_would_block);
    assert(asbestos_invalidate_dirty_pages(context->asbestos, context->tlb));
    context->finished = true;
    return NULL;
}

static void test_drain_serializes_with_compile_insert(void) {
    struct asbestos *asbestos = asbestos_new(NULL);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    tlb_clear_runtime_dirty_pages(tlb);

    const page_t page = 333;
    tlb_mark_dirty_page(tlb, (addr_t) page << PAGE_BITS);
    struct drain_context context = {
        .asbestos = asbestos,
        .tlb = tlb,
    };
    assert(pthread_mutex_init(&context.wait_mutex, NULL) == 0);
    assert(pthread_cond_init(&context.wait_cond, NULL) == 0);

    // Model a compiler that already read old guest bytes and owns the complete
    // compile/insert lock order. Dirty draining must wait on the coherence
    // writer even while the page bucket is still empty; otherwise the stale
    // block can be inserted after clear.
    write_wrlock(&asbestos->dirty_coherence_lock);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, drain_dirty_pages, &context) == 0);
    wait_for_dirty_reader(&context);
    lock(&asbestos->lock);
    struct fiber_block *stale = add_block(asbestos, page);
    unlock(&asbestos->lock);
    write_wrunlock(&asbestos->dirty_coherence_lock);

    assert(pthread_join(thread, NULL) == 0);
    assert(context.finished);
    assert(stale->is_jetsam);
    assert(asbestos->num_blocks == 0);
    assert(invalidate_generation(asbestos) == 1);
    assert_dirty_set_empty(tlb);

    assert(pthread_cond_destroy(&context.wait_cond) == 0);
    assert(pthread_mutex_destroy(&context.wait_mutex) == 0);
    asbestos_free(asbestos);
    free(tlb);
}

struct page_invalidate_context {
    struct asbestos *asbestos;
    page_t page;
    pthread_mutex_t wait_mutex;
    pthread_cond_t wait_cond;
    bool reader_would_block;
    bool finished;
};

static void *invalidate_page(void *opaque) {
    struct page_invalidate_context *context = opaque;
    confirm_dirty_reader_would_block(context->asbestos, &context->wait_mutex,
            &context->wait_cond, &context->reader_would_block);
    asbestos_invalidate_page(context->asbestos, context->page);
    context->finished = true;
    return NULL;
}

static void wait_for_page_invalidator(struct page_invalidate_context *context) {
    assert(pthread_mutex_lock(&context->wait_mutex) == 0);
    while (!context->reader_would_block)
        assert(pthread_cond_wait(&context->wait_cond, &context->wait_mutex) == 0);
    assert(pthread_mutex_unlock(&context->wait_mutex) == 0);
}

static void test_page_invalidation_serializes_with_compile_insert(void) {
    struct asbestos *asbestos = asbestos_new(NULL);
    assert(asbestos != NULL);
    const page_t page = 444;
    struct page_invalidate_context context = {
        .asbestos = asbestos,
        .page = page,
    };
    assert(pthread_mutex_init(&context.wait_mutex, NULL) == 0);
    assert(pthread_cond_init(&context.wait_cond, NULL) == 0);

    // The page fast path must not inspect page_hash without synchronization.
    // Hold a compiler writer before it has published occupancy, then publish a
    // stale block: invalidate_page must wait and observe that publication.
    write_wrlock(&asbestos->dirty_coherence_lock);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, invalidate_page, &context) == 0);
    wait_for_page_invalidator(&context);
    lock(&asbestos->lock);
    struct fiber_block *stale = add_block(asbestos, page);
    unlock(&asbestos->lock);
    write_wrunlock(&asbestos->dirty_coherence_lock);

    assert(pthread_join(thread, NULL) == 0);
    assert(context.finished);
    assert(stale->is_jetsam);
    assert(asbestos->num_blocks == 0);
    assert(invalidate_generation(asbestos) == 1);

    assert(pthread_cond_destroy(&context.wait_cond) == 0);
    assert(pthread_mutex_destroy(&context.wait_mutex) == 0);
    asbestos_free(asbestos);
}

static void test_multi_page_invalidation(void) {
    struct asbestos *asbestos = asbestos_new(NULL);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    tlb_clear_runtime_dirty_pages(tlb);

    const page_t first_page = 3;
    const page_t second_page = 70;
    const page_t collision_page = first_page + FIBER_PAGE_HASH_SIZE;
    const page_t clean_page = 111;
    struct fiber_block *first = add_block(asbestos, first_page);
    struct fiber_block *second = add_block(asbestos, second_page);
    struct fiber_block *collision = add_block(asbestos, collision_page);
    struct fiber_block *clean = add_block(asbestos, clean_page);

    // The exact diagnostic slot ends on second_page, but both bucket bits must
    // survive. The colliding block is conservatively invalidated, matching the
    // existing page_hash granularity; an unrelated bucket remains compiled.
    tlb_mark_dirty_page(tlb, (addr_t) first_page << PAGE_BITS);
    tlb_mark_dirty_page(tlb, (addr_t) second_page << PAGE_BITS);
    assert(asbestos_invalidate_dirty_pages(asbestos, tlb));
    assert(first->is_jetsam);
    assert(second->is_jetsam);
    assert(collision->is_jetsam);
    assert(!clean->is_jetsam);
    assert(asbestos->num_blocks == 1);
    assert(invalidate_generation(asbestos) == 1);
    assert_dirty_set_empty(tlb);

    // Consuming the set is one-shot: a second boundary must not invalidate an
    // untouched block or advance the cache generation again.
    assert(!asbestos_invalidate_dirty_pages(asbestos, tlb));
    assert(!clean->is_jetsam);
    assert(invalidate_generation(asbestos) == 1);

    // Mixed/legacy markers may update only dirty_page. Even with an older bit
    // already present, the last exact page is ORed in conservatively.
    tlb_mark_dirty_page(tlb, (addr_t) 222 << PAGE_BITS);
    tlb->dirty_page = (addr_t) clean_page << PAGE_BITS;
    assert(asbestos_invalidate_dirty_pages(asbestos, tlb));
    assert(clean->is_jetsam);
    assert(asbestos->num_blocks == 0);
    assert(invalidate_generation(asbestos) == 2);
    assert_dirty_set_empty(tlb);

    asbestos_free(asbestos);
    free(tlb);
}

int main(void) {
    test_c_access_marking();
#ifdef GUEST_ARM64
    test_ic_ivau_dispatch_boundary();
#if defined(__aarch64__)
    test_arm64_high_tlb_alias_boundary();
    test_arm64_smc_direct_chain_boundary();
    test_arm64_smc_return_cache_boundary();
    test_arm64_two_page_smc_direct_chain();
#endif
#endif
#ifdef GUEST_X86
    test_exact_trace_survives_runtime_drain();
    test_x86_instruction_length_boundary();
    test_x86_smc_direct_chain_boundary();
#endif
    test_mem_did_write_post_invalidation();
    test_single_dirty_page_collision_is_exact();
    test_cross_page_end_invalidation_filters_collision();
    test_drain_serializes_with_compile_insert();
    test_page_invalidation_serializes_with_compile_insert();
    test_multi_page_invalidation();
    puts("dirty-page invalidation regressions passed");
    return 0;
}
