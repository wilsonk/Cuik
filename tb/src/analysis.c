// This file contains generic analysis functions for operating on the TBIR
#include "tb_internal.h"

typedef struct {
    TB_Function* f;
    TB_PostorderWalk order;
} DomContext;

// we'll be walking backwards from the end node
static void postorder(TB_Function* f, TB_PostorderWalk* ctx, TB_Node* n) {
    TB_BasicBlock bb = { .end = n };
    while (n->type != TB_REGION && n->type != TB_START) {
        assert(tb_has_effects(n));
        n = n->inputs[0];
    }

    ptrdiff_t search = nl_map_get(ctx->visited, n);
    if (search >= 0) {
        return;
    }

    nl_map_put(ctx->visited, n, 0);

    // walk control edges (aka predecessors)
    FOREACH_N(i, 0, n->input_count) {
        postorder(f, ctx, n->inputs[i]);
    }

    bb.start = n;
    ctx->traversal[ctx->count++] = bb;
}

TB_API TB_PostorderWalk tb_function_get_postorder(TB_Function* f) {
    TB_PostorderWalk walk = {
        .traversal = tb_platform_heap_alloc(f->control_node_count * sizeof(TB_Node*))
    };

    TB_FOR_RETURNS(n, f) {
        postorder(f, &walk, n);
    }

    nl_map_free(walk.visited);
    return walk;
}

TB_API void tb_function_free_postorder(TB_PostorderWalk* walk) {
    nl_map_free(walk->traversal);
}

static int find_traversal_index(DomContext* ctx, TB_Node* bb) {
    FOREACH_N(i, 0, ctx->order.count) {
        if (ctx->order.traversal[i].start == bb) return i;
    }

    tb_todo();
}

TB_API TB_DominanceFrontiers tb_get_dominance_frontiers(TB_Function* f, TB_Dominators doms, const TB_PostorderWalk* order) {
    TB_DominanceFrontiers df = NULL;

    FOREACH_N(i, 0, order->count) {
        TB_Node* bb = order->traversal[i].start;

        if (bb->input_count >= 2) {
            FOREACH_N(k, 0, bb->input_count) {
                TB_Node* runner = bb->inputs[k];
                while (runner != 0 && runner != nl_map_get_checked(doms, bb)) {
                    // add to frontier set
                    TB_FrontierSet* set = &nl_map_get_checked(df, runner);
                    nl_map_put(*set, bb, 0);
                    runner = nl_map_get_checked(doms, runner);
                }
            }
        }
    }

    return df;
}

TB_API void tb_free_dominance_frontiers(TB_Function* f, TB_DominanceFrontiers frontiers, const TB_PostorderWalk* order) {
    FOREACH_N(i, 0, nl_map_get_capacity(frontiers)) if (frontiers[i].k != NULL) {
        nl_map_free(frontiers[i].v);
    }
    nl_map_free(frontiers);
}

// https://www.cs.rice.edu/~keith/EMBED/dom.pdf
TB_API TB_Dominators tb_get_dominators(TB_Function* f) {
    // entrypoint dominates itself
    DomContext ctx = { .f = f };

    TB_Dominators doms = NULL;
    nl_map_create(doms, f->control_node_count);
    nl_map_put(doms, f->start_node, f->start_node);

    // identify post order traversal order
    ctx.order = tb_function_get_postorder(f);

    bool changed = true;
    while (changed) {
        changed = false;

        // for all nodes, b, in reverse postorder (except start node)
        FOREACH_REVERSE_N(i, 0, ctx.order.count - 1) {
            TB_Node* b = ctx.order.traversal[i].start;
            TB_Node* new_idom = b->inputs[0];

            // for all other predecessors, p, of b
            FOREACH_N(j, 1, b->input_count) {
                TB_Node* p = b->inputs[j];

                // if doms[p] already calculated
                ptrdiff_t search_p = nl_map_get(doms, p);
                if (search_p < 0) {
                    int a = find_traversal_index(&ctx, p);
                    int b = find_traversal_index(&ctx, new_idom);

                    while (a != b) {
                        // while (finger1 < finger2)
                        //   finger1 = doms[finger1]
                        while (a < b) {
                            a = find_traversal_index(&ctx, nl_map_get_checked(doms, ctx.order.traversal[a]));
                        }

                        // while (finger2 < finger1)
                        //   finger2 = doms[finger2]
                        while (b < a) {
                            b = find_traversal_index(&ctx, nl_map_get_checked(doms, ctx.order.traversal[b]));
                        }
                    }

                    new_idom = ctx.order.traversal[a].start;
                }
            }

            ptrdiff_t search_b = nl_map_get(doms, b);
            assert(search_b >= 0);

            if (doms[search_b].v != new_idom) {
                doms[search_b].v = new_idom;
                changed = true;
            }
        }
    }

    return doms;
}

TB_API bool tb_is_dominated_by(TB_Dominators doms, TB_Node* expected_dom, TB_Node* bb) {
    while (bb != 0 && expected_dom != bb) {
        ptrdiff_t search = nl_map_get(doms, bb);
        if (search < 0) break;

        bb = doms[search].v;
    }

    return (expected_dom == bb);
}

TB_API TB_LoopInfo tb_get_loop_info(TB_Function* f, TB_PostorderWalk order, TB_Dominators doms) {
    // Find loops
    DynArray(TB_Loop) loops = dyn_array_create(TB_Loop, 64);
    FOREACH_N(i, 0, order.count) {
        TB_Node* bb = order.traversal[i].start;

        TB_Node* backedge = NULL;
        FOREACH_N(j, 0, bb->input_count) {
            if (tb_is_dominated_by(doms, bb, bb->inputs[j])) {
                backedge = bb->inputs[j];
                break;
            }
        }

        if (backedge) {
            TB_Loop l = { .parent_loop = -1, .header = bb, .backedge = backedge };

            // check if we have a parent...
            FOREACH_REVERSE_N(o, 0, dyn_array_length(loops)) {
                if (tb_is_dominated_by(doms, loops[o].header, bb)) {
                    l.parent_loop = o;
                    break;
                }
            }

            dyn_array_put(loops, l);
        }
    }

    return (TB_LoopInfo){ .count = dyn_array_length(loops), .loops = &loops[0] };
}

TB_API void tb_free_loop_info(TB_LoopInfo l) {
    dyn_array_destroy(l.loops);
}
