// TODO(NeGate): Refactor this code...
//
// NOTE(NeGate): This code leaks the filename strings but it doesn't actually matter
// because this is a compiler and momma aint raised no bitch.
//
// NOTE(NeGate): the_shtuffs is the simple linear allocator in this preprocessor, just avoids
// wasting time on the heap allocator
#include <cuik.h>
#include <assert.h>
#include "../str.h"
#include "../diagnostic.h"

#include "lexer.h"
#include <setjmp.h>
#include <sys/stat.h>
#include <futex.h>

#if USE_INTRIN
#include <x86intrin.h>
#endif

typedef struct TokenNode TokenNode;
struct TokenNode {
    TokenNode* next;
    Token t;
};

typedef struct {
    const uint8_t* start;
    TokenNode *head, *tail;
} TokenList;

// GOD I HATE FORWARD DECLARATIONS
static bool is_defined(Cuik_CPP* restrict c, const unsigned char* start, size_t length);
static void expect(TokenArray* restrict in, char ch);
static intmax_t eval(Cuik_CPP* restrict c, TokenArray* restrict in);

static void* gimme_the_shtuffs(Cuik_CPP* restrict c, size_t len);
static void* gimme_the_shtuffs_fill(Cuik_CPP* restrict c, const char* str);
static void trim_the_shtuffs(Cuik_CPP* restrict c, void* new_top);

static bool push_scope(Cuik_CPP* restrict ctx, TokenArray* restrict in, bool initial);
static bool pop_scope(Cuik_CPP* restrict ctx, TokenArray* restrict in);

static void print_token_stream(TokenArray* in, size_t start, size_t end);

static TokenNode* expand(Cuik_CPP* restrict c, TokenNode* restrict head, uint32_t parent_macro, TokenArray* rest);
static TokenList expand_ident(Cuik_CPP* restrict c, TokenArray* in, TokenNode* head, uint32_t parent_macro, TokenArray* rest);
static bool locate_file(Cuik_CPP* ctx, bool search_lib_first, const char* dir, const char* og_path, char canonical[FILENAME_MAX], bool* is_system);

static char* alloc_directory_path(const char* filepath);
static void compute_line_map(TokenStream* s, bool is_system, int depth, SourceLoc include_site, const char* filename, char* data, size_t length);

#define MAX_CPP_STACK_DEPTH 1024

typedef struct CPPStackSlot {
    const char* filepath;
    const char* directory;

    uint32_t file_id;
    SourceLoc loc; // location of the #include
    TokenArray tokens;

    // https://gcc.gnu.org/onlinedocs/cppinternals/Guard-Macros.html
    struct CPPIncludeGuard {
        enum {
            INCLUDE_GUARD_INVALID = -1,

            INCLUDE_GUARD_LOOKING_FOR_IFNDEF,
            INCLUDE_GUARD_LOOKING_FOR_DEFINE,
            INCLUDE_GUARD_LOOKING_FOR_ENDIF,
            INCLUDE_GUARD_EXPECTING_NOTHING,
        } status;
        int if_depth; // the depth value we expect the include guard to be at
        String define;
    } include_guard;
} CPPStackSlot;

// just the value (doesn't track the name of the parameter)
typedef struct {
    String content;
    SourceRange loc;
} MacroArg;

typedef struct {
    int key_count;
    String* keys;

    int value_count;
    MacroArg* values;

    bool has_varargs;
} MacroArgs;

static Token peek(TokenArray* restrict in) {
    return in->tokens[in->current];
}

static bool at_token_list_end(TokenArray* restrict in) {
    return in->current >= dyn_array_length(in->tokens)-1;
}

static Token consume(TokenArray* restrict in) {
    assert(in->current < dyn_array_length(in->tokens));
    return in->tokens[in->current++];
}

static void expect_from_lexer(Cuik_CPP* c, Lexer* l, char ch) {
    Token t = lexer_read(l);
    if (t.type != ch) {
        ResolvedSourceLoc r = cuikpp_find_location(&c->tokens, t.location);

        fprintf(stderr, "error %s:%d: expected '%c' got '%.*s'", r.file->filename, r.line, ch, (int) t.content.length, t.content.data);
        abort();
    }
}

