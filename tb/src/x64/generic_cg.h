#include "../tb_internal.h"
#include "../codegen/emitter.h"

enum {
    CG_VAL_UNRESOLVED = 0,
    CG_VAL_FLAGS      = 1,
    CG_VAL_REGISTER   = 2,
};

static void get_data_type_size(TB_DataType dt, TB_CharUnits* out_size, TB_CharUnits* out_align) {
    switch (dt.type) {
        case TB_INT: {
            // above 64bits we really dont care that much about natural alignment
            bool is_big_int = dt.data > 64;

            // round up bits to a byte
            int bits = is_big_int ? ((dt.data + 7) / 8) : tb_next_pow2(dt.data - 1);

            *out_size  = ((bits+7) / 8) << dt.width;
            *out_align = is_big_int ? 8 : ((dt.data + 7) / 8);
            break;
        }
        case TB_FLOAT: {
            int s = 0;
            if (dt.data == TB_FLT_32) s = 4;
            else if (dt.data == TB_FLT_64) s = 8;
            else tb_unreachable();

            *out_size = s << dt.width;
            *out_align = s;
            break;
        }
        case TB_PTR: {
            *out_size = 8;
            *out_align = 8;
            break;
        }
        default: tb_unreachable();
    }
}

typedef struct {
    TB_Node* key;

    int t, user_count;
} Liveness;

typedef struct {
    TB_Node* key;
    int val;
} ValueDesc;

typedef struct Sequence {
    struct Sequence* next;

    TB_Node* node;

    int inst_count, label;
    Inst insts[16];
} Sequence;

typedef struct MachineBB {
    Sequence* first;

    // on the timeline/slot indices
    int start, end;

    // local live sets
    Set gen, kill;
    // global
    Set live_in, live_out;
} MachineBB;

typedef struct Def {
    TB_Node* node;

    // lifetime
    int start, end;

    // regalloc
    int reg_class, reg, hint;

    // when we preallocate a definition we
    // specify here which definition must
    // be completed for it to be free again
    int live_until;
} Def;

typedef struct Ctx {
    TB_CGEmitter emit;

    TB_Module* module;
    TB_Function* f;
    TB_ABI target_abi;

    // machine output sequences
    Sequence *first, *last;
    DynArray(Def) defs;
    DynArray(TB_Node*) in_bound;

    // hash map
    size_t interval_count, interval_exp;
    Liveness* intervals;

    TB_Label fallthrough;

    // Stack
    uint32_t stack_usage;
    NL_Map(TB_Node*, int) stack_slots;

    // Reg alloc
    Def** active;
    size_t active_count;

    Set free_regs[CG_REGISTER_CLASSES];

    // current value table
    size_t values_count, values_exp;
    ValueDesc* values;
} Ctx;

#if 1
#define ASM if (ctx->emit.emit_asm)
#else
#define ASM if (0)
#endif

#define NAME(n) (get_liveness(ctx, n)->t)
static Liveness* get_liveness(Ctx* restrict ctx, TB_Node* n) {
    uint32_t hash = (((uintptr_t) n) * 11400714819323198485llu) >> 32u;

    for (size_t i = hash;;) {
        // hash table lookup
        uint32_t mask = (1 << ctx->interval_exp) - 1;
        uint32_t step = (hash >> (32 - ctx->interval_exp)) | 1;
        i = (i + step) & mask;

        if (ctx->intervals[i].key == NULL) {
            return NULL;
        } else if (ctx->intervals[i].key == n) {
            return &ctx->intervals[i];
        }
    }
}

