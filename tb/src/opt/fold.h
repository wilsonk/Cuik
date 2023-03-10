
#define MASK_UPTO(pos) (~UINT64_C(0) >> (64 - pos))
#define BEXTR(src,pos) (((src) >> (pos)) & 1)
uint64_t tb__sxt(uint64_t src, uint64_t src_bits, uint64_t dst_bits) {
    uint64_t sign_bit = BEXTR(src, src_bits-1);
    uint64_t mask = MASK_UPTO(dst_bits) & ~MASK_UPTO(src_bits);

    uint64_t dst = src & ~mask;
    return dst | (sign_bit ? mask : 0);
}

static bool single_word_compare_fold(TB_NodeTypeEnum node_type, TB_DataType dt, uint64_t ai, uint64_t bi) {
    uint64_t diff;
    bool overflow = tb_sub_overflow(ai, bi, &diff);
    bool sign = diff & (1u << (dt.data - 1));

    switch (node_type) {
        case TB_CMP_EQ:  return (diff == 0);
        case TB_CMP_NE:  return (diff != 0);
        case TB_CMP_SLT: return (sign != overflow);
        case TB_CMP_SLE: return (diff == 0) || (sign != overflow);
        case TB_CMP_ULT: return (overflow);
        case TB_CMP_ULE: return (diff == 0) || overflow;
        default: tb_unreachable(); return false;
    }
}

typedef struct {
    uint64_t result;
    bool poison;
} ArithResult;

static ArithResult single_word_arith_fold(TB_NodeTypeEnum node_type, TB_DataType dt, uint64_t ai, uint64_t bi, TB_ArithmaticBehavior ab) {
    uint64_t shift = 64-dt.data;
    uint64_t mask = ~UINT64_C(0) >> shift;

    switch (node_type) {
        case TB_AND: return (ArithResult){ ai & bi };
        case TB_XOR: return (ArithResult){ ai ^ bi };
        case TB_OR:  return (ArithResult){ ai | bi };
        case TB_ADD: {
            uint64_t result;
            bool ovr = tb_add_overflow(ai << shift, bi << shift, &result);

            if ((ab & TB_ARITHMATIC_NUW) && ovr) {
                return (ArithResult){ 0, true };
            } else {
                return (ArithResult){ (result >> shift) & mask };
            }
            break;
        }
        case TB_SUB: {
            uint64_t result;
            bool ovr = tb_sub_overflow(ai << shift, bi << shift, &result);

            if ((ab & TB_ARITHMATIC_NUW) && ovr) {
                return (ArithResult){ 0, true };
            } else {
                return (ArithResult){ (result >> shift) & mask };
            }
        }
        case TB_MUL: {
            TB_MultiplyResult res = tb_mul64x128(ai, bi);

            if ((ab & TB_ARITHMATIC_NUW) && (res.hi || res.lo & ~mask)) {
                return (ArithResult){ 0, true };
            } else if ((ab & TB_ARITHMATIC_NSW) && res.hi != res.lo >> 63) {
                return (ArithResult){ 0, true };
            } else {
                return (ArithResult){ res.lo & mask };
            }
        }
        case TB_UDIV:
        case TB_SDIV: {
            if (bi == 0) {
                return (ArithResult){ 0, true };
            }

            if (node_type == TB_SDIV) {
                return (ArithResult){ ((int64_t)ai / (int64_t)bi) & mask };
            } else {
                return (ArithResult){ (ai / bi) & mask };
            }
        }
        case TB_SHL: {
            return (ArithResult){ (ai << bi) };
        }
        case TB_SHR: {
            return (ArithResult){ (ai >> bi) };
        }
        case TB_SAR: {
            tb_assert_once("Idk if this works");

            bool sign_bit = BEXTR(ai, dt.data - 1);
            uint64_t mask = (~UINT64_C(0) >> (64 - dt.data)) << dt.data;

            return (ArithResult){ (ai >> bi) | (sign_bit ? mask : 0) };
        }
        default: tb_todo();
    }
}

static bool is_associative(TB_NodeTypeEnum type) {
    switch (type) {
        case TB_ADD: case TB_MUL:
        case TB_AND: case TB_XOR: case TB_OR:
        return true;

        default:
        return false;
    }
}

static bool is_commutative(TB_NodeTypeEnum type) {
    switch (type) {
        case TB_ADD: case TB_MUL:
        case TB_AND: case TB_XOR: case TB_OR:
        case TB_CMP_NE: case TB_CMP_EQ:
        return true;

        default:
        return false;
    }
}