static TokenArray convert_to_token_list(Cuik_CPP* restrict c, uint32_t file_id, size_t length, char* data) {
    Lexer l = {
        .file_id = file_id,
        .start = (unsigned char*) data,
        .current = (unsigned char*) data,
    };

    TokenArray list = { 0 };
    list.tokens = dyn_array_create(Token, 32 + ((length + 7) / 8));

    for (;;) {
        Token t = lexer_read(&l);
        if (t.type == 0) break;

        dyn_array_put(list.tokens, t);
    }

    dyn_array_put(list.tokens, (Token){ 0 });
    return list;
}

static String get_token_as_string(TokenStream* restrict in) {
    return tokens_get(in)->content;
}

// include this if you want to enable the preprocessor debugger
// #include "cpp_dbg.h"

// Basically a mini-unity build that takes up just the CPP module
#include "cpp_symtab.h"
#include "cpp_expand.h"
#include "cpp_fs.h"
#include "cpp_expr.h"
#include "cpp_directive.h"
#include "cpp_iters.h"

const char* cuikpp_get_main_file(TokenStream* tokens) {
    return tokens->filepath;
}

bool cuikpp_is_in_main_file(TokenStream* tokens, SourceLoc loc) {
    // TODO(NeGate): macros can be in the main file, we should be walking the macro
    // trace to check for that
    // assert((loc.raw & SourceLoc_IsMacro) == 0 && "TODO: support macro in cuikpp_is_in_main_file");
    if (loc.raw & SourceLoc_IsMacro) {
        return false;
    }

    Cuik_File* f = &tokens->files[loc.raw >> SourceLoc_FilePosBits];
    return f->filename == tokens->filepath;
}

Cuik_CPP* cuikpp_make(const char filepath[FILENAME_MAX], Cuik_DiagCallback callback, void* userdata) {
    if (filepath == NULL) {
        filepath = "";
    }

    Cuik_CPP* ctx = cuik_malloc(sizeof(Cuik_CPP));
    *ctx = (Cuik_CPP){
        .stack = cuik__valloc(MAX_CPP_STACK_DEPTH * sizeof(CPPStackSlot)),
        .macros = {
            .exp = 24,
            .keys = cuik__valloc((1u << 24) * sizeof(String)),
            .vals = cuik__valloc((1u << 24) * sizeof(MacroDef)),
        },
        .the_shtuffs = cuik__valloc(THE_SHTUFFS_SIZE),
    };

    // initialize dynamic arrays
    ctx->system_include_dirs = dyn_array_create(char*, 64);
    ctx->stack_ptr = 1;

    char* slash = strrchr(filepath, '\\');
    if (!slash) slash = strrchr(filepath, '/');

    char* directory = gimme_the_shtuffs(ctx, FILENAME_MAX);
    if (slash) {
        #if _WIN32
        sprintf_s(directory, FILENAME_MAX, "%.*s\\", (int)(slash - filepath), filepath);
        #else
        snprintf(directory, FILENAME_MAX, "%.*s/", (int)(slash - filepath), filepath);
        #endif
    } else {
        directory[0] = '\0';
    }

    ctx->tokens.diag = cuikdg_make(callback, userdata);
    ctx->tokens.filepath = filepath;
    ctx->tokens.list.tokens = dyn_array_create(Token, 4096);
    ctx->tokens.invokes = dyn_array_create(MacroInvoke, 4096);
    ctx->tokens.files = dyn_array_create(Cuik_File, 256);

    // MacroID 0 is a null invocation
    dyn_array_put(ctx->tokens.invokes, (MacroInvoke){ 0 });

    // FileID 0 is the builtin macro file or the NULL file depending on who you ask
    dyn_array_put(ctx->tokens.files, (Cuik_File){ .filename = "<builtin>", .content_length = (1u << SourceLoc_FilePosBits) - 1u });
    tls_init();

    ctx->state1 = CUIK__CPP_FIRST_FILE;
    ctx->stack[0] = (CPPStackSlot){
        .filepath = filepath,
        .directory = directory,
    };
    return ctx;
}

