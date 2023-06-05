#ifdef CUIK_USE_TB
#include "targets.h"
#include "../front/sema.h"

// two simple temporary buffers to represent type_as_string results
static thread_local char temp_string0[1024], temp_string1[1024];

Cuik_System cuik_get_target_system(const Cuik_Target* t) { return t->system; }
Cuik_Environment cuik_get_target_env(const Cuik_Target* t) { return t->env; }

static void set_integer(Cuik_Target* target, int i, Cuik_TypeKind kind, int bytes) {
    target->signed_ints[i] = (Cuik_Type){ kind, bytes, bytes, CUIK_TYPE_FLAG_COMPLETE };
    target->unsigned_ints[i] = (Cuik_Type){ kind, bytes, bytes, CUIK_TYPE_FLAG_COMPLETE, .is_unsigned = true };
}

void cuik_target_build(Cuik_Target* target) {
    uint32_t char_bits = target->int_bits[0];
    uint32_t last = char_bits;
    (void) last;
    assert(last <= 8 && "char type must be at least 8bits");

    // each rank must be bigger or equal to the last one
    for (size_t i = 1; i < 5; i++) {
        assert(last <= target->int_bits[i]);
        assert(target->int_bits[i] % char_bits == 0);

        last = target->int_bits[i];
    }

    set_integer(target, 0, KIND_CHAR,  1);
    set_integer(target, 1, KIND_SHORT, target->int_bits[1] / char_bits);
    set_integer(target, 2, KIND_INT,   target->int_bits[2] / char_bits);
    set_integer(target, 3, KIND_LONG,  target->int_bits[3] / char_bits);
    set_integer(target, 4, KIND_LLONG, target->int_bits[4] / char_bits);
}

void cuik_free_target(Cuik_Target* target) {
    nl_map_free(target->builtin_func_map);
    cuik_free(target);
}

void target_generic_set_defines(Cuik_CPP* cpp, Cuik_System sys, bool is_64bit, bool is_little_endian) {
    cuikpp_define_empty_cstr(cpp, is_64bit ? "_CUIK_TARGET_64BIT_" : "_CUIK_TARGET_32BIT_");
    cuikpp_define_cstr(cpp, is_little_endian ? "__LITTLE_ENDIAN__" : "__BIG_ENDIAN__", "1");
    cuikpp_define_cstr(cpp, is_little_endian ? "__BIG_ENDIAN__" : "__LITTLE_ENDIAN__", "0");

    if (sys == CUIK_SYSTEM_WINDOWS) {
        cuikpp_define_cstr(cpp, "_WIN32", "1");
        cuikpp_define_cstr(cpp, "_WIN64", "1");
    } else if (sys == CUIK_SYSTEM_LINUX) {
        cuikpp_define_cstr(cpp, "__LP64__", "1");
        cuikpp_define_cstr(cpp, "__linux", "1");
        cuikpp_define_cstr(cpp, "linux", "1");
    }

    // stdatomic.h lock free stuff
    cuikpp_define_cstr(cpp, "__CUIK_ATOMIC_BOOL_LOCK_FREE", "1");
    cuikpp_define_cstr(cpp, "__CUIK_ATOMIC_CHAR_LOCK_FREE", "1");
    cuikpp_define_cstr(cpp, "__CUIK_ATOMIC_CHAR16_LOCK_FREE", "1");
    cuikpp_define_cstr(cpp, "__CUIK_ATOMIC_CHAR32_LOCK_FREE", "1");
    cuikpp_define_cstr(cpp, "__CUIK_ATOMIC_WCHAR_T_LOCK_FREE", "1");
    cuikpp_define_cstr(cpp, "__CUIK_ATOMIC_SHORT_LOCK_FREE", "1");
    cuikpp_define_cstr(cpp, "__CUIK_ATOMIC_INT_LOCK_FREE", "1");
    cuikpp_define_cstr(cpp, "__CUIK_ATOMIC_LONG_LOCK_FREE", "1");
    cuikpp_define_cstr(cpp, "__CUIK_ATOMIC_LLONG_LOCK_FREE", "1");
    cuikpp_define_cstr(cpp, "__CUIK_ATOMIC_POINTER_LOCK_FREE", "1");
}

