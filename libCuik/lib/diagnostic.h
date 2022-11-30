#pragma once
#include "common.h"
#include "preproc/lexer.h"
#include <cuik.h>
#include <threads.h>
#undef ERROR

#define REPORT(lvl, loc, ...) report(REPORT_##lvl, &tu->tokens, (SourceLoc){ 0 }, __VA_ARGS__)
#define REPORT_RANGED(lvl, a, b, ...) report_ranged(REPORT_##lvl, &tu->tokens, a, b, __VA_ARGS__)
#define REPORT_EXPR(lvl, e, ...) report_ranged(REPORT_##lvl, &tu->tokens, (e)->loc.start, (e)->loc.end, __VA_ARGS__)
#define REPORT_STMT(lvl, s, ...) report_ranged(REPORT_##lvl, &tu->tokens, (s)->loc.start, (s)->loc.end, __VA_ARGS__)

extern mtx_t report_mutex;
extern bool report_using_thin_errors;

typedef struct DiagFixit {
    SourceRange loc;
    int offset;
    const char* hint;
} DiagFixit;

// These are your options for arguments in diagnostics
typedef enum {
    DIAG_NOTE,
    DIAG_WARN,
    DIAG_ERR,
} DiagType;

typedef struct {
    TokenStream* tokens;
    ResolvedSourceLoc base;

    const char* line_start;
    const char* line_end;

    size_t dist_from_line_start;
    size_t cursor;
} DiagWriter;

typedef struct {
    TokenStream* tokens;
} Diagnostics;

typedef enum Cuik_ReportLevel {
    REPORT_VERBOSE,
    REPORT_INFO,
    REPORT_WARNING,
    REPORT_ERROR,
    REPORT_MAX
} Cuik_ReportLevel;

void cuikdg_init(void);
int cuikdg_error_count(TokenStream* s);
void cuikdg_tally_error(TokenStream* s);

////////////////////////////////
// Complex diagnostic builder
////////////////////////////////
void diag_header(DiagType type, const char* fmt, ...);

DiagWriter diag_writer(TokenStream* tokens);
void diag_writer_highlight(DiagWriter* writer, SourceRange loc);
bool diag_writer_is_compatible(DiagWriter* writer, SourceRange loc);
void diag_writer_done(DiagWriter* writer);

static void report_two_spots(Cuik_ReportLevel level, TokenStream* tokens, SourceLoc loc, SourceLoc loc2, const char* msg, const char* loc_msg, const char* loc_msg2, const char* interjection) {}
static void report(Cuik_ReportLevel level, TokenStream* tokens, SourceLoc loc, const char* fmt, ...) {}
static void report_ranged(Cuik_ReportLevel level, TokenStream* tokens, SourceLoc start_loc, SourceLoc end_loc, const char* fmt, ...) {}
static void report_fix(Cuik_ReportLevel level, TokenStream* tokens, SourceLoc loc, const char* tip, const char* fmt, ...) {}

// Report primitives
static void report_header(Cuik_ReportLevel level, const char* fmt, ...) {}
static void report_line(TokenStream* tokens, SourceLoc loci, int indent) {}

#if 0
// loc_msg      |
// loc_msg2     |> are all nullable
// interjection |
void report_two_spots(Cuik_ReportLevel level, Cuik_ErrorStatus* err, TokenStream* tokens, SourceLoc loc, SourceLoc loc2, const char* msg, const char* loc_msg, const char* loc_msg2, const char* interjection);
void report(Cuik_ReportLevel level, Cuik_ErrorStatus* err, TokenStream* tokens, SourceLoc loc, const char* fmt, ...);
void report_ranged(Cuik_ReportLevel level, Cuik_ErrorStatus* err, TokenStream* tokens, SourceLoc start_loc, SourceLoc end_loc, const char* fmt, ...);
void report_fix(Cuik_ReportLevel level, Cuik_ErrorStatus* err, TokenStream* tokens, SourceLoc loc, const char* tip, const char* fmt, ...);

// Report primitives
void report_header(Cuik_ReportLevel level, const char* fmt, ...);
void report_line(TokenStream* tokens, SourceLoc loci, int indent);

bool has_reports(Cuik_ReportLevel min, Cuik_ErrorStatus* err);
#endif