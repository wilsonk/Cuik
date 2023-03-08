#include "tb_internal.h"

TB_Exports tb_coff_write_output(TB_Module* restrict m, const IDebugFormat* dbg);
TB_Exports tb_macho_write_output(TB_Module* restrict m, const IDebugFormat* dbg);
TB_Exports tb_elf64obj_write_output(TB_Module* restrict m, const IDebugFormat* dbg);
TB_Exports tb_wasm_write_output(TB_Module* restrict m, const IDebugFormat* dbg);

static const IDebugFormat* find_debug_format(TB_DebugFormat debug_fmt) {
    switch (debug_fmt) {
        //case TB_DEBUGFMT_DWARF: return &tb__dwarf_debug_format;
        case TB_DEBUGFMT_CODEVIEW: return &tb__codeview_debug_format;
        default: return NULL;
    }
}

TB_API TB_Exports tb_module_object_export(TB_Module* m, TB_DebugFormat debug_fmt){
    typedef TB_Exports ExporterFn(TB_Module* restrict m, const IDebugFormat* dbg);

    // map target systems to exporters (maybe we wanna decouple this later)
    static ExporterFn* const fn[TB_SYSTEM_MAX] = {
        [TB_SYSTEM_WINDOWS] = tb_coff_write_output,
        [TB_SYSTEM_MACOS]   = tb_macho_write_output,
        [TB_SYSTEM_LINUX]   = tb_elf64obj_write_output,
        [TB_SYSTEM_WEB]     = tb_wasm_write_output,
    };

    assert(fn[m->target_system] != NULL && "TODO");
    TB_Exports e;
    CUIK_TIMED_BLOCK("export") {
        e = fn[m->target_system](m, find_debug_format(debug_fmt));
    }
    return e;
}

TB_API void tb_exporter_free(TB_Exports exports) {
    FOREACH_N(i, 0, exports.count) {
        tb_platform_heap_free(exports.files[i].data);
    }
}

static void layout_section(TB_ModuleSection* restrict section) {
    if (section->laid_out) {
        return;
    }

    CUIK_TIMED_BLOCK_ARGS("layout section", section->name) {
        uint64_t offset = 0;
        dyn_array_for(i, section->globals) {
            TB_Global* g = section->globals[i];

            g->pos = offset;
            offset = align_up(offset + g->size, g->align);
        }
        section->total_size = offset;
        section->laid_out = true;
    }
}

static int compare_symbols(const void* a, const void* b) {
    const TB_Symbol* sym_a = *(const TB_Symbol**) a;
    const TB_Symbol* sym_b = *(const TB_Symbol**) b;

    return sym_a->ordinal - sym_b->ordinal;
}

TB_API void tb_module_layout_sections(TB_Module* m) {
    // text section is special because it holds code
    TB_Symbol** array_form = NULL;
    FOREACH_N(tag, 0, TB_SYMBOL_MAX) {
        if (m->symbol_count[tag] < 1) continue;

        CUIK_TIMED_BLOCK("sort") {
            array_form = tb_platform_heap_realloc(array_form, m->symbol_count[tag] * sizeof(TB_Symbol*));

            size_t i = 0;
            CUIK_TIMED_BLOCK("convert to array") {
                for (TB_Symbol* s = m->first_symbol_of_tag[tag]; s != NULL; s = s->next) {
                    array_form[i++] = s;
                }
                assert(i == m->symbol_count[tag]);
            }

            CUIK_TIMED_BLOCK("sort by ordinal") {
                qsort(array_form, i, sizeof(TB_Symbol*), compare_symbols);
            }

            CUIK_TIMED_BLOCK("convert back to list") {
                m->first_symbol_of_tag[tag] = array_form[0];
                m->last_symbol_of_tag[tag] = array_form[i - 1];

                FOREACH_N(j, 1, i) {
                    array_form[j-1]->next = array_form[j];
                }
                array_form[i-1]->next = NULL;
            }
        }
    }
    tb_platform_heap_free(array_form);

    CUIK_TIMED_BLOCK("layout code") {
        size_t offset = 0;
        TB_FOR_FUNCTIONS(f, m) {
            TB_FunctionOutput* out_f = f->output;
            if (out_f != NULL) {
                out_f->code_pos = offset;
                offset += out_f->code_size;
            }
        }

        m->text.total_size = offset;
        m->text.laid_out = true;
    }

    layout_section(&m->data);
    layout_section(&m->rdata);
    layout_section(&m->tls);
}

size_t tb_helper_write_section(TB_Module* m, size_t write_pos, TB_ModuleSection* section, uint8_t* output, uint32_t pos) {
    assert(write_pos == pos);
    uint8_t* data = &output[pos];

    switch (section->kind) {
        case TB_MODULE_SECTION_TEXT:
        TB_FOR_FUNCTIONS(f, m) {
            TB_FunctionOutput* out_f = f->output;

            if (out_f != NULL) {
                memcpy(data + out_f->code_pos, out_f->code, out_f->code_size);
            }
        }
        break;

        case TB_MODULE_SECTION_DATA:
        case TB_MODULE_SECTION_TLS:
        dyn_array_for(i, section->globals) {
            TB_Global* restrict g = section->globals[i];

            memset(&data[g->pos], 0, g->size);
            FOREACH_N(k, 0, g->obj_count) {
                if (g->objects[k].type == TB_INIT_OBJ_REGION) {
                    memcpy(&data[g->pos + g->objects[k].offset], g->objects[k].region.ptr, g->objects[k].region.size);
                }
            }
        }
        break;

        default:
        tb_todo();
        break;
    }

    return write_pos + section->total_size;
}