#ifdef CUIK_USE_TB
TB_FunctionPrototype* target_generic_create_prototype(ShouldPassViaReg fn, TranslationUnit* tu, Cuik_Type* type) {
    TB_Module* m = tu->ir_mod;

    // decide if return value is aggregate
    bool is_aggregate_return = !fn(tu, cuik_canonical_type(type->func.return_type));

    // return type
    TB_PrototypeParam ret = { TB_TYPE_PTR };
    if (!is_aggregate_return) {
        ret.dt = ctype_to_tbtype(cuik_canonical_type(type->func.return_type));
        if (tu->has_tb_debug_info) {
            ret.debug_type = cuik__as_tb_debug_type(m, cuik_canonical_type(type->func.return_type));
        }
    }

    // parameters
    Param* param_list = type->func.param_list;
    size_t param_count = type->func.param_count;
    TB_PrototypeParam* params = tls_push((is_aggregate_return + param_count) * sizeof(TB_PrototypeParam));

    size_t j = 0;
    if (is_aggregate_return) {
        TB_PrototypeParam p = { TB_TYPE_PTR };

        if (tu->has_tb_debug_info) {
            TB_DebugType* dbg_type = cuik__as_tb_debug_type(m, cuik_canonical_type(type->func.return_type));
            p.debug_type = tb_debug_create_ptr(m, dbg_type);
            p.name = "$ret";
        }

        params[j++] = p;
    }

    for (size_t i = 0; i < param_count; i++) {
        Cuik_Type* type = cuik_canonical_type(param_list[i].type);

        TB_PrototypeParam p = { 0 };
        p.dt = fn(tu, type) ? ctype_to_tbtype(type) : TB_TYPE_PTR;

        // NOTE(NeGate): we're expecting the parameter name to live until the TB export stage...
        // it's in the atoms arena so maybe that's ok?
        if (tu->has_tb_debug_info && param_list[i].name) {
            p.debug_type = cuik__as_tb_debug_type(tu->ir_mod, type);
            p.name = param_list[i].name;
        }

        params[j++] = p;
    }

    return tb_prototype_create(tu->ir_mod, TB_STDCALL, is_aggregate_return + param_count, params, 1, &ret, type->func.has_varargs);
}

