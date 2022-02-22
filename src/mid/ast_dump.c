// It's only *sorta* part of the semantics pass
#include "sema.h"

// two simple temporary buffers to represent type_as_string results
static _Thread_local char temp_string0[256], temp_string1[256];
static _Thread_local StmtIndex function_stmt;

static void dump_expr(TranslationUnit* tu, FILE* stream, ExprIndex e, int depth) {
	for (int i = 0; i < depth; i++) printf("  ");
	
	Expr* restrict ep = &tu->exprs[e];
	switch (ep->op) {
		case EXPR_INT: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "IntegerLiteral %llu '%s'\n", ep->int_num.num, temp_string0);
			break;
		}
		case EXPR_FLOAT32:
		case EXPR_FLOAT64: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "FloatLiteral %f '%s'\n", ep->float_num, temp_string0);
			break;
		}
		case EXPR_SYMBOL: {
			StmtIndex stmt = ep->symbol;
			
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			if (tu->stmts[stmt].op == STMT_LABEL) {
				fprintf(stream, "LabelRef\n");
			} else {
				fprintf(stream, "Symbol %s '%s'\n", tu->stmts[stmt].decl.name, temp_string0);
			}
			break;
		}
		case EXPR_PARAM: {
			int param_num = ep->param_num;
			
			Type* func_type = &tu->types[tu->stmts[function_stmt].decl.type];
			Param* params = &tu->params[func_type->func.param_list];
			
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "Symbol %s '%s'\n", params[param_num].name, temp_string0);
			break;
		}
		case EXPR_STR: {
			// TODO(NeGate): Convert the string back into a C string literal so we don't cause any weird text printing
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "StringLiteral \"...\" '%s'\n", temp_string0);
			break;
		}
		case EXPR_CALL: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "FunctionCall '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->call.target, depth + 1);
			
			ExprIndex* args = ep->call.param_start;
			int arg_count = ep->call.param_count;
			
			for (size_t i = 0; i < arg_count; i++) {
				dump_expr(tu, stream, args[i], depth + 1);
			}
			break;
		}
		case EXPR_TERNARY: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "Ternary '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->ternary_op.left, depth + 1);
			dump_expr(tu, stream, ep->ternary_op.middle, depth + 1);
			dump_expr(tu, stream, ep->ternary_op.right, depth + 1);
			break;
		}
		case EXPR_ARROW: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "Arrow %s '%s'\n", ep->arrow.name, temp_string0);
			
			dump_expr(tu, stream, ep->arrow.base, depth + 1);
			break;
		}
		case EXPR_DOT: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "Dot %s '%s'\n", ep->dot.name, temp_string0);
			
			dump_expr(tu, stream, ep->dot.base, depth + 1);
			break;
		}
		case EXPR_SUBSCRIPT: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "Subscript '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->subscript.base, depth + 1);
			dump_expr(tu, stream, ep->subscript.index, depth + 1);
			break;
		}
		case EXPR_DEREF: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "Deref '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->unary_op.src, depth + 1);
			break;
		}
		case EXPR_ADDR: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "Addr '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->unary_op.src, depth + 1);
			break;
		}
		case EXPR_POST_INC: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "PostIncrement '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->unary_op.src, depth + 1);
			break;
		}
		case EXPR_POST_DEC: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "PostDecrement '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->unary_op.src, depth + 1);
			break;
		}
		case EXPR_PRE_INC: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "PreIncrement '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->unary_op.src, depth + 1);
			break;
		}
		case EXPR_PRE_DEC: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "PreDecrement '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->unary_op.src, depth + 1);
			break;
		}
		case EXPR_LOGICAL_NOT: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "LogicalNot '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->unary_op.src, depth + 1);
			break;
		}
		case EXPR_NOT: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "BinaryNot '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->unary_op.src, depth + 1);
			break;
		}
		case EXPR_NEGATE: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "Negate '%s'\n", temp_string0);
			
			dump_expr(tu, stream, ep->unary_op.src, depth + 1);
			break;
		}
		case EXPR_CAST: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			type_as_string(tu, sizeof(temp_string1), temp_string1, ep->cast.type);
			fprintf(stream, "Cast '%s' -> '%s'\n", temp_string0, temp_string1);
			
			dump_expr(tu, stream, ep->cast.src, depth + 1);
			break;
		}
		case EXPR_COMMA:
		case EXPR_PLUS:
		case EXPR_MINUS:
		case EXPR_TIMES:
		case EXPR_SLASH:
		case EXPR_PERCENT:
		case EXPR_AND:
		case EXPR_OR:
		case EXPR_XOR:
		case EXPR_SHL:
		case EXPR_SHR:
		case EXPR_PLUS_ASSIGN:
		case EXPR_MINUS_ASSIGN:
		case EXPR_ASSIGN:
		case EXPR_TIMES_ASSIGN:
		case EXPR_SLASH_ASSIGN:
		case EXPR_AND_ASSIGN:
		case EXPR_OR_ASSIGN:
		case EXPR_XOR_ASSIGN:
		case EXPR_SHL_ASSIGN:
		case EXPR_SHR_ASSIGN:
		case EXPR_CMPEQ:
		case EXPR_CMPNE:
		case EXPR_CMPGT:
		case EXPR_CMPGE:
		case EXPR_CMPLT:
		case EXPR_CMPLE:
		case EXPR_LOGICAL_AND:
		case EXPR_LOGICAL_OR:
		case EXPR_PTRADD:
		case EXPR_PTRSUB:
		case EXPR_PTRDIFF: {
			static const char* names[] = {
				[EXPR_COMMA] = "Comma",
				
				[EXPR_PLUS] = "Plus",
				[EXPR_MINUS] = "Minus",
				[EXPR_TIMES] = "Times",
				[EXPR_SLASH] = "Slash",
				[EXPR_PERCENT] = "Percent",
				[EXPR_AND] = "And",
				[EXPR_OR] = "Or",
				[EXPR_XOR] = "Xor",
				[EXPR_SHL] = "ShiftLeft",
				[EXPR_SHR] = "ShiftRight",
				
				[EXPR_PLUS_ASSIGN]  = "PlusAssign",
				[EXPR_MINUS_ASSIGN] = "MinusAssign",
				[EXPR_ASSIGN]       = "Assign",
				[EXPR_TIMES_ASSIGN] = "TimesAssign",
				[EXPR_SLASH_ASSIGN] = "SlashAssign",
				[EXPR_AND_ASSIGN]   = "AndAssign",
				[EXPR_OR_ASSIGN]    = "OrAssign",
				[EXPR_XOR_ASSIGN]   = "XorAssign",
				[EXPR_SHL_ASSIGN]   = "ShiftLeftAssign",
				[EXPR_SHR_ASSIGN]   = "ShiftRightAssign",
				
				[EXPR_CMPEQ] = "CompareEqual",
				[EXPR_CMPNE] = "CompareNotEqual",
				[EXPR_CMPGT] = "CompareGreater",
				[EXPR_CMPGE] = "CompareGreaterOrEqual",
				[EXPR_CMPLT] = "CompareLesser",
				[EXPR_CMPLE] = "CompareLesserOrEqual",
				
				[EXPR_LOGICAL_AND] = "LogicalAnd",
				[EXPR_LOGICAL_OR] = "LogicalOr",
				
				[EXPR_PTRADD] = "PointerAdd",
				[EXPR_PTRSUB] = "PointerSub",
				[EXPR_PTRDIFF] = "PointerDiff"
			};
			
			type_as_string(tu, sizeof(temp_string0), temp_string0, ep->type);
			fprintf(stream, "%s '%s'\n", names[ep->op], temp_string0);
			
			dump_expr(tu, stream, ep->bin_op.left, depth + 1);
			dump_expr(tu, stream, ep->bin_op.right, depth + 1);
			break;
		}
		default: abort();
	}
}

