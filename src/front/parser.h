#pragma once
#include "arena.h"
#include "atoms.h"
#include "big_array.h"
#include "common.h"
#include "diagnostic.h"
#include "memory.h"
#include "settings.h"
#include <cuik.h>
#include <preproc/lexer.h>
#include <ext/threadpool.h>
#include <ext/threads.h>

#include <tb.h>

// the Cuik AST types are all exposed to the public interface
#include <cuik_ast.h>

#define MAX_LOCAL_SYMBOLS (1 << 20)
#define MAX_LOCAL_TAGS (1 << 16)

typedef struct ConstValue {
    bool is_signed;
    union {
        intmax_t signed_value;
        uintmax_t unsigned_value;
    };
} ConstValue;

typedef struct Decl {
    Type* type;
    Atom name;
    SourceLocIndex loc;
} Decl;

typedef enum StorageClass {
    STORAGE_NONE,

    STORAGE_STATIC_FUNC,  // .text
    STORAGE_STATIC_VAR,   // .data
    STORAGE_STATIC_CONST, // .rdata

    STORAGE_FUNC,
    STORAGE_PARAM,
    STORAGE_GLOBAL,
    STORAGE_LOCAL,
    STORAGE_ENUM,
    STORAGE_TYPEDEF
} StorageClass;

typedef struct Symbol {
    Atom name;
    Type* type;
    SourceLocIndex loc;
    StorageClass storage_class;

    union {
        // used if storage_class == STORAGE_PARAM
        int param_num;

        // used if storage_class == STORAGE_ENUM, refers to an index in the entries table
        int enum_value;

        Stmt* stmt;
    };

    // when handling global symbols and delaying their parsing
    // we want to be able to store what the position of the symbol's
    // "value" aka function bodies and intitial expressions
    int current;
    int terminator;
} Symbol;

typedef struct {
    Atom key;
    Stmt* value;
} ExportedSymbolEntry;

struct TranslationUnit {
    // circular references amirite...
    struct CompilationUnit* parent;

    TB_Module* ir_mod;
    const char* filepath;

    // chain of TUs for the compilation unit
    struct TranslationUnit* next;

    atomic_int id_gen;
    bool is_free;

    // token stream
    TokenStream tokens;

    mtx_t arena_mutex;
    Arena ast_arena;
    Arena type_arena;

    // stb_ds array
    // NOTE(NeGate): should this be an stb_ds array?
    Stmt** top_level_stmts;
};

// builtin types at the start of the type table
enum {
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_CHAR,
    TYPE_SHORT,
    TYPE_INT,
    TYPE_LONG,
    TYPE_UCHAR,
    TYPE_USHORT,
    TYPE_UINT,
    TYPE_ULONG,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    BUILTIN_TYPE_COUNT,
};
extern Type builtin_types[BUILTIN_TYPE_COUNT];

Type* new_func(TranslationUnit* tu);
Type* new_enum(TranslationUnit* tu);
Type* new_blank_type(TranslationUnit* tu);
Type* new_record(TranslationUnit* tu, bool is_union);
Type* copy_type(TranslationUnit* tu, Type* base);
Type* new_pointer(TranslationUnit* tu, Type* base);
Type* new_typeof(TranslationUnit* tu, Expr* src);
Type* new_array(TranslationUnit* tu, Type* base, int count);
Type* new_vector(TranslationUnit* tu, Type* base, int count);
Type* get_common_type(TranslationUnit* tu, Type* ty1, Type* ty2);
bool type_equal(TranslationUnit* tu, Type* a, Type* b);
size_t type_as_string(TranslationUnit* tu, size_t max_len, char* buffer, Type* type_index);

void type_layout(TranslationUnit* restrict tu, Type* type);

Stmt* resolve_unknown_symbol(TranslationUnit* tu, Expr* e);
ConstValue const_eval(TranslationUnit* tu, const Expr* e);
bool const_eval_try_offsetof_hack(TranslationUnit* tu, const Expr* e, uint64_t* out);

// if thread_pool is NULL, the semantics are done single threaded
void sema_pass(TranslationUnit* tu, threadpool_t* thread_pool);