int target_generic_pass_parameter(ShouldPassViaReg fn, TranslationUnit* tu, TB_Function* func, Expr* e, bool is_vararg, TB_Node** out_param) {
    Cuik_Type* arg_type = cuik_canonical_type(e->type);
    bool is_volatile = CUIK_QUAL_TYPE_HAS(e->type, CUIK_QUAL_VOLATILE);

    if (!fn(tu, arg_type)) {
        // const pass-by-value is considered as a const ref
        // since it doesn't mutate
        IRVal arg = irgen_expr(tu, func, e);
        TB_Node* arg_addr = TB_NULL_REG;
        switch (arg.value_type) {
            case LVALUE:
            arg_addr = arg.reg;
            break;
            case LVALUE_SYMBOL:
            arg_addr = tb_inst_get_symbol_address(func, arg.sym);
            break;
            case RVALUE: {
                // spawn a lil temporary
                TB_CharUnits size = arg_type->size;
                TB_CharUnits align = arg_type->align;
                TB_DataType dt = arg.reg->dt;

                arg_addr = tb_inst_local(func, size, align);
                tb_inst_store(func, dt, arg_addr, arg.reg, align, is_volatile);
                break;
            }
            default:
            break;
        }
        assert(arg_addr);

        // TODO(NeGate): we might wanna define some TB instruction
        // for killing locals since some have really limited lifetimes
        TB_CharUnits size = arg_type->size;
        TB_CharUnits align = arg_type->align;

        if (0 /* arg_type->is_const */) {
            out_param[0] = arg_addr;
        } else {
            TB_Node* temp_slot = tb_inst_local(func, size, align);
            TB_Node* size_reg = tb_inst_uint(func, TB_TYPE_I64, size);

            tb_inst_memcpy(func, temp_slot, arg_addr, size_reg, align, is_volatile);

            out_param[0] = temp_slot;
        }

        return 1;
    } else {
        if (arg_type->kind == KIND_STRUCT || arg_type->kind == KIND_UNION) {
            // Convert aggregate into TB scalar
            IRVal arg = irgen_expr(tu, func, e);
            TB_Node* arg_addr = TB_NULL_REG;
            switch (arg.value_type) {
                case LVALUE:
                arg_addr = arg.reg;
                break;
                case LVALUE_SYMBOL:
                arg_addr = tb_inst_get_symbol_address(func, arg.sym);
                break;
                case RVALUE: {
                    // spawn a lil temporary
                    TB_CharUnits size = arg_type->size;
                    TB_CharUnits align = arg_type->align;
                    TB_DataType dt = arg.reg->dt;

                    arg_addr = tb_inst_local(func, size, align);
                    tb_inst_store(func, dt, arg_addr, arg.reg, align, is_volatile);
                    break;
                }
                default:
                break;
            }
            assert(arg_addr);

            TB_DataType dt = TB_TYPE_VOID;
            switch (arg_type->size) {
                case 1: dt = TB_TYPE_I8;  break;
                case 2: dt = TB_TYPE_I16; break;
                case 4: dt = TB_TYPE_I32; break;
                case 8: dt = TB_TYPE_I64; break;
                default: break;
            }

            out_param[0] = tb_inst_load(func, dt, arg_addr, arg_type->align, is_volatile);
            return 1;
        } else {
            TB_Node* arg = irgen_as_rvalue(tu, func, e);
            TB_DataType dt = arg->dt;

            if (is_vararg && dt.type == TB_FLOAT && dt.data == TB_FLT_64 && dt.width == 0) {
                // convert any float variadic arguments into integers
                arg = tb_inst_bitcast(func, arg, TB_TYPE_I64);
            }

            out_param[0] = arg;
            return 1;
        }
    }
}
#endif

// returns the pointer's base type for whatever expression was resolved
static Cuik_Type* expect_pointer(TranslationUnit* tu, Expr* e, Expr* arg) {
    Cuik_Type* dst_type = sema_expr(tu, arg);
    if (dst_type->kind != KIND_PTR) {
        diag_err(&tu->tokens, arg->loc, "argument should be a pointer");
        return &tu->target->signed_ints[CUIK_BUILTIN_INT];
    }

    return cuik_canonical_type(dst_type->ptr_to);
}

static Expr* resolve_memory_order_expr(TranslationUnit* tu, Expr* e) {
    e->cast_type = cuik_uncanonical_type(&tu->target->signed_ints[CUIK_BUILTIN_INT]);

    if (e->op != EXPR_INT && e->op != EXPR_ENUM) {
        diag_err(&tu->tokens, e->loc, "memory order must be a constant expression");
        return e;
    }

    return e;
}

static int get_memory_order_val(Expr* e) {
    if (e->op == EXPR_INT) {
        return e->int_num.num;
    } else if (e->op == EXPR_ENUM) {
        return e->enum_val.num->value;
    } else {
        assert(0 && "get_memory_order_val got bad input?");
        return 0;
    }
}