static void put_liveness(Ctx* restrict ctx, TB_Node* n, int ordinal) {
    uint32_t hash = (((uintptr_t) n) * 11400714819323198485llu) >> 32u;

    for (size_t i = hash;;) {
        // hash table lookup
        uint32_t mask = (1 << ctx->interval_exp) - 1;
        uint32_t step = (hash >> (32 - ctx->interval_exp)) | 1;
        i = (i + step) & mask;

        if (ctx->intervals[i].key == NULL) {
            assert(ctx->interval_count + 1 < (1u << ctx->interval_exp));

            // new slot
            ctx->interval_count++;
            ctx->intervals[i] = (Liveness){ .key = n, .t = ordinal };
            return;
        } else if (ctx->intervals[i].key == n) {
            assert(0 && "Huh?");
        }
    }
}

static bool try_tile(Ctx* restrict ctx, TB_Node* n) {
    dyn_array_for(i, ctx->in_bound) if (ctx->in_bound[i] == n) {
        dyn_array_remove(ctx->in_bound, i);
        return true;
    }

    return false;
}

#define DEF(n, rg) put_def(ctx, n, rg)
static int put_def(Ctx* restrict ctx, TB_Node* n, int reg_class) {
    int i = dyn_array_length(ctx->defs);
    dyn_array_put(ctx->defs, (Def){ .node = n, .start = INT_MAX, .reg_class = reg_class, .reg = -1, .hint = -1 });
    return i;
}

#define DEF_HINTED(n, rg, hint) put_def_hinted(ctx, n, rg, hint)
static int put_def_hinted(Ctx* restrict ctx, TB_Node* n, int reg_class, int hint) {
    int i = dyn_array_length(ctx->defs);
    dyn_array_put(ctx->defs, (Def){ .node = n, .start = INT_MAX, .end = INT_MIN, .reg_class = reg_class, .reg = -1, .hint = hint });
    return i;
}

#define DEF_FORCED(n, rg, reg, live_until) put_def_forced(ctx, n, rg, reg, live_until)
static int put_def_forced(Ctx* restrict ctx, TB_Node* n, int reg_class, int reg, int live_until) {
    int i = dyn_array_length(ctx->defs);
    dyn_array_put(ctx->defs, (Def){ .node = n, .start = INT_MAX, .end = INT_MIN, .reg_class = reg_class, .reg = reg, .live_until = live_until });
    return i;
}

#define GET_VAL(n) (*get_val(ctx, n))
static int* get_val(Ctx* restrict ctx, TB_Node* n) {
    uint32_t hash = (((uintptr_t) n) * 11400714819323198485llu) >> 32u;

    for (size_t i = hash;;) {
        // hash table lookup
        uint32_t mask = (1 << ctx->values_exp) - 1;
        uint32_t step = (hash >> (32 - ctx->values_exp)) | 1;
        i = (i + step) & mask;

        if (ctx->values[i].key == NULL) {
            assert(ctx->values_count + 1 < (1u << ctx->values_exp));

            // new slot
            ctx->values_count++;
            ctx->values[i].key = n;
            ctx->values[i].val = -1;
            return &ctx->values[i].val;
        } else if (ctx->values[i].key == n) {
            return &ctx->values[i].val;
        }
    }
}

static bool fits_into_int8(uint64_t x) {
    int8_t y = x & 0xFFFFFFFF;
    return (int64_t)y == x;
}

static bool fits_into_int32(uint64_t x) {
    int32_t y = x & 0xFFFFFFFF;
    return (int64_t)y == x;
}

static int classify_reg_class(TB_DataType dt);
static int isel(Ctx* restrict ctx, Sequence* restrict seq, TB_Node* n);
static void emit_sequence(Ctx* restrict ctx, Sequence* restrict seq, TB_Node* n);
static void patch_local_labels(Ctx* restrict ctx);
static void resolve_stack_usage(Ctx* restrict ctx, size_t caller_usage);
static void copy_value(Ctx* restrict ctx, Sequence* seq, TB_Node* phi, int dst, TB_Node* src, TB_DataType dt);

#define ISEL(n) USE(isel(ctx, seq, n))

// references an allocated
#define USE(x) (-((x) + 2))
#define USE_VAL(n) (-(GET_VAL(n) + 2))

