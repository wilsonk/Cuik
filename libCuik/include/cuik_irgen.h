////////////////////////////////////////////
// TB IR generation
////////////////////////////////////////////
// This is the necessary code to integrate TB with Cuik, it'll generate IR for top level
// statements.
//
// Notes:
//   * cuikcg_function and cuikcg_decl can be run on multiple threads
//     for the same module so long as the AST node is different.
//
//   * the generator functions will return NULL if the AST node is incompatible.
//
//   * unused AST nodes do not NEED be compiled.
//
#pragma once
#include "cuik_prelude.h"
#include <tb.h>

// allocates all the functions and globals necessary to do IRgen.
void cuikcg_allocate_ir(CompilationUnit* restrict cu, Cuik_IThreadpool* restrict thread_pool, TB_Module* m);

// returns NULL on failure
TB_Symbol* cuikcg_top_level(TranslationUnit* restrict tu, TB_Module* m, Stmt* restrict s);