static void dump_stmt(TranslationUnit* tu, FILE* stream, StmtIndex s, int depth) {
	for (int i = 0; i < depth; i++) printf("  ");
	
	Stmt* restrict sp = &tu->stmts[s];
	switch (sp->op) {
		case STMT_DECL:
		case STMT_GLOBAL_DECL: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, sp->decl.type);
			fprintf(stream, "VarDecl %s '%s'\n", sp->decl.name, temp_string0);
			
			if (sp->decl.initial) {
				dump_expr(tu, stream, tu->stmts[s].decl.initial, depth + 1);
			}
			break;
		}
		case STMT_FUNC_DECL: {
			type_as_string(tu, sizeof(temp_string0), temp_string0, sp->decl.type);
			fprintf(stream, "FunctionDecl %s '%s'\n", sp->decl.name, temp_string0);
			
			StmtIndex old_function_stmt = function_stmt;
			function_stmt = s;
			
			dump_stmt(tu, stream, (StmtIndex)sp->decl.initial, depth + 1);
			
			function_stmt = old_function_stmt;
			break;
		}
		case STMT_COMPOUND: {
			fprintf(stream, "Compound\n");
			
			StmtIndex* kids = sp->compound.kids;
			size_t count = sp->compound.kids_count;
			
			for (size_t i = 0; i < count; i++) {
				dump_stmt(tu, stream, kids[i], depth + 1);
			}
			break;
		}
		case STMT_EXPR: {
			fprintf(stream, "Expr\n");
			dump_expr(tu, stream, sp->expr.expr, depth + 1);
			break;
		}
		case STMT_RETURN: {
			fprintf(stream, "Return\n");
			if (sp->return_.expr) {
				dump_expr(tu, stream, sp->return_.expr, depth + 1);
			}
			break;
		}
		case STMT_IF: {
			fprintf(stream, "If\n");
			dump_expr(tu, stream, sp->if_.cond, depth + 1);
			
			if (sp->if_.body) {
				dump_stmt(tu, stream, sp->if_.body, depth + 1);
			}
			
			if (sp->if_.next) {
				for (int i = 0; i < depth; i++) printf("  ");
				fprintf(stream, "Else\n");
				dump_stmt(tu, stream, sp->if_.next, depth + 1);
			}
			break;
		}
		case STMT_FOR: {
			fprintf(stream, "For\n");
			if (sp->for_.first) {
				for (int i = 0; i < depth; i++) printf("  ");
				fprintf(stream, "  Init:\n");
				
				if (tu->stmts[sp->for_.first].op == STMT_COMPOUND) {
					// @shadow
					Stmt* restrict sp_first = &tu->stmts[sp->for_.first];
					
					StmtIndex* kids = sp_first->compound.kids;
					size_t count = sp_first->compound.kids_count;
					
					for (size_t i = 0; i < count; i++) {
						dump_stmt(tu, stream, kids[i], depth + 2);
					}
				} else {
					dump_stmt(tu, stream, sp->for_.first, depth + 2);
				}
			}
			
			if (sp->for_.cond) {
				for (int i = 0; i < depth; i++) printf("  ");
				fprintf(stream, "  Cond:\n");
				dump_expr(tu, stream, sp->for_.cond, depth + 2);
			}
			
			for (int i = 0; i < depth; i++) printf("  ");
			fprintf(stream, "  Body:\n");
			dump_stmt(tu, stream, sp->for_.body, depth + 2);
			
			if (sp->for_.next) {
				for (int i = 0; i < depth; i++) printf("  ");
				fprintf(stream, "  Next:\n");
				dump_expr(tu, stream, sp->for_.next, depth + 1);
			}
			break;
		}
		default: abort();
	}
}

void ast_dump(TranslationUnit* tu, FILE* stream) {
	for (size_t i = 0, count = arrlen(tu->top_level_stmts); i < count; i++) {
		dump_stmt(tu, stream, tu->top_level_stmts[i], 0);
	}
}