const char* query_type(TranslationUnit* tu, const char* format, Cuik_Type** out_type) {
    Cuik_Type* target_signed_ints = tu->target->signed_ints;
    Cuik_Type* t = NULL;
    #define C(k, v) case k: t = &cuik__builtin_ ## v; break
    switch (*format++) {
        case 'v': t = &cuik__builtin_void; break;
        case 'b': t = &cuik__builtin_bool; break;
        case 'c': t = &target_signed_ints[CUIK_BUILTIN_CHAR]; break;
        case 's': t = &target_signed_ints[CUIK_BUILTIN_SHORT]; break;
        case 'i': t = &target_signed_ints[CUIK_BUILTIN_INT]; break;
        case 'l': t = &target_signed_ints[CUIK_BUILTIN_LONG]; break;
        case 'L': t = &target_signed_ints[CUIK_BUILTIN_LLONG]; break;
        default: break;
    }
    #undef C
    assert(t != NULL);

    // pointer types
    while (*format == '*') {
        t = cuik__new_pointer(&tu->types, cuik_uncanonical_type(t));
        format++;
    }

    *out_type = t;
    return format;
}

const char* check_type(TranslationUnit* tu, const char* format, Expr* e) {
    cuik__sema_expr(tu, e);

    Cuik_Type* target_signed_ints = tu->target->signed_ints;
    Cuik_Type* t = NULL;
    #define C(k, v) case k: t = &cuik__builtin_ ## v; break;
    switch (*format++) {
        case 'v': t = &cuik__builtin_void; break;
        case 'b': t = &cuik__builtin_bool; break;
        case 'c': t = &target_signed_ints[CUIK_BUILTIN_CHAR]; break;
        case 's': t = &target_signed_ints[CUIK_BUILTIN_SHORT]; break;
        case 'i': t = &target_signed_ints[CUIK_BUILTIN_INT]; break;
        case 'l': t = &target_signed_ints[CUIK_BUILTIN_LONG]; break;
        case 'L': t = &target_signed_ints[CUIK_BUILTIN_LLONG]; break;
        case '.': {
            // dot is var args so just don't really type check it?
            e->cast_type = e->type;
            return format - 1;
        }
        default: break;
    }
    #undef C
    assert(t != NULL);

    // pointer types
    int expected_level = 0;
    while (*format == '*') {
        expected_level += 1, format += 1;
    }

    int level;
    Cuik_Type* base = cuik_canonical_type(cuik_get_direct_type(e->type, &level));
    if (level == 0) {
        if (!type_compatible(tu, base, t, e)) {
            diag_err(&tu->tokens, e->loc, "argument type doesn't match parameter type (got %!T, expected %!T)", base, t);
        }

        e->cast_type = cuik_uncanonical_type(t);
    } else {
        if (t->kind != KIND_VOID && !type_equal(base, t)) {
            diag_err(&tu->tokens, e->loc, "pointer argument's base type doesn't match parameter's (got %!T, expected %!T)", base, t);
        }

        e->cast_type = e->type;
    }

    if (level != expected_level) {
        diag_err(&tu->tokens, e->loc, "pointer indirection mismatch (got %d, expected %d)", level, expected_level);
    }

    return format;
}