static void add_active(Ctx* restrict ctx, Def* restrict d) {
    // insert by increasing end point
    // TODO(NeGate): do binary insert since the array is sorted
    size_t i = 0;
    for (; i < ctx->active_count; i++) {
        if (ctx->active[i]->end >= d->end) break;
    }

    // we know where to insert
    FOREACH_REVERSE_N(j, i, ctx->active_count) {
        ctx->active[j+1] = ctx->active[j];
    }

    ctx->active[i] = d;
    ctx->active_count += 1;
}

static void remove_active(Ctx* restrict ctx, size_t i) {
    if (i + 1 != ctx->active_count) {
        memmove(&ctx->active[i], &ctx->active[i + 1], (ctx->active_count - i) * sizeof(Def*));
    }
    ctx->active_count -= 1;
}

static size_t estimate_hash_map_size(size_t s) {
    // allocate values map and active, for linear scan
    size_t ht_cap = tb_next_pow2((s * 8) / 5);
    size_t ht_exp = 32 - tb_clz(ht_cap - 1);

    assert(ht_cap == (1u << ht_exp));
    return ht_exp;
}

static void phi_edge(TB_Function* f, Ctx* restrict ctx, Sequence* restrict seq, TB_Label src, TB_Label dst) {
    TB_FOR_NODE(n, f, dst) {
        if (n->type == TB_NULL) continue;
        if (n->type != TB_PHI) break;

        // allocate virtual register
        int* dst_vreg = &GET_VAL(n);
        if (*dst_vreg < 0) {
            *dst_vreg = DEF(n, classify_reg_class(n->dt));
        }

        // handle phis
        TB_NodePhi* phi = TB_NODE_GET_EXTRA(n);
        FOREACH_N(i, 0, n->input_count) if (phi->labels[i] == src) {
            copy_value(ctx, seq, n, USE(*dst_vreg), n->inputs[i], n->dt);
            break;
        }
    }
}

////////////////////////////////
// Linear scan reg allocation
////////////////////////////////
static int compare_defs(const void* a, const void* b) {
    int as = (*(Def**) a)->start;
    int bs = (*(Def**) b)->start;

    return as - bs;
}

static void add_range(Ctx* restrict ctx, Def* restrict d, int start, int end) {
    assert(start <= end);

    if (start < d->start) d->start = start;
    if (end < d->start) d->start = end;

    // max
    if (start > d->end) d->end = start;
    if (end > d->end) d->end = end;
}

static void reverse_bb_walk(Ctx* restrict ctx, TB_Function* f, MachineBB* bb, Sequence* seq) {
    Sequence* next = seq->next;
    if (next && next->label == seq->label) {
        reverse_bb_walk(ctx, f, bb, next);
    }

    FOREACH_REVERSE_N(i, 0, seq->inst_count) {
        Inst* restrict inst = &seq->insts[i];

        // mark def
        if (inst->regs[0] >= 0) {
            Def* d = &ctx->defs[inst->regs[0]];

            if (d->start >= 0) {
                d->start = inst->time;
                if (d->end == INT_MIN) d->end = inst->time;
            }
        }

        // mark users
        FOREACH_N(j, 1, 4) if (inst->regs[j] < -1) {
            Def* d = &ctx->defs[-inst->regs[j] - 2];

            add_range(ctx, d, bb->start, inst->time + (j == 1 ? 0 : 1));
        }
    }
}

