#include <cuik.h>
#include "targets/targets.h"

// hacky
void hook_crash_handler(void);
void init_timer_system(void);

thread_local TB_Arena thread_arena;

void cuik_init(bool use_crash_handler) {
    cuik_init_terminal();
    init_timer_system();

    if (use_crash_handler) {
        hook_crash_handler();
    }
}

void cuik_free_thread_resources(void) {
    atoms_free();
    tb_arena_destroy(&thread_arena);
}

Cuik_Target* cuik_target_host(void) {
    #if defined(_WIN32)
    return cuik_target_x64(CUIK_SYSTEM_WINDOWS, CUIK_ENV_MSVC);
    #elif defined(__linux) || defined(linux)
    return cuik_target_x64(CUIK_SYSTEM_LINUX, CUIK_ENV_GNU);
    #elif defined(__APPLE__) || defined(__MACH__) || defined(macintosh)
    return cuik_target_x64(CUIK_SYSTEM_MACOS, CUIK_ENV_GNU);
    #endif
}

void cuik_set_standard_defines(Cuik_CPP* cpp, const Cuik_DriverArgs* args) {
    // DO NOT REMOVE THESE, IF THEY'RE MISSING THE PREPROCESSOR WILL NOT DETECT THEM
    cuikpp_define_empty_cstr(cpp, "__FILE__");
    cuikpp_define_empty_cstr(cpp, "L__FILE__");
    cuikpp_define_empty_cstr(cpp, "__LINE__");
    cuikpp_define_empty_cstr(cpp, "__COUNTER__");

    // CuikC specific
    cuikpp_define_cstr(cpp, "__CUIK__", STR(CUIK_COMPILER_MAJOR));
    cuikpp_define_cstr(cpp, "__CUIK_MINOR__", STR(CUIK_COMPILER_MINOR));

    // C23/Cuik bool being available without stdbool.h
    cuikpp_define_empty_cstr(cpp, "__bool_true_false_are_defined");
    cuikpp_define_cstr(cpp, "bool", "_Bool");
    cuikpp_define_cstr(cpp, "false", "0");
    cuikpp_define_cstr(cpp, "true", "1");

    // GNU C
    cuikpp_define_cstr(cpp, "__BYTE_ORDER__", "1");
    cuikpp_define_cstr(cpp, "__ORDER_LITTLE_ENDIAN", "1");
    cuikpp_define_cstr(cpp, "__ORDER_BIG_ENDIAN", "2");

    // Standard C macros
    cuikpp_define_cstr(cpp, "__STDC__", "1");
    cuikpp_define_cstr(cpp, "__STDC_VERSION__", "201112L"); // C11

    // currently there's no freestanding mode but if there was this would be
    // turned off for it
    bool freestanding = false;

    cuikpp_define_cstr(cpp, "__STDC_HOSTED__", freestanding ? "0" : "1");
    cuikpp_define_cstr(cpp, "__STDC_NO_COMPLEX__", "1");
    cuikpp_define_cstr(cpp, "__STDC_NO_VLA__", "1");
    cuikpp_define_cstr(cpp, "__STDC_NO_THREADS__", "1");

    // The time of translation of the preprocessing translation unit
    static const char mon_name[][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    time_t rawtime;
    time(&rawtime);

    struct tm* timeinfo = localtime(&rawtime);

    // Mmm dd yyyy
    char date_str[20];
    snprintf(date_str, 20, "\"%.3s%3d %d\"", mon_name[timeinfo->tm_mon], timeinfo->tm_mday, 1900 + timeinfo->tm_year);
    cuikpp_define_cstr(cpp, "__DATE__", date_str);

    // The time of translation of the preprocessing translation unit: a
    // character string literal of the form "hh:mm:ss" as in the time
    // generated by the asctime function. If the time of translation is
    // not available, an implementation-defined valid time shall be supplied.
    char time_str[20];
    snprintf(time_str, 20, "\"%.2d:%.2d:%.2d\"", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    cuikpp_define_cstr(cpp, "__TIME__", time_str);

    cuikpp_define_cstr(cpp, "static_assert", "_Static_assert");
    cuikpp_define_cstr(cpp, "typeof", "_Typeof");

    cuikpp_add_include_directory(cpp, true, "$cuik/");
    args->toolchain.set_preprocessor(args->toolchain.ctx, args->nocrt, cpp);

    if (args->target != NULL) {
        args->target->set_defines(args->target, cpp);
    }
}

Cuik_Entrypoint cuik_get_entrypoint_status(TranslationUnit* restrict tu) {
    return tu->entrypoint_status;
}

TokenStream* cuik_get_token_stream_from_tu(TranslationUnit* restrict tu) {
    return &tu->tokens;
}

void cuik_print_type(Cuik_Type* restrict type) {
    char str[1024];
    type_as_string(1024, str, type);
    printf("%s", str);
}

#ifndef _WIN32
// non-windows platforms generally just don't have the safe functions so
// let's provide them
int sprintf_s(char* buffer, size_t len, const char* format, ...) {
    if (buffer == NULL || len == 0) return -1;

    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, len, format, args);
    va_end(args);

    if (result < 0 && result >= len) {
        fprintf(stderr, "error: buffer overflow on sprintf_s!\n");
        abort();
    }

    return result;
}
#endif