CUIK_API Cuik_File* cuikpp_get_files(TokenStream* restrict s) {
    return &s->files[1];
}

CUIK_API size_t cuikpp_get_file_count(TokenStream* restrict s) {
    return dyn_array_length(s->files) - 1;
}

Token* cuikpp_get_tokens(TokenStream* restrict s) {
    return &s->list.tokens[0];
}

size_t cuikpp_get_token_count(TokenStream* restrict s) {
    // don't tell them about the EOF token :P
    return dyn_array_length(s->list.tokens) - 1;
}

void cuiklex_free_tokens(TokenStream* tokens) {
    dyn_array_for(i, tokens->files) {
        // only free the root line_map, all the others are offsets of this one
        if (tokens->files[i].file_pos_bias == 0) {
            dyn_array_destroy(tokens->files[i].line_map);
        }
    }

    dyn_array_destroy(tokens->files);
    dyn_array_destroy(tokens->list.tokens);
    dyn_array_destroy(tokens->invokes);
    cuikdg_free(tokens->diag);
}

void cuikpp_finalize(Cuik_CPP* ctx) {
    #if CUIK__CPP_STATS
    fprintf(stderr, " %80s | %.06f ms read+lex\t| %4zu files read\t| %zu fstats\t| %f ms (%zu defines)\n",
        ctx->tokens.filepath,
        ctx->total_io_time / 1000000.0,
        ctx->total_files_read,
        ctx->total_fstats,
        ctx->total_define_access_time / 1000000.0,
        ctx->total_define_accesses
    );
    #endif

    CUIK_TIMED_BLOCK("cuikpp_finalize") {
        cuik__vfree(ctx->macros.keys, (1u << ctx->macros.exp) * sizeof(String));
        cuik__vfree(ctx->macros.vals, (1u << ctx->macros.exp) * sizeof(MacroDef));
        cuik__vfree(ctx->stack, MAX_CPP_STACK_DEPTH * sizeof(CPPStackSlot));

        ctx->macros.keys = NULL;
        ctx->macros.vals = NULL;
        ctx->stack = NULL;
    }

    nl_strmap_free(ctx->include_once);
}

void cuikpp_free(Cuik_CPP* ctx) {
    dyn_array_for(i, ctx->system_include_dirs) {
        cuik_free(ctx->system_include_dirs[i].name);
    }
    dyn_array_destroy(ctx->system_include_dirs);

    if (ctx->macros.keys) {
        cuikpp_finalize(ctx);
    }

    cuik__vfree((void*) ctx->the_shtuffs, THE_SHTUFFS_SIZE);
    cuik_free(ctx);
}

// we can infer the column and line from doing a binary search on the TokenStream's line map
static ResolvedSourceLoc find_location(Cuik_File* file, uint32_t file_pos) {
    if (file->line_map == NULL) {
        return (ResolvedSourceLoc){
            .file = file,
            .line_str = "",
            .line = 1, .column = file_pos
        };
    }

    file_pos += file->file_pos_bias;

    size_t left = 0;
    size_t right = dyn_array_length(file->line_map);
    while (left < right) {
        size_t middle = (left + right) / 2;
        if (file->line_map[middle] > file_pos) {
            right = middle;
        } else {
            left = middle + 1;
        }
    }

    uint32_t l = file->line_map[right - 1];
    assert(file_pos >= l);

    // NOTE(NeGate): it's possible that l is lesser than file->file_pos_bias in the
    // line_str calculation, this is fine (it happens when the line starts in the last
    // chunk and crosses the boundary) we just need to do the math with ptrdiff_t
    // such that if it goes negative it'll refer to the previous chunk of the content
    return (ResolvedSourceLoc){
        .file = file,
        .line_str = &file->content[(ptrdiff_t)l - (ptrdiff_t)file->file_pos_bias],
        .line = right, .column = file_pos - l
    };
}