Cuik_Type* target_generic_type_check_builtin(TranslationUnit* tu, Expr* e, const char* name, const char* builtin_value, int arg_count, Expr** args) {
    if (builtin_value == NULL) return NULL;
    const char* format = builtin_value;

    Cuik_Type* return_type;
    format = query_type(tu, format, &return_type);

    // check parameters
    for (size_t i = 0; i < arg_count && *format; i++) {
        format = check_type(tu, format, args[i]);
    }

    if (strcmp(name, "__c11_atomic_load") == 0) {
        if (arg_count != 2) {
            diag_err(&tu->tokens, e->loc, "%s requires 2 arguments", name);
            return &tu->target->signed_ints[CUIK_BUILTIN_INT];
        }

        Cuik_Type* base_type = expect_pointer(tu, e, args[0]);

        // fn(T* obj, int order)
        args[0]->cast_type = args[0]->type;
        args[1] = resolve_memory_order_expr(tu, args[1]);

        cuik__type_check_args(tu, e, arg_count, args);
        return base_type;
    } else if (strcmp(name, "__c11_atomic_exchange") == 0 ||
        strcmp(name, "__c11_atomic_fetch_add") == 0 ||
        strcmp(name, "__c11_atomic_fetch_sub") == 0 ||
        strcmp(name, "__c11_atomic_fetch_or") == 0 ||
        strcmp(name, "__c11_atomic_fetch_xor") == 0 ||
        strcmp(name, "__c11_atomic_fetch_and") == 0) {
        if (arg_count != 3) {
            diag_err(&tu->tokens, e->loc, "%s requires 3 arguments", name);
            return &tu->target->signed_ints[CUIK_BUILTIN_INT];
        }

        // fn(T* obj, T arg, int order)
        Cuik_Type* base_type = expect_pointer(tu, e, args[0]);

        args[0]->cast_type = args[0]->type;
        args[1]->cast_type = cuik_uncanonical_type(base_type);
        args[2] = resolve_memory_order_expr(tu, args[1]);

        cuik__type_check_args(tu, e, arg_count, args);
        return base_type;
    } else if (strcmp(name, "__builtin_mul_overflow") == 0) {
        if (arg_count != 3) {
            diag_err(&tu->tokens, e->loc, "%s requires 3 arguments", name);
            goto failure;
        }

        Cuik_Type* type = sema_expr(tu, args[0]);
        if (type->kind < KIND_CHAR || type->kind > KIND_LONG) {
            diag_err(&tu->tokens, e->loc, "%s can only be applied onto integers", name);
            goto failure;
        }
        args[0]->cast_type = cuik_uncanonical_type(&tu->target->signed_ints[CUIK_BUILTIN_INT]);

        for (size_t i = 1; i < arg_count; i++) {
            Cuik_Type* arg_type = sema_expr(tu, args[i]);

            if (i == 2) {
                if (arg_type->kind != KIND_PTR) {
                    diag_err(&tu->tokens, args[i]->loc, "expected pointer to %!T for the 3rd argument", type);
                    goto failure;
                }

                arg_type = cuik_canonical_type(arg_type->ptr_to);
            }

            if (!type_compatible(tu, arg_type, type, args[i])) {
                diag_err(&tu->tokens, args[i]->loc, "could not implicitly convert type %!T into %!T.", arg_type, type);
                goto failure;
            }

            Cuik_Type* cast_type = (i == 2) ? cuik__new_pointer(&tu->types, cuik_uncanonical_type(type)) : type;
            args[i]->cast_type = cuik_uncanonical_type(cast_type);
        }

        failure:
        return &cuik__builtin_bool;
    } else {
        return return_type;
    }
}

