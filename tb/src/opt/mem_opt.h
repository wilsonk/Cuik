// Certain aliasing optimizations technically count as peepholes lmao, these can get fancy
// so the sliding window notion starts to break down but there's no global analysis and
// i can make them incremental technically so we'll go wit it.
typedef struct {
    TB_Node* base;
    int64_t offset;
} KnownPointer;

static KnownPointer known_pointer(TB_Node* n) {
    if (n->type == TB_MEMBER_ACCESS) {
        return (KnownPointer){ n->inputs[1], TB_NODE_GET_EXTRA_T(n, TB_NodeMember)->offset };
    } else {
        return (KnownPointer){ n, 0 };
    }
}

static TB_Node* ideal_load(TB_Passes* restrict p, TB_Function* f, TB_Node* n) {
    TB_Node* addr = n->inputs[2];
    if (n->inputs[0] != NULL) {
        TB_Node* base = addr;
        while (base->type == TB_MEMBER_ACCESS || base->type == TB_ARRAY_ACCESS) {
            base = base->inputs[1];
        }

        // loads based on LOCALs don't need control-dependence, it's actually kinda annoying
        if (base->type == TB_LOCAL) {
            set_input(p, n, NULL, 0);
            return n;
        }
    }

    // loads based on PHIs may be reduced into data PHIs
    if (n->inputs[1]->type == TB_PHI) {
        assert(n->inputs[1]->dt.type == TB_MEMORY && "memory input should be memory");

        TB_Arena* func_arena = p->f->arena;
        TB_ArenaSavepoint sp = tb_arena_save(func_arena);

        size_t path_count = n->inputs[1]->input_count - 1;
        TB_Node** paths = tb_arena_alloc(func_arena, path_count * sizeof(TB_Node*));

        // walk each path to find relevant STOREs
        TB_Node** phi_ins = n->inputs[1]->inputs;
        FOREACH_N(i, 0, path_count) {
            TB_Node* head = phi_ins[1 + i];
            TB_Node* ctrl = head->inputs[0];
            TB_Node* path = NULL;

            do {
                if (head->type == TB_STORE && addr == head->inputs[2]) {
                    path = head->inputs[3];
                    break;
                }

                // previous memory effect
                head = head->inputs[1];
            } while (head->inputs[0] == ctrl);

            if (path == NULL) {
                // we have a path with an unknown value... sadge
                tb_arena_restore(func_arena, sp);
                return NULL;
            }
            paths[i] = path;
        }

        // convert to PHI
        TB_Node* phi = tb_alloc_node(f, TB_PHI, n->dt, 1 + path_count, 0);
        set_input(p, phi, phi_ins[0], 0);
        FOREACH_N(i, 0, path_count) {
            set_input(p, phi, paths[i], 1+i);
        }

        return phi;
    }

    // if a load is control dependent on a store and it doesn't alias we can move the
    // dependency up a bit.
    /*if (n->inputs[1]->type != TB_STORE) return NULL;

    KnownPointer ld_ptr = known_pointer(n->inputs[2]);
    KnownPointer st_ptr = known_pointer(n->inputs[1]->inputs[2]);
    if (ld_ptr.base != st_ptr.base) return NULL;

    // it's probably not the fastest way to grab this value ngl...
    ICodeGen* cg = tb__find_code_generator(f->super.module);
    ld_ptr.offset *= cg->minimum_addressable_size;
    st_ptr.offset *= cg->minimum_addressable_size;

    size_t loaded_end = ld_ptr.offset + bits_in_data_type(cg->pointer_size, n->dt);
    size_t stored_end = st_ptr.offset + bits_in_data_type(cg->pointer_size, n->inputs[0]->inputs[2]->dt);

    // both bases match so if the effective ranges don't intersect, they don't alias.
    if (ld_ptr.offset <= stored_end && st_ptr.offset <= loaded_end) return NULL;

    set_input(p, n, n->inputs[1]->inputs[1], 1);
    return n;*/
    return NULL;
}

static TB_Node* identity_load(TB_Passes* restrict p, TB_Function* f, TB_Node* n) {
    // god i need a pattern matcher
    //   (load (store X A Y) A) => Y
    TB_Node *mem = n->inputs[1], *addr = n->inputs[2];
    if (mem->type == TB_STORE && mem->inputs[2] == addr &&
        n->dt.raw == mem->inputs[3]->dt.raw && is_same_align(n, mem)) {
        return mem->inputs[3];
    }

    return n;
}

static TB_Node* ideal_store(TB_Passes* restrict p, TB_Function* f, TB_Node* n) {
    TB_Node *mem = n->inputs[1], *addr = n->inputs[2], *val = n->inputs[3];
    TB_DataType dt = val->dt;

    // if a store has only one user in this chain it means it's only job was
    // to facilitate the creation of that user store... if we can detect that
    // user store is itself dead, everything in the middle is too.
    while (mem->type == TB_STORE && single_use(p, mem)) {
        if (mem->inputs[2] == addr && mem->inputs[3]->dt.raw == dt.raw && is_same_align(n, mem)) {
            set_input(p, n, mem->inputs[1], 1);
            return n;
        }

        mem = mem->inputs[1];
    }

    return NULL;
}

// LOCALs can be unlinked here
static TB_Node* dead_mem(TB_Passes* restrict p, TB_Function* f, TB_Node* mem) {
    if (single_use(p, mem) && mem->type == TB_STORE && mem->inputs[2]->type == TB_LOCAL) {
        tb_pass_kill_node(p, mem);
        return mem->inputs[1];
    }

    return NULL;
}

static TB_Node* ideal_end(TB_Passes* restrict p, TB_Function* f, TB_Node* n) {
    // END will perform dead store on local vars
    bool changes = false;
    /*TB_Node* mem = n->inputs[1];
    if (mem->type == TB_PHI) {
        FOREACH_N(i, 1, mem->input_count) {
            TB_Node* above = dead_mem(p, f, mem->inputs[i]);
            if (above) {
                set_input(p, mem, above, i);
                changes = true;
            }
        }
    } else {
        TB_Node* above = dead_mem(p, f, mem);
        if (above) {
            set_input(p, n, above, 1);
            changes = true;
        }
    }*/

    return changes ? n : NULL;
}

static TB_Node* ideal_memcpy(TB_Passes* restrict p, TB_Function* f, TB_Node* n) {
    return NULL;
}