MacroInvoke* cuikpp_find_macro(TokenStream* tokens, SourceLoc loc) {
    if ((loc.raw & SourceLoc_IsMacro) == 0) {
        return NULL;
    }

    uint32_t macro_id = (loc.raw >> SourceLoc_MacroOffsetBits) & ((1u << SourceLoc_MacroIDBits) - 1);
    return &tokens->invokes[macro_id];
}

Cuik_File* cuikpp_find_file(TokenStream* tokens, SourceLoc loc) {
    while (loc.raw & SourceLoc_IsMacro) {
        uint32_t macro_id = (loc.raw >> SourceLoc_MacroOffsetBits) & ((1u << SourceLoc_MacroIDBits) - 1);
        loc = tokens->invokes[macro_id].call_site;
    }

    return &tokens->files[loc.raw >> SourceLoc_FilePosBits];
}

Cuik_FileLoc cuikpp_find_location_in_bytes(TokenStream* tokens, SourceLoc loc) {
    assert((loc.raw & SourceLoc_IsMacro) == 0);
    assert((loc.raw >> SourceLoc_FilePosBits) < dyn_array_length(tokens->files));
    Cuik_File* f = &tokens->files[loc.raw >> SourceLoc_FilePosBits];
    uint32_t pos = loc.raw & ((1u << SourceLoc_FilePosBits) - 1);

    return (Cuik_FileLoc){ f, pos };

    /*if ((loc.raw & SourceLoc_IsMacro) == 0) {
        assert((loc.raw >> SourceLoc_FilePosBits) < dyn_array_length(tokens->files));
        Cuik_File* f = &tokens->files[loc.raw >> SourceLoc_FilePosBits];
        uint32_t pos = loc.raw & ((1u << SourceLoc_FilePosBits) - 1);

        return (Cuik_FileLoc){ f, pos };
    } else {
        uint32_t macro_id = (loc.raw & ((1u << SourceLoc_MacroIDBits) - 1)) >> SourceLoc_MacroOffsetBits;
        uint32_t macro_off = loc.raw & ((1u << SourceLoc_MacroOffsetBits) - 1);

        Cuik_FileLoc fl = cuikpp_find_location_in_bytes(tokens, tokens->invokes[macro_id].def_site.start);
        return (Cuik_FileLoc){ fl.file, fl.pos + macro_off };
    }*/
}

SourceLoc cuikpp_get_physical_location(TokenStream* tokens, SourceLoc loc) {
    MacroInvoke* m;
    while ((m = cuikpp_find_macro(tokens, loc)) != NULL) { loc = m->call_site; }
    return loc;
}

ResolvedSourceLoc cuikpp_find_location2(TokenStream* tokens, Cuik_FileLoc loc) {
    return find_location(loc.file, loc.pos);
}

ResolvedSourceLoc cuikpp_find_location(TokenStream* tokens, SourceLoc loc) {
    MacroInvoke* m;
    while ((m = cuikpp_find_macro(tokens, loc)) != NULL) {
        loc = m->call_site;
    }

    Cuik_FileLoc fl = cuikpp_find_location_in_bytes(tokens, loc);
    return find_location(fl.file, fl.pos);
}

static void compute_line_map(TokenStream* s, bool is_system, int depth, SourceLoc include_site, const char* filename, char* data, size_t length) {
    DynArray(uint32_t) line_map = dyn_array_create(uint32_t, (length / 20) + 32);

    #if 1
    // !USE_INTRIN
    dyn_array_put(line_map, 0);

    for (size_t i = 0; i < length;) {
        while (i < length && data[i] != '\n') i += 1;

        i += 1;
        dyn_array_put(line_map, i);
    }
    #else
    // TODO(NeGate): make non-x64 SIMD variants
    for (size_t i = 0; i < length; i += 16) {
        while (i < length) {
            __m128i bytes = _mm_load_si128((__m128i*) &data[i]);
            unsigned int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(bytes, _mm_set1_epi8('\n')));
            int len = __builtin_ffs(~mask));

            if (len) {
                i += len;
                break;
            } else {
                i += 16;
            }
        }

        dyn_array_put(line_map, i);
    }
    #endif

    // files bigger than the SourceLoc_FilePosBits allows will be fit into multiple sequencial files
    size_t i = 0, single_file_limit = (1u << SourceLoc_FilePosBits);
    do {
        size_t chunk_end = i + single_file_limit;
        if (chunk_end > length) chunk_end = length;

        dyn_array_put(s->files, (Cuik_File){ filename, is_system, depth, include_site, i, chunk_end - i, &data[i], line_map });
        i += single_file_limit;
    } while (i < length);
}