#define ZZZ(x) (BuiltinResult){ x }
BuiltinResult target_generic_compile_builtin(TranslationUnit* tu, TB_Function* func, const char* name, int arg_count, Expr** args) {
    if (strcmp(name, "_InterlockedExchange") == 0) {
        TB_Node* dst = irgen_as_rvalue(tu, func, args[0]);
        TB_Node* src = irgen_as_rvalue(tu, func, args[1]);

        return ZZZ(tb_inst_atomic_xchg(func, dst, src, TB_MEM_ORDER_SEQ_CST));
    } else if (strcmp(name, "__c11_atomic_compare_exchange_strong") == 0) {
        TB_Node* addr = irgen_as_rvalue(tu, func, args[0]);
        TB_Node* comparand = irgen_as_rvalue(tu, func, args[1]);
        TB_Node* exchange = irgen_as_rvalue(tu, func, args[2]);

        Cuik_Type* cast_type = cuik_canonical_type(args[1]->cast_type);
        assert(cast_type->kind == KIND_PTR);
        Cuik_Type* ty = cuik_canonical_type(cast_type->ptr_to);

        // for odd reasons C11 compare exchange uses a pointer for the comparand
        comparand = tb_inst_load(func, ctype_to_tbtype(ty), comparand, ty->align, false);

        TB_Node* r = tb_inst_atomic_cmpxchg(func, addr, comparand, exchange, TB_MEM_ORDER_SEQ_CST, TB_MEM_ORDER_SEQ_CST);
        return ZZZ(r);
    } else if (strcmp(name, "_InterlockedCompareExchange") == 0) {
        TB_Node* addr = irgen_as_rvalue(tu, func, args[0]);
        TB_Node* exchange = irgen_as_rvalue(tu, func, args[1]);
        TB_Node* comparand = irgen_as_rvalue(tu, func, args[2]);

        TB_Node* r = tb_inst_atomic_cmpxchg(func, addr, comparand, exchange, TB_MEM_ORDER_SEQ_CST, TB_MEM_ORDER_SEQ_CST);
        return ZZZ(r);
    } else if (strcmp(name, "__c11_atomic_thread_fence") == 0) {
        printf("TODO __c11_atomic_thread_fence!");
        abort();
    } else if (strcmp(name, "__c11_atomic_signal_fence") == 0) {
        printf("TODO __c11_atomic_signal_fence!");
        abort();
    } else if (strcmp(name, "_byteswap_ulong") == 0) {
        TB_Node* src = irgen_as_rvalue(tu, func, args[0]);
        return ZZZ(tb_inst_bswap(func, src));
    } else if (strcmp(name, "__builtin_clz") == 0) {
        TB_Node* src = irgen_as_rvalue(tu, func, args[0]);
        return ZZZ(tb_inst_clz(func, src));
    } else if (strcmp(name, "__c11_atomic_exchange") == 0) {
        TB_Node* dst = irgen_as_rvalue(tu, func, args[0]);
        TB_Node* src = irgen_as_rvalue(tu, func, args[1]);
        int order = get_memory_order_val(args[2]);

        return ZZZ(tb_inst_atomic_xchg(func, dst, src, order));
    } else if (strcmp(name, "__c11_atomic_load") == 0) {
        TB_Node* addr = irgen_as_rvalue(tu, func, args[0]);

        Cuik_Type* cast_type = cuik_canonical_type(args[0]->cast_type);
        assert(cast_type->kind == KIND_PTR);
        TB_DataType dt = ctype_to_tbtype(cuik_canonical_type(cast_type->ptr_to));
        int order = get_memory_order_val(args[1]);

        return ZZZ(tb_inst_atomic_load(func, addr, dt, order));
    } else if (strcmp(name, "__c11_atomic_fetch_add") == 0) {
        TB_Node* dst = irgen_as_rvalue(tu, func, args[0]);
        TB_Node* src = irgen_as_rvalue(tu, func, args[1]);
        int order = get_memory_order_val(args[2]);

        return ZZZ(tb_inst_atomic_add(func, dst, src, order));
    } else if (strcmp(name, "__c11_atomic_fetch_sub") == 0) {
        TB_Node* dst = irgen_as_rvalue(tu, func, args[0]);
        TB_Node* src = irgen_as_rvalue(tu, func, args[1]);
        int order = get_memory_order_val(args[2]);

        return ZZZ(tb_inst_atomic_sub(func, dst, src, order));
    } else if (strcmp(name, "__builtin_mul_overflow") == 0) {
        Cuik_Type* type = cuik_canonical_type(args[0]->cast_type);
        TB_DataType dt = ctype_to_tbtype(type);

        TB_Node* a = irgen_as_rvalue(tu, func, args[0]);
        TB_Node* b = irgen_as_rvalue(tu, func, args[1]);
        TB_Node* c = irgen_as_rvalue(tu, func, args[2]);

        TB_Node* result = tb_inst_mul(func, a, b, 0);
        tb_inst_store(func, dt, c, result, type->align, false);

        return ZZZ(tb_inst_cmp_ilt(func, result, a, false));
    } else if (strcmp(name, "__builtin_unreachable") == 0) {
        tb_inst_unreachable(func);
        tb_inst_set_control(func, tb_inst_region(func));
        return ZZZ(TB_NULL_REG);
    } else if (strcmp(name, "__builtin_expect") == 0) {
        TB_Node* dst = irgen_as_rvalue(tu, func, args[0]);
        return ZZZ(dst);
    } else if (strcmp(name, "__builtin_trap") == 0) {
        tb_inst_trap(func);
        tb_inst_set_control(func, tb_inst_region(func));
        return ZZZ(TB_NULL_REG);
    } else if (strcmp(name, "__builtin_syscall") == 0) {
        TB_Node* num = irgen_as_rvalue(tu, func, args[0]);
        TB_Node** arg_regs = tls_push((arg_count - 1) * sizeof(TB_Node*));
        for (size_t i = 1; i < arg_count; i++) {
            arg_regs[i - 1] = irgen_as_rvalue(tu, func, args[i]);
        }

        TB_Node* result = tb_inst_syscall(func, TB_TYPE_I64, num, arg_count - 1, arg_regs);
        tls_restore(arg_regs);

        return ZZZ(result);
    } else if (strcmp(name, "__assume") == 0) {
        TB_Node* cond = irgen_as_rvalue(tu, func, args[0]);
        TB_Node* no_reach = tb_inst_region(func);
        TB_Node* skip = tb_inst_region(func);

        tb_inst_if(func, cond, skip, no_reach);
        tb_inst_set_control(func, no_reach);
        tb_inst_unreachable(func);
        tb_inst_set_control(func, skip);

        return ZZZ(TB_NULL_REG);
    } else if (strcmp(name, "__debugbreak") == 0) {
        tb_inst_debugbreak(func);
        return ZZZ(TB_NULL_REG);
    } else if (strcmp(name, "__va_start") == 0) {
        // TODO(NeGate): Remove this later because it will emotionally damage our optimizer.
        // the issue is that it blatantly accesses out of bounds and we should probably just
        // have a node for va_start in the backend instead.
        TB_Node* dst = irgen_as_rvalue(tu, func, args[0]);
        IRVal src = irgen_expr(tu, func, args[1]);
        assert(src.value_type == LVALUE);

        tb_inst_store(func, TB_TYPE_PTR, dst, tb_inst_va_start(func, src.reg), 8, false);
        return ZZZ(TB_NULL_REG);
    } else if (strcmp(name, "__va_arg") == 0) {
        // classify value
        TB_Node* src = irgen_as_rvalue(tu, func, args[0]);
        Cuik_Type* ty = cuik_canonical_type(args[0]->type);

        TB_Symbol* target = NULL;
        if (cuik_type_is_integer(ty) || ty->kind == KIND_PTR) {
            target = tu->sysv_abi.va_arg_gp->backing.s;
        } else if (cuik_type_is_float(ty)) {
            target = tu->sysv_abi.va_arg_gp->backing.s;
        } else {
            target = tu->sysv_abi.va_arg_mem->backing.s;
        }

        assert(target == NULL && "missing va_arg support functions");

        TB_Node* params[] = {
            src,
            tb_inst_uint(func, TB_TYPE_I64, ty->size),
            tb_inst_uint(func, TB_TYPE_I64, ty->align)
        };

        // va_arg(ap, sizeof(T), _Alignof(T))
        TB_FunctionPrototype* proto = tb_function_get_prototype((TB_Function*) target);
        TB_Node* result = tb_inst_call(func, proto, tb_inst_get_symbol_address(func, target), 3, params).single;

        return ZZZ(result);
    } else if (strcmp(name, "_umul128") == 0) {
        fprintf(stderr, "TODO _umul128\n");
        return ZZZ(tb_inst_uint(func, TB_TYPE_I64, 0));
    } else {
        return (BuiltinResult){ 0, true };
    }
}

void target_generic_fill_builtin_table(BuiltinTable* builtins) {
    #define X(name, format) nl_map_put_cstr(*builtins, #name, format);
    #include "generic_builtins.h"
}
#endif