static void linear_scan(Ctx* restrict ctx, TB_Function* f, TB_PostorderWalk* walk, size_t node_count) {
    size_t def_count = dyn_array_length(ctx->defs);

    // generate live intervals for virtual registers
    CUIK_TIMED_BLOCK("build intervals") {
        // find BB boundaries in sequences
        MachineBB* seq_bb = ARENA_ARR_ALLOC(&tb__arena, f->bb_count, MachineBB);

        FOREACH_N(bb, 0, f->bb_count) {
            seq_bb[bb] = (MachineBB){
                .gen = set_create(def_count), .kill = set_create(def_count),
                .live_in = set_create(def_count), .live_out = set_create(def_count)
            };
        }

        // generate local live sets
        if (ctx->first) {
            Set copy_init = set_create(def_count);

            Sequence* seq = ctx->first;
            seq_bb[seq->label].first = seq;
            seq_bb[seq->label].start = 0;

            int last_bb = seq->label, timeline = 0;
            for (; seq; seq = seq->next) {
                TB_Label bb = seq->label;
                if (last_bb != bb) {
                    seq_bb[last_bb].end = timeline;
                    last_bb = bb;
                    timeline += 2; // reserved two extra spaces at the end of the BB

                    seq_bb[bb].first = seq->next;
                    seq_bb[bb].start = timeline;
                }

                Set* gen = &seq_bb[bb].gen;
                Set* kill = &seq_bb[bb].kill;

                FOREACH_N(i, 0, seq->inst_count) {
                    Inst* restrict inst = &seq->insts[i];
                    inst->time = timeline;

                    if (inst->type == X86_INST_MOVE) {
                        assert(inst->regs[1] < -1);
                        int di = -inst->regs[1] - 2;

                        if (!set_get(&copy_init, di)) {
                            set_put(&copy_init, di);

                            inst->type = (int) X86_INST_COPY;
                            inst->regs[0] = USE(inst->regs[1]);
                            inst->regs[1] = inst->regs[2];
                            inst->regs[2] = 0;
                        }
                    }

                    FOREACH_N(j, 1, 4) if (inst->regs[j] < -1) {
                        int di = -inst->regs[j] - 2;
                        if (!set_get(kill, di)) {
                            set_put(gen, di);
                        }
                    }

                    if (inst->regs[0] >= 0) {
                        set_put(kill, inst->regs[0]);
                    }

                    timeline += 2;
                }
            }

            seq_bb[last_bb].end = timeline;
            set_free(&copy_init);
        }

        // generate global live sets
        bool changes;
        do {
            changes = false;

            FOREACH_REVERSE_N(bb, 0, f->bb_count) {
                set_clear(&seq_bb[bb].live_out);

                // walk all successors
                TB_Node* end = f->bbs[bb].end;
                if (end && end->type == TB_BRANCH) {
                    TB_NodeBranch* br = TB_NODE_GET_EXTRA(end);
                    FOREACH_N(i, 0, br->count) {
                        // union with successor's lives
                        TB_Label succ = br->targets[i].value;
                        set_union(&seq_bb[bb].live_out, &seq_bb[succ].live_in);
                    }

                    set_union(&seq_bb[bb].live_out, &seq_bb[br->default_label].live_in);
                }

                Set* live_in = &seq_bb[bb].live_in;
                Set* live_out = &seq_bb[bb].live_out;
                Set* kill = &seq_bb[bb].kill;
                Set* gen = &seq_bb[bb].gen;

                // live_in = (live_out - live_kill) U live_gen
                FOREACH_N(i, 0, (def_count + 63) / 64) {
                    uint64_t new = (live_out->data[i] & ~kill->data[i]) | gen->data[i];

                    changes |= (live_in->data[i] != new);
                    live_in->data[i] = new;
                }
            }
        } while (changes);

        // this is a reverse BB walk
        FOREACH_N(i, 0, walk->count) {
            TB_Label bb = walk->traversal[i];

            int bb_start = seq_bb[bb].start;
            int bb_end = seq_bb[bb].end + 2;

            // for anything that's live out, add the entire range
            Set* live_in = &seq_bb[bb].live_in;
            Set* live_out = &seq_bb[bb].live_out;
            FOREACH_N(i, 0, (def_count + 63) / 64) {
                uint64_t bits = live_in->data[i] & live_out->data[i];
                if (bits == 0) continue;

                FOREACH_N(j, 0, 64) if (bits & (1ull << j)) {
                    size_t k = (i*64) + j;
                    add_range(ctx, &ctx->defs[k], bb_start, bb_end);
                }
            }

            // for all instruction in BB (in reverse), add ranges
            if (seq_bb[bb].first) {
                reverse_bb_walk(ctx, f, &seq_bb[bb], seq_bb[bb].first);
            }
        }
    }

    // sort by starting point
    Def** sorted = tb_platform_heap_alloc(def_count * sizeof(Def*));
    FOREACH_N(i, 0, def_count) {
        Def* d = &ctx->defs[i];
        if (d->reg >= 0 && d->live_until >= 0) {
            Def* until = &ctx->defs[d->live_until];
            add_range(ctx, d, until->end, until->end);
        }

        sorted[i] = d;
    }
    qsort(sorted, def_count, sizeof(Def*), compare_defs);

    FOREACH_N(i, 0, def_count) {
        Def* d = sorted[i];
        int time = d->start;

        ASM {
            printf("  \x1b[32m# D%zu t=[%d,%d) ", sorted[i] - ctx->defs, time, d->end);
            if (d->node) printf("r%d", NAME(d->node));
            printf("\x1b[0m\n");
        }

        // expire intervals
        for (size_t i = 0; i < ctx->active_count;) {
            Def* k = ctx->active[i];
            if (k->end > time) break;

            if (k->reg >= 0) {
                ASM {
                    printf("  \x1b[32m#   free %s\x1b[0m\n", GPR_NAMES[k->reg]);
                }
                set_remove(&ctx->free_regs[k->reg_class], k->reg);
                remove_active(ctx, i);
            } else {
                i++;
            }
        }

        ptrdiff_t reg_num = -1;
        // try to allocate hint
        if (d->reg >= 0) {
            if (set_get(&ctx->free_regs[d->reg_class], d->reg)) {
                // spill previous user until the end of the definition's lifetime
                // tb_todo();
            }

            reg_num = d->reg;
            ASM {
                printf("  \x1b[32m#   forced assign %s\x1b[0m\n", GPR_NAMES[reg_num]);
            }
        } else if (d->hint >= 0 && !set_get(&ctx->free_regs[d->reg_class], d->hint)) {
            reg_num = d->hint;
            ASM {
                printf("  \x1b[32m#   hinted assign %s\x1b[0m\n", GPR_NAMES[reg_num]);
            }
        } else {
            // try standard allocation
            reg_num = set_pop_any(&ctx->free_regs[d->reg_class]);
            ASM {
                printf("  \x1b[32m#   assign %s\x1b[0m\n", GPR_NAMES[reg_num]);
            }
        }

        if (reg_num < 0) {
            // spill to make room
            tb_todo();
        }

        set_put(&ctx->free_regs[d->reg_class], reg_num);
        add_active(ctx, d);
        d->reg = reg_num;
    }
    tb_platform_heap_free(sorted);
}