static bool const_fold(TB_Function* f, TB_Label bb, TB_Node* n) {
    TB_DataType dt = n->dt;

    switch (n->type) {
        ////////////////////////////////
        // Unary operator folding
        ////////////////////////////////
        // This is merely true
        //   -x => ~x + 1
        case TB_NEG: {
            #if 0
            TB_Node* src = n->inputs[0];

            if (src->type == TB_INTEGER_CONST) {
                assert(src->dt.type == TB_INT && src->dt.data > 0);
                TB_NodeInt* src_i = TB_NODE_GET_EXTRA(src);

                uint64_t* words = tb_transmute_to_int(n, src_i->num_words);
                BigInt_copy(src->num_words, words, src_i->words);
                BigInt_not(src->num_words, words);
                BigInt_inc(src->num_words, words);
                return true;
            }
            #endif

            break;
        }

        case TB_NOT: {
            TB_Node* src = n->inputs[0];

            if (src->type == TB_INTEGER_CONST) {
                assert(src->dt.type == TB_INT && src->dt.data > 0);
                TB_NodeInt* src_i = TB_NODE_GET_EXTRA(src);

                uint64_t* words = tb_transmute_to_int(f, bb, n, src_i->num_words);
                BigInt_copy(src_i->num_words, words, src_i->words);
                BigInt_not(src_i->num_words, words);
                return true;
            }

            break;
        }

        case TB_ZERO_EXT:
        case TB_SIGN_EXT: {
            TB_Node* src = n->inputs[0];
            if (src->type == TB_INTEGER_CONST) {
                TB_NodeInt* src_i = TB_NODE_GET_EXTRA(src);

                size_t src_num_words = src_i->num_words;
                size_t dst_num_words = (n->dt.data + (BigIntWordSize*8) - 1) / (BigIntWordSize*8);
                bool is_signed = false;
                if (n->type == TB_SIGN_EXT) {
                    is_signed = BigInt_bextr(src_i->num_words, src_i->words, src->dt.data-1);
                }

                uint64_t* words = tb_transmute_to_int(f, bb, n, dst_num_words);
                BigInt_copy(src_i->num_words, words, src_i->words);

                FOREACH_N(i, src_i->num_words, dst_num_words) {
                    words[i] = is_signed ? ~UINT64_C(0) : 0;
                }

                // fixup the bits here
                uint64_t shift = (64 - (src->dt.data % 64));
                uint64_t mask = (~UINT64_C(0) >> shift) << shift;

                if (is_signed) words[src_num_words - 1] |= mask;
                else words[src_num_words - 1] &= ~mask;
                return true;
            }

            break;
        }

        case TB_TRUNCATE: {
            TB_Node* src = n->inputs[0];
            if (src->type == TB_INTEGER_CONST) {
                TB_NodeInt* src_i = TB_NODE_GET_EXTRA(src);

                size_t dst_num_words = (n->dt.data + (BigIntWordSize*8) - 1) / (BigIntWordSize*8);
                uint64_t* words = tb_transmute_to_int(f, bb, n, dst_num_words);
                BigInt_copy(dst_num_words, words, src_i->words);

                // fixup the bits here
                uint64_t shift = (64 - (dt.data % 64)), mask = (~UINT64_C(0) >> shift) << shift;
                words[dst_num_words-1] &= ~mask;
                return true;
            }

            break;
        }

        ////////////////////////////////
        // Binary operator folding
        ////////////////////////////////
        case TB_AND:
        case TB_OR:
        case TB_XOR:
        case TB_ADD:
        case TB_SUB:
        case TB_MUL:
        case TB_SHL:
        case TB_SHR:
        case TB_SAR:
        case TB_UDIV:
        case TB_SDIV:
        case TB_UMOD:
        case TB_SMOD:
        case TB_CMP_EQ:
        case TB_CMP_NE:
        case TB_CMP_SLT:
        case TB_CMP_SLE:
        case TB_CMP_ULT:
        case TB_CMP_ULE:
        case TB_CMP_FLT:
        case TB_CMP_FLE: {
            // if it's commutative: move constants to the right
            if (is_commutative(n->type) && n->inputs[0]->type == TB_INTEGER_CONST && n->inputs[1]->type != TB_INTEGER_CONST) {
                tb_swap(TB_Node*, n->inputs[0], n->inputs[1]);
            }

            TB_Node* a = n->inputs[0];
            TB_Node* b = n->inputs[1];

            if (n->dt.type == TB_FLOAT && n->dt.data == TB_FLT_32 && a->type == TB_FLOAT32_CONST && b->type == TB_FLOAT32_CONST) {
                // comparisons
                float af = TB_NODE_GET_EXTRA_T(a, TB_NodeFloat32)->value;
                float bf = TB_NODE_GET_EXTRA_T(b, TB_NodeFloat32)->value;

                if (n->type >= TB_CMP_EQ && n->type <= TB_CMP_FLE) {
                    bool result = false;
                    switch (n->type) {
                        case TB_CMP_EQ: result  = (af == bf); break;
                        case TB_CMP_NE: result  = (af != bf); break;
                        case TB_CMP_FLT: result = (af <  bf); break;
                        case TB_CMP_FLE: result = (af <= bf); break;
                        default: tb_todo();
                    }

                    uint64_t* words = tb_transmute_to_int(f, bb, n, 1);
                    words[0] = result;
                    return true;
                } else if (n->type >= TB_FADD && n->type <= TB_FDIV) {
                    tb_todo();
                    /* float result = 0.0f;
                    switch (n->type) {
                        case TB_FADD: result = (af + bf); break;
                        case TB_FSUB: result = (af - bf); break;
                        case TB_FMUL: result = (af * bf); break;
                        case TB_FDIV: result = (af / bf); break;
                        default: tb_todo();
                    }

                    n->type = TB_FLOAT32_CONST;
                    n->dt = TB_TYPE_F32;
                    n->flt32.value = result; */
                    return true;
                }
            } else if (n->dt.type == TB_INT && b->type == TB_INTEGER_CONST) {
                if (a->type == TB_INTEGER_CONST && n->type >= TB_AND && n->type <= TB_MUL) {
                    // fully fold
                    TB_NodeInt* ai = TB_NODE_GET_EXTRA(a);
                    TB_NodeInt* bi = TB_NODE_GET_EXTRA(b);
                    assert(ai->num_words == bi->num_words);

                    BigInt_t *a_words = ai->words, *b_words = bi->words;

                    size_t num_words = ai->num_words;
                    BigInt_t* words = tb_transmute_to_int(f, bb, n, ai->num_words);
                    switch (n->type) {
                        case TB_AND: BigInt_and(num_words, a_words, b_words, words); break;
                        case TB_OR:  BigInt_or(num_words, a_words, b_words, words); break;
                        case TB_XOR: BigInt_xor(num_words, a_words, b_words, words); break;
                        case TB_ADD: BigInt_add(num_words, a_words, num_words, b_words, num_words, words); break;
                        case TB_SUB: BigInt_sub(num_words, a_words, num_words, b_words, num_words, words); break;
                        case TB_MUL: BigInt_mul_basic(num_words, a_words, b_words, words); break;
                        default: goto fail;
                    }

                    // fixup the bits here
                    uint64_t shift = (64 - (n->dt.data % 64)), mask = (~UINT64_C(0) >> shift) << shift;
                    words[num_words-1] &= ~mask;

                    // if we fail, delete our allocation, we probably should avoid failing in the future
                    fail:;
                } else {
                    // partial binary operations e.g.
                    //   a * 0 = 0
                    //   a + 0 = a
                    TB_NodeInt* bi = TB_NODE_GET_EXTRA(b);

                    if (BigInt_is_zero(bi->num_words, bi->words)) {
                        switch (n->type) {
                            case TB_ADD: case TB_SUB:
                            case TB_XOR: case TB_OR:
                            case TB_SHL: case TB_SHR:
                            case TB_SAR:
                            tb_transmute_to_pass(n, a);
                            return true;

                            case TB_MUL: case TB_AND:
                            uint64_t* words = tb_transmute_to_int(f, bb, n, 1);
                            words[0] = 0;
                            return true;

                            case TB_SDIV: case TB_UDIV:
                            tb_transmute_to_poison(n);
                            return true;

                            default: break;
                        }
                    } else if (BigInt_is_small_num(bi->num_words, bi->words, 1)) {
                        switch (n->type) {
                            case TB_MUL: case TB_SDIV: case TB_UDIV:
                            tb_transmute_to_pass(n, a);
                            return true;

                            default: break;
                        }
                    }

                    if (bi->num_words == 1) {
                        if (n->type == TB_MUL) {
                            // (a * b) => (a << log2(b)) where b is a power of two
                            uint64_t log2 = tb_ffs(bi->words[0]) - 1;
                            if (bi->words[0] == (UINT64_C(1) << log2)) {
                                OPTIMIZER_LOG(n, "converted power-of-two multiply into left shift");

                                // It's a power of two, swap in a left-shift
                                // tb_todo();
                                /* TB_Node* new_op = tb_alloc_node(f, TB_INTEGER_CONST, dt, 0, sizeof(TB_NodeInt) + sizeof(uint64_t));
                                TB_NODE_GET_EXTRA_T(new_op, TB_NodeInt)->words[0] = log2;

                                f->nodes[new_op].type = TB_INTEGER_CONST;
                                f->nodes[new_op].dt = dt;
                                f->nodes[new_op].integer.num_words = 1;
                                f->nodes[new_op].integer.single_word = log2;

                                n->type = TB_SHL;
                                n->dt = dt;
                                n->i_arith = (struct TB_NodeIArith) { .a = a - f->nodes, .b = new_op };
                                return true; */
                            }
                        } else if (n->type == TB_UMOD || n->type == TB_SMOD) {
                            // (mod a N) => (and a N-1) where N is a power of two
                            uint64_t mask = bi->words[0];
                            if (tb_is_power_of_two(mask)) {
                                OPTIMIZER_LOG(n, "converted modulo into AND with constant mask");

                                tb_todo();
                                // generate mask
                                /* TB_Reg extra_reg = tb_function_insert_after(f, bb, ar);
                                TB_Node* extra = &f->nodes[extra_reg];
                                extra->type = TB_INTEGER_CONST;
                                extra->dt = n->dt;
                                extra->integer.num_words = 1;
                                extra->integer.single_word = mask - 1;

                                // new AND operation to replace old MOD
                                n = &f->nodes[r];
                                n->type = TB_AND;
                                n->i_arith.b = extra_reg;
                                return true; */
                            }
                        }
                    }
                }
            }
            break;
        }

        default:
        break;
    }

    // didn't change shit :(
    return false;
}