#if 0
static CPPTask* alloc_cpp_task(Cuik_CPP* ctx) {
    uint64_t old, free_bit;
    do {
        old = ctx->task_busy;

        // find empty slot we can reserve
        free_bit = old != 0 ? tb_ffs64(~old) - 1 : 0;

        // don't continue until we successfully commit
    } while (!atomic_compare_exchange_strong(&threadpool->queue, &old, old | (1ull << free_bit)));

    return free_bit;
}
#endif

void cuikpp_task_done(CPPTask* restrict t) {
    __debugbreak();
}

static bool locate_file(Cuik_CPP* ctx, bool search_lib_first, const char* dir, const char* og_path, char canonical[FILENAME_MAX], bool* is_system) {
    char path[FILENAME_MAX];
    if (!search_lib_first) {
        sprintf_s(path, FILENAME_MAX, "%s%s", dir, og_path);
        if (ctx->locate(ctx->user_data, path, canonical)) {
            *is_system = false;
            return true;
        }
    }

    dyn_array_for(i, ctx->system_include_dirs) {
        sprintf_s(path, FILENAME_MAX, "%s%s", ctx->system_include_dirs[i].name, og_path);

        if (ctx->locate(ctx->user_data, path, canonical)) {
            *is_system = ctx->system_include_dirs[i].is_system;
            return true;
        }
    }

    if (search_lib_first) {
        sprintf_s(path, FILENAME_MAX, "%s%s", dir, og_path);
        if (ctx->locate(ctx->user_data, path, canonical)) {
            *is_system = false;
            return true;
        }
    }

    return false;
}

static char* alloc_directory_path(const char* filepath) {
    // identify directory path
    char* slash = strrchr(filepath, '/');
    if (!slash) slash = strrchr(filepath, '\\');

    char* new_dir = NULL;
    if (slash) {
        size_t slash_pos = slash - filepath;

        char* new_dir = arena_alloc(&thread_arena, slash_pos + 2, 1);
        memcpy(new_dir, filepath, slash_pos);
        new_dir[slash_pos] = '/';
        new_dir[slash_pos + 1] = 0;
        return new_dir;
    } else {
        char* new_dir = arena_alloc(&thread_arena, 2, 1);
        new_dir[0] = '/';
        new_dir[1] = 0;
        return new_dir;
    }
}