static Sequence* alloc_sequence(int label, TB_Node* n) {
    Sequence* restrict s = arena_alloc(&tb__arena, sizeof(Sequence), _Alignof(Sequence));
    s->next = NULL;
    s->node = n;
    s->label = label;
    s->inst_count = 0;
    return s;
}

static void append_sequence(Ctx* restrict ctx, Sequence* seq) {
    if (ctx->last == NULL) {
        ctx->first = ctx->last = seq;
    } else {
        ctx->last->next = seq;
        ctx->last = seq;
    }
}

static void fence(Ctx* restrict ctx, TB_Label bb) {
    while (dyn_array_length(ctx->in_bound)) {
        TB_Node* n = dyn_array_pop(ctx->in_bound);

        Sequence* seq = alloc_sequence(bb, n);
        isel(ctx, seq, n);
        append_sequence(ctx, seq);
    }
    dyn_array_clear(ctx->in_bound);
}

// Codegen through here is done in phases
static TB_FunctionOutput compile_function(TB_Function* restrict f, const TB_FeatureSet* features, uint8_t* out, size_t out_capacity) {
    TB_TemporaryStorage* tls = tb_tls_allocate();
    tb_function_print(f, tb_default_print_callback, stdout, false);

    Ctx* restrict ctx = arena_alloc(&tb__arena, sizeof(Ctx), _Alignof(Ctx));
    *ctx = (Ctx){
        .module = f->super.module,
        .f = f,
        .target_abi = f->super.module->target_abi,
        .emit = {
            .f = f,
            .data = out,
            .capacity = out_capacity,
        }
    };

    ctx->free_regs[0] = set_create(16);
    ctx->free_regs[1] = set_create(16);

    set_put(&ctx->free_regs[0], RBP), set_put(&ctx->free_regs[0], RSP);

    // BB scheduling:
    //   we run through BBs in a reverse postorder walk, currently
    //   there's no reodering based on branch weights (since we don't
    //   do those but if we did that would go here.
    TB_PostorderWalk walk = {
        .visited = tb_tls_push(tls, f->bb_count * sizeof(bool)),
        .traversal = tb_tls_push(tls, f->bb_count * sizeof(TB_Node*)),
    };
    tb_function_get_postorder_explicit(f, &walk);
    assert(walk.traversal[walk.count - 1] == 0 && "Codegen must always schedule entry BB first");

    // Live intervals:
    //   We compute this for register allocation along
    //   with the "ordinals" which act as our timeline.
    ctx->interval_exp = estimate_hash_map_size(f->node_count);
    size_t interval_cap = (1u << ctx->interval_exp);

    ctx->intervals = arena_alloc(&tb__arena, interval_cap * sizeof(Liveness), _Alignof(Liveness));
    memset(ctx->intervals, 0, interval_cap * sizeof(Liveness));

    int counter = 0, label_patch_count = 0, return_count = 0, caller_usage = 0, line_count = 0;
    FOREACH_REVERSE_N(i, 0, walk.count) {
        TB_Label bb = walk.traversal[i];

        TB_FOR_NODE(n, f, bb) if (1) {
            put_liveness(ctx, n, counter++);

            line_count += (n->type == TB_LINE_INFO);
        }
    }

    mtx_lock(&f->super.module->lock);
    f->line_count = 0;
    f->lines = ARENA_ARR_ALLOC(&f->super.module->arena, line_count, TB_Line);
    mtx_unlock(&f->super.module->lock);

    // allocate values map and active, for linear scan
    ctx->values_exp = estimate_hash_map_size(counter);
    size_t values_cap = (1u << ctx->values_exp);

    ctx->values = arena_alloc(&tb__arena, values_cap * sizeof(ValueDesc), _Alignof(ValueDesc));
    ctx->active = arena_alloc(&tb__arena, counter * sizeof(TB_Node*), _Alignof(TB_Node*));
    memset(ctx->values, 0, values_cap * sizeof(ValueDesc));

    CUIK_TIMED_BLOCK("liveness analysis") {
        TB_FOR_BASIC_BLOCK(bb, f) {
            TB_FOR_NODE(n, f, bb) if (n->type != TB_NULL) {
                if (n->type == TB_BRANCH) {
                    label_patch_count += 1 + TB_NODE_GET_EXTRA_T(n, TB_NodeBranch)->count;
                } else if (n->type == TB_RET) {
                    return_count += 1;
                } else if (n->type == TB_CALL) {
                    // system calls don't count, we track this for ABI
                    // and stack allocation purposes.
                    if (caller_usage < n->input_count) {
                        caller_usage = n->input_count;
                    }
                }

                TB_FOR_INPUT_IN_NODE(in, n) if (*in) {
                    Liveness* l = get_liveness(ctx, *in);
                    l->user_count++;
                }
            }
        }
    }

    // allocate more stuff now that we've run stats on the IR
    ctx->emit.labels = ARENA_ARR_ALLOC(&tb__arena, f->bb_count, uint32_t);
    ctx->emit.label_patches = ARENA_ARR_ALLOC(&tb__arena, label_patch_count, LabelPatch);
    ctx->emit.ret_patches = ARENA_ARR_ALLOC(&tb__arena, return_count, ReturnPatch);

    // Instruction selection:
    //   we just decide which instructions to emit, which operands are
    //   fixed and which need allocation. For now regalloc is handled
    //   immediately but in theory it could be delayed until all selection
    //   is done.
    ctx->emit.emit_asm = true;
    CUIK_TIMED_BLOCK("isel") FOREACH_REVERSE_N(i, 0, walk.count) {
        TB_Label bb = walk.traversal[i];

        // mark fallthrough
        ctx->fallthrough = (i > 0 ? walk.traversal[i - 1] : -1);

        TB_FOR_NODE(n, f, bb) if (n->type != TB_NULL) {
            Sequence* seq = alloc_sequence(bb, n);

            // build up tile
            Liveness* l = get_liveness(ctx, n);
            if (l->user_count <= 1 && tb_is_expr_like(n)) {
                if (l->user_count == 1) dyn_array_put(ctx->in_bound, n);
                continue;
            }

            // Handle branch edges
            if (n->type == TB_BRANCH) {
                // copy out from active phi-edges
                TB_NodeBranch* br = TB_NODE_GET_EXTRA(n);
                FOREACH_REVERSE_N(i, 0, br->count) {
                    phi_edge(f, ctx, seq, bb, br->targets[i].value);
                }
                phi_edge(f, ctx, seq, bb, br->default_label);
            }

            isel(ctx, seq, n);
            append_sequence(ctx, seq);

            fence(ctx, bb);
        }
    }

    CUIK_TIMED_BLOCK("linear scan") {
        linear_scan(ctx, f, &walk, counter);
    }

    CUIK_TIMED_BLOCK("emit sequences") {
        TB_Label last = -1;
        for (Sequence* seq = ctx->first; seq; seq = seq->next) {
            // mark label
            TB_Label bb = seq->label;
            if (last != bb) {
                ctx->emit.labels[bb] = GET_CODE_POS(&ctx->emit);

                ASM { printf("L%d:\n", bb); }
                last = bb;
            }

            if (seq->inst_count == 0) {
                continue;
            }

            TB_Node* n = seq->node;
            if (n != NULL) {
                if (n->type == TB_LINE_INFO) {
                    TB_NodeLine* l = TB_NODE_GET_EXTRA(n);
                    f->lines[f->line_count++] = (TB_Line) {
                        .file = l->file,
                        .line = l->line,
                        .pos = GET_CODE_POS(&ctx->emit)
                    };
                    continue;
                }

                ASM { printf("  \x1b[32m# %s sequence\x1b[0m\n", tb_node_get_name(n)); }
            } else {
                ASM { printf("  \x1b[32m# magic sequence\x1b[0m\n"); }
            }

            // Arch-specific: convert instruction buffer into actual instructions
            emit_sequence(ctx, seq, n);
        }
    }

    // __debugbreak();
    ASM {
        printf(".ret:\n");
    }

    resolve_stack_usage(ctx, caller_usage);

    //  Label patching: we make sure any local labels
    patch_local_labels(ctx);

    // we're done, clean up
    TB_FunctionOutput func_out = {
        .linkage = f->linkage,
        .code = ctx->emit.data,
        .code_size = ctx->emit.count,
        .stack_usage = ctx->stack_usage,
        // .prologue_epilogue_metadata = ctx->regs_to_save[0],
        // .stack_slots = ctx->stack_slots
    };

    arena_clear(&tb__arena);
    // __debugbreak();
    return func_out;
}