Cuikpp_Status cuikpp_run(Cuik_CPP* ctx, Cuikpp_LocateFile* locate, Cuikpp_GetFile* fs, void* user_data) {
    assert(ctx->stack_ptr > 0);
    ctx->user_data = user_data;
    ctx->fs = fs;
    ctx->locate = locate;

    CPPStackSlot* restrict slot = &ctx->stack[ctx->stack_ptr - 1];

    ////////////////////////////////
    // first file doesn't need to check include paths
    ////////////////////////////////
    slot->include_guard = (struct CPPIncludeGuard){ 0 };

    #if CUIK__CPP_STATS
    uint64_t start_time = cuik_time_in_nanos();
    #endif

    Cuik_FileResult main_file;
    if (!fs(user_data, slot->filepath, &main_file)) {
        fprintf(stderr, "\x1b[31merror\x1b[0m: file \"%s\" doesn't exist.\n", slot->filepath);
        return CUIKPP_ERROR;
    }

    #if CUIK__CPP_STATS
    ctx->total_io_time += (cuik_time_in_nanos() - start_time);
    ctx->total_files_read += 1;
    #endif

    // initialize the lexer in the stack slot & record the file entry
    slot->file_id = dyn_array_length(ctx->tokens.files);
    slot->tokens = convert_to_token_list(ctx, dyn_array_length(ctx->tokens.files), main_file.length, main_file.data);
    compute_line_map(&ctx->tokens, false, 0, (SourceLoc){ 0 }, slot->filepath, main_file.data, main_file.length);

    // continue along to the actual preprocessing now
    #ifdef CPP_DBG
    cppdbg__break();
    #endif /* CPP_DBG */

    if (cuikperf_is_active()) {
        cuikperf_region_start(cuik_time_in_nanos(), "preprocess", slot->filepath);
    }

    TokenStream* restrict s = &ctx->tokens;
    for (;;) yield: {
        slot = &ctx->stack[ctx->stack_ptr - 1];

        TokenArray* restrict in = &slot->tokens;
        for (;;) {
            // Hot code, just copying tokens over
            Token first;
            for (;;) {
                if (at_token_list_end(in)) goto pop_stack;

                if (slot->include_guard.status == INCLUDE_GUARD_EXPECTING_NOTHING) {
                    slot->include_guard.status = INCLUDE_GUARD_INVALID;
                }

                first = consume(in);
                if (first.type == TOKEN_IDENTIFIER) {
                    // check if it's actually a macro, if not categorize it if it's a keyword
                    if (!is_defined(ctx, first.content.data, first.content.length)) {
                        // FAST PATH
                        first.type = classify_ident(first.content.data, first.content.length);
                        dyn_array_put(s->list.tokens, first);
                    } else {
                        in->current -= 1;

                        // SLOW PATH BECAUSE IT NEEDS TO SPAWN POSSIBLY METRIC SHIT LOADS
                        // OF TOKENS AND EXPAND WITH THE AVERAGE C PREPROCESSOR SPOOKIES
                        if (expand_builtin_idents(ctx, &first)) {
                            dyn_array_put(s->list.tokens, first);
                        } else {
                            void* savepoint = tls_save();

                            TokenList l = expand_ident(ctx, in, NULL, 0, NULL);
                            for (TokenNode* n = l.head; n != l.tail; n = n->next) {
                                Token* restrict t = &n->t;
                                if (t->type == 0) {
                                    continue;
                                } else if (t->type == TOKEN_IDENTIFIER) {
                                    t->type = classify_ident(t->content.data, t->content.length);
                                }

                                dyn_array_put(s->list.tokens, *t);
                            }

                            tls_restore(savepoint);
                        }
                    }
                } else if (first.type == TOKEN_HASH) {
                    // slow path
                    break;
                } else {
                    dyn_array_put(s->list.tokens, first);
                }
            }

            // Slow code, defines
            DirectiveResult result = DIRECTIVE_UNKNOWN;
            String directive = consume(in).content;

            // shorthand for calling the directives in cpp_directive.h
            #define MATCH(str)                                         \
            if (memcmp(directive.data, #str, sizeof(#str) - 1) == 0) { \
                result = cpp__ ## str(ctx, slot, in);                  \
                break;                                                 \
            }

            // all the directives go here
            switch (directive.length) {
                case 2:
                MATCH(if);
                break;

                case 4:
                MATCH(elif);
                MATCH(else);
                break;

                case 5:
                MATCH(undef);
                MATCH(error);
                MATCH(ifdef);
                MATCH(endif);
                MATCH(embed);
                break;

                case 6:
                MATCH(define);
                MATCH(pragma);
                MATCH(ifndef);
                break;

                case 7:
                MATCH(include);
                MATCH(warning);
                MATCH(version);
                break;

                case 9:
                MATCH(extension);
                break;
            }
            #undef MATCH

            if (result == DIRECTIVE_YIELD) {
                goto yield;
            } else if (result == DIRECTIVE_ERROR) {
                return CUIKPP_ERROR;
            } else if (result == DIRECTIVE_UNKNOWN) {
                SourceRange r = { first.location, get_end_location(&in->tokens[in->current - 1]) };
                diag_err(s, r, "unknown directive %_S", directive);
                return CUIKPP_ERROR;
            }
        }

        // this is called when we're done with a specific file
        pop_stack:
        ctx->stack_ptr -= 1;

        if (slot->include_guard.status == INCLUDE_GUARD_EXPECTING_NOTHING) {
            // the file is practically pragma once
            nl_strmap_put_cstr(ctx->include_once, (const char*) slot->filepath, 0);
        }

        // write out profile entry
        if (cuikperf_is_active()) {
            cuikperf_region_end();
        }

        // free the token stream
        dyn_array_destroy(in->tokens);

        // if this is the last file, just exit
        if (ctx->stack_ptr == 0) {
            // place last token
            dyn_array_put(s->list.tokens, (Token){ 0 });

            s->list.current = 0;
            return CUIKPP_DONE;
        }
    }
}

TokenStream* cuikpp_get_token_stream(Cuik_CPP* ctx) {
    return &ctx->tokens;
}

static void print_token_stream(TokenArray* in, size_t start, size_t end) {
    int last_line = 0;

    printf("Tokens: ");
    for (size_t i = start; i < end; i++) {
        Token* t = &in->tokens[i];
        printf("%.*s ", (int) t->content.length, t->content.data);
    }
    printf("\n\n");
}

void cuikpp_dump_defines(Cuik_CPP* ctx) {
    int count = 0;

    size_t cap = 1u << ctx->macros.exp;
    for (size_t i = 0; i < cap; i++) {
        if (ctx->macros.keys[i].length != 0 && ctx->macros.keys[i].length != MACRO_DEF_TOMBSTONE) {
            String key = ctx->macros.keys[i];
            String val = ctx->macros.vals[i].value;

            printf("  #define %.*s %.*s\n", (int)key.length, key.data, (int)val.length, val.data);
        }
    }

    printf("\n// Macro defines active: %d\n", count);
}

static void* gimme_the_shtuffs(Cuik_CPP* restrict c, size_t len) {
    unsigned char* allocation = c->the_shtuffs + c->the_shtuffs_size;

    c->the_shtuffs_size += len;
    if (c->the_shtuffs_size >= THE_SHTUFFS_SIZE) {
        printf("Preprocessor: out of memory!\n");
        abort();
    }

    return allocation;
}

static void* gimme_the_shtuffs_fill(Cuik_CPP* restrict c, const char* str) {
    size_t len = strlen(str) + 1;
    unsigned char* allocation = c->the_shtuffs + c->the_shtuffs_size;

    c->the_shtuffs_size += len;
    if (c->the_shtuffs_size >= THE_SHTUFFS_SIZE) {
        printf("Preprocessor: out of memory!\n");
        abort();
    }

    memcpy(allocation, str, len);
    return allocation;
}

static void trim_the_shtuffs(Cuik_CPP* restrict c, void* new_top) {
    size_t i = ((uint8_t*)new_top) - c->the_shtuffs;
    assert(i <= c->the_shtuffs_size);
    c->the_shtuffs_size = i;
}

static bool push_scope(Cuik_CPP* restrict ctx, TokenArray* restrict in, bool initial) {
    if (ctx->depth >= CPP_MAX_SCOPE_DEPTH - 1) {
        diag_err(&ctx->tokens, get_token_range(&in->tokens[in->current - 1]), "too many #ifs");
        return false;
    }

    ctx->scope_eval[ctx->depth++] = (struct Cuikpp_ScopeEval){ in->tokens[in->current - 1].location, initial };
    return true;
}

static bool pop_scope(Cuik_CPP* restrict ctx, TokenArray* restrict in) {
    if (ctx->depth == 0) {
        diag_err(&ctx->tokens, get_token_range(&in->tokens[in->current - 1]), "too many #endif");
        diag_note(&ctx->tokens, (SourceRange){ ctx->scope_eval[0].start, ctx->scope_eval[0].start }, "expected for:");
        return false;
    }

    ctx->depth--;
    return true;
}

static void expect(TokenArray* restrict in, char ch) {
    Token t = consume(in);
    if (t.type != ch) {
        // report(REPORT_ERROR, NULL, in, tokens_get_location_index(in), "expected '%c' got '%.*s'", ch, (int) t.content.length, t.content.data);
        abort();
    }
}
