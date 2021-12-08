#include "parser.h"

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

impl_arena(Stmt, stmt_arena)
impl_arena(StmtIndex, stmt_ref_arena)
impl_arena(Expr, expr_arena)
impl_arena(ExprIndex, expr_ref_arena)

// for the global hash table
typedef struct SymbolEntry {
	Atom key;
	Symbol value;
} SymbolEntry;

static SymbolEntry* global_symbols;

static int local_symbol_count = 0;
static Symbol local_symbols[64 * 1024];

static int typedef_count = 0;
static Atom typedef_names[64 * 1024];
static TypeIndex typedefs[64 * 1024];

static void expect(TokenStream* restrict s, char ch);
static Symbol* find_local_symbol(TokenStream* restrict s);
static Symbol* find_global_symbol(char* name);
static StmtIndex parse_stmt(TokenStream* restrict s);
static ExprIndex parse_expr(TokenStream* restrict s);
static StmtIndex parse_compound_stmt(TokenStream* restrict s);
static bool try_parse_declspec(TokenStream* restrict s, Attribs* attr);
static TypeIndex parse_declspec(TokenStream* restrict s, Attribs* attr);
static Decl parse_declarator(TokenStream* restrict s, TypeIndex type);
static TypeIndex parse_typename(TokenStream* restrict s);
static bool is_typename(TokenStream* restrict s);
static _Noreturn void generic_error(TokenStream* restrict s, const char* msg);

inline static int align_up(int a, int b) { return a + (b - (a % b)) % b; }

TopLevel parse_file(TokenStream* restrict s) {
	////////////////////////////////
	// Parsing
	////////////////////////////////
	tls_init();
	
	init_stmt_arena(4 * 1024);
	init_stmt_ref_arena(4 * 1024);
	init_arg_arena(4 * 1024);
	init_expr_arena(4 * 1024);
	init_expr_ref_arena(4 * 1024);
	init_type_arena(4 * 1024);
	init_member_arena(4 * 1024);
	
	const static Type default_types[] = {
		[TYPE_NONE] = { KIND_VOID, 0, 0 },
		
		[TYPE_VOID] = { KIND_VOID, 0, 0 },
		[TYPE_BOOL] = { KIND_BOOL, 1, 1 },
		
		[TYPE_CHAR] = { KIND_CHAR, 1, 1 },
		[TYPE_SHORT] = { KIND_SHORT, 2, 2 },
		[TYPE_INT] = { KIND_INT, 4, 4 },
		[TYPE_LONG] = { KIND_LONG, 8, 8 },
		
		[TYPE_UCHAR] = { KIND_CHAR, 1, 1, .is_unsigned = true },
		[TYPE_USHORT] = { KIND_SHORT, 2, 2, .is_unsigned = true },
		[TYPE_UINT] = { KIND_INT, 4, 4, .is_unsigned = true },
		[TYPE_ULONG] = { KIND_LONG, 8, 8, .is_unsigned = true },
		
		[TYPE_FLOAT] = { KIND_FLOAT, 4, 4 },
		[TYPE_DOUBLE] = { KIND_DOUBLE, 8, 8 }
	};
	
	memcpy(type_arena.data, default_types, sizeof(default_types));
	type_arena.count = sizeof(default_types) / sizeof(default_types[0]);
	
	////////////////////////////////
	// Parse translation unit
	////////////////////////////////
	StmtIndex* top_level = NULL;
	
	while (tokens_get(s)->type) {
		// program = (typedef | function-definition | global-variable)*
		Attribs attr = { 0 };
		TypeIndex type = parse_declspec(s, &attr);
		
		if (attr.is_typedef) {
			// TODO(NeGate): Kinda ugly
			// don't expect one the first time
			bool expect_comma = false;
			while (tokens_get(s)->type != ';') {
				if (expect_comma) {
					expect(s, ',');
				} else expect_comma = true;
				
				Decl decl = parse_declarator(s, type);
				assert(decl.name);
				
				int i = typedef_count++;
				typedefs[i] = decl.type;
				typedef_names[i] = decl.name;
			}
			
			expect(s, ';');
		} else {
			if (tokens_get(s)->type == ';') {
				tokens_next(s);
				continue;
			}
			
			Decl decl = parse_declarator(s, type);
			
			if (type_arena.data[decl.type].kind == KIND_FUNC) {
				// function
				// TODO(NeGate): Check for redefines
				StmtIndex n = push_stmt_arena(1);
				stmt_arena.data[n] = (Stmt) {
					.op = STMT_DECL,
					.decl_type = decl.type,
					.decl_name = decl.name,
				};
				
				Symbol func_symbol = (Symbol){
					.name = decl.name,
					.type = decl.type,
					.storage_class = attr.is_static ? STORAGE_STATIC_FUNC : STORAGE_FUNC,
					.stmt = n
				};
				shput(global_symbols, decl.name, func_symbol);
				
				if (tokens_get(s)->type == '{') {
					ArgIndex arg_start = type_arena.data[decl.type].func.arg_start;
					ArgIndex arg_end = type_arena.data[decl.type].func.arg_end;
					
					int p = 0; // param counter
					for (ArgIndex i = arg_start; i != arg_end; i++) {
						Arg* a = &arg_arena.data[i];
						
						local_symbols[local_symbol_count++] = (Symbol){
							.name = a->name,
							.type = a->type,
							.storage_class = STORAGE_PARAM,
							.param_num = p
						};
						p += 1;
					}
					
					tokens_next(s);
					
					stmt_arena.data[n].op = STMT_FUNC_DECL;
					
					// NOTE(NeGate): STMT_FUNC_DECL is always followed by a compound block
#if NDEBUG
					parse_compound_stmt(s);
#else
					StmtIndex body = parse_compound_stmt(s);
					assert(body == n + 1);
#endif
				} else if (tokens_get(s)->type == ';') {
					// Forward decl
					tokens_next(s);
				} else {
					abort();
				}
				
				arrput(top_level, n);
				local_symbol_count = 0;
			} else {
				// TODO(NeGate): Normal decls
				abort();
			}
		}
	}
	
	////////////////////////////////
	// Semantics
	////////////////////////////////
	// NOTE(NeGate): Best thing about tables is that I don't actually have
	// walk a tree to check all nodes :)
	for (size_t i = 0; i < expr_arena.count; i++) {
		if (expr_arena.data[i].op == EXPR_UNKNOWN_SYMBOL) {
			Symbol* sym = find_global_symbol((char*)expr_arena.data[i].unknown_sym);
			
			// TODO(NeGate): Give a decent error message
			if (!sym) {
				printf("Could not find symbol: %s\n", (char*)expr_arena.data[i].unknown_sym);
				abort();
			}
			
			// Parameters are local and a special case how tf
			assert(sym->storage_class != STORAGE_PARAM);
			
			expr_arena.data[i].op = EXPR_SYMBOL;
			expr_arena.data[i].symbol = sym->stmt;
		}
	}
	
	return (TopLevel) { top_level };
}

static Symbol* find_local_symbol(TokenStream* restrict s) {
	Token* t = tokens_get(s);
	const unsigned char* name = t->start;
	size_t length = t->end - t->start;
	
	// Try local variables
	size_t i = local_symbol_count;
	while (i--) {
		// TODO(NeGate): Implement string interning
		const unsigned char* sym = local_symbols[i].name;
		size_t sym_length = strlen((const char*)sym);
		
		if (sym_length == length && memcmp(name, sym, length) == 0) {
			return &local_symbols[i];
		}
	}
	
	return NULL;
}

static Symbol* find_global_symbol(char* name) {
	ptrdiff_t search = shgeti(global_symbols, name);
	
	if (search >= 0) return &global_symbols[search].value;
	return NULL;
}

////////////////////////////////
// STATEMENTS
////////////////////////////////
static StmtIndex parse_compound_stmt(TokenStream* restrict s) {
	// mark when the local scope starts
	int saved = local_symbol_count;
	
	StmtIndex node = push_stmt_arena(1);
	stmt_arena.data[node] = (Stmt) {
		.op = STMT_COMPOUND
	};
	
	size_t body_count = 0; // He be fuckin
	void* body = tls_save();
	
	while (tokens_get(s)->type != '}') {
		StmtIndex stmt = parse_stmt(s);
		
		if (stmt) {
			*((StmtIndex*)tls_push(sizeof(StmtIndex))) = stmt;
			body_count++;
		} else if (is_typename(s)) {
			Attribs attr = { 0 };
			TypeIndex type = parse_declspec(s, &attr);
			
			// TODO(NeGate): Kinda ugly
			// don't expect one the first time
			bool expect_comma = false;
			while (tokens_get(s)->type != ';') {
				if (expect_comma) {
					expect(s, ',');
				} else expect_comma = true;
				
				Decl decl = parse_declarator(s, type);
				
				StmtIndex n = push_stmt_arena(1);
				stmt_arena.data[n] = (Stmt) {
					.op = STMT_DECL,
					.decl_type = decl.type,
					.decl_name = decl.name,
				};
				local_symbols[local_symbol_count++] = (Symbol){
					.name = decl.name,
					.type = decl.type,
					.storage_class = STORAGE_LOCAL,
					.stmt = n
				};
				
				if (tokens_get(s)->type == '=') {
					// initial value
					tokens_next(s);
					
					stmt_arena.data[n].expr = parse_expr(s);
				}
				
				*((StmtIndex*)tls_push(sizeof(StmtIndex))) = n;
				body_count++;
			}
			
			expect(s, ';');
		} else {
			stmt = push_stmt_arena(1);
			stmt_arena.data[stmt].op = STMT_EXPR;
			stmt_arena.data[stmt].expr = parse_expr(s);
			
			*((StmtIndex*)tls_push(sizeof(StmtIndex))) = stmt;
			body_count++;
			
			expect(s, ';');
		}
	}
	expect(s, '}');
	local_symbol_count = saved;
	
	StmtIndexIndex start = push_stmt_ref_arena(body_count);
	memcpy(&stmt_ref_arena.data[start], body, body_count * sizeof(ArgIndex));
	
	stmt_arena.data[node].kids_start = start;
	stmt_arena.data[node].kids_end = start + body_count;
	
	tls_restore(body);
	return node;
}

// TODO(NeGate): Doesn't handle declarators or expression-statements
static StmtIndex parse_stmt(TokenStream* restrict s) {
	if (tokens_get(s)->type == '{') {
		tokens_next(s);
		return parse_compound_stmt(s);
	}
	
	if (tokens_get(s)->type == TOKEN_KW_return) {
		tokens_next(s);
		
		StmtIndex n = push_stmt_arena(1);
		stmt_arena.data[n].op = STMT_RETURN;
		
		if (tokens_get(s)->type != ';') {
			stmt_arena.data[n].expr = parse_expr(s);
		}
		
		expect(s, ';');
		return n;
	}
	
	if (tokens_get(s)->type == TOKEN_KW_if) {
		tokens_next(s);
		
		StmtIndex n = push_stmt_arena(1);
		stmt_arena.data[n].op = STMT_IF;
		
		expect(s, '(');
		stmt_arena.data[n].expr = parse_expr(s);
		expect(s, ')');
		
		stmt_arena.data[n].body = parse_stmt(s);
		
		if (tokens_get(s)->type == TOKEN_KW_else) {
			tokens_next(s);
			stmt_arena.data[n].body2 = parse_stmt(s);
		} else {
			stmt_arena.data[n].body2 = 0;
		}
		
		return n;
	}
	
	if (tokens_get(s)->type == TOKEN_KW_while) {
		tokens_next(s);
		
		StmtIndex n = push_stmt_arena(1);
		stmt_arena.data[n].op = STMT_WHILE;
		
		expect(s, '(');
		stmt_arena.data[n].expr = parse_expr(s);
		expect(s, ')');
		
		stmt_arena.data[n].body = parse_stmt(s);
		return n;
	}
	
	if (tokens_get(s)->type == TOKEN_KW_do) {
		tokens_next(s);
		
		StmtIndex n = push_stmt_arena(1);
		stmt_arena.data[n].op = STMT_DO_WHILE;
		stmt_arena.data[n].body = parse_stmt(s);
		
		if (tokens_get(s)->type != TOKEN_KW_while) {
			//int loc = lexer_get_location(s);
			//printf("error on line %d: expected 'while' got '%.*s'", loc, (int)(l->token_end - l->token_start), l->token_start);
			abort();
		}
		tokens_next(s);
		
		expect(s, '(');
		stmt_arena.data[n].expr = parse_expr(s);
		expect(s, ')');
		expect(s, ';');
		return n;
	}
	
	if (tokens_get(s)->type == ';') {
		tokens_next(s);
		return 0;
	}
	
	return 0;
}

////////////////////////////////
// EXPRESSIONS
//
// Quick reference:
// https://en.cppreference.com/w/c/language/operator_precedence
////////////////////////////////
static ExprIndex parse_expr_l0(TokenStream* restrict s) {
	if (tokens_get(s)->type == '(') {
		tokens_next(s);
		ExprIndex e = parse_expr(s);
		expect(s, ')');
		
		return e;
	} else if (tokens_get(s)->type == TOKEN_IDENTIFIER) {
		Symbol* sym = find_local_symbol(s);
		
		ExprIndex e = push_expr_arena(1);
		if (sym) {
			if (sym->storage_class == STORAGE_PARAM) {
				expr_arena.data[e] = (Expr) {
					.op = EXPR_PARAM,
					.param_num = sym->param_num
				};
			} else {
				expr_arena.data[e] = (Expr) {
					.op = EXPR_SYMBOL,
					.symbol = sym->stmt
				};
			}
		} else {
			// We'll defer any global identifier resolution
			Token* t = tokens_get(s);
			Atom name = atoms_put(t->end - t->start, t->start);
			
			expr_arena.data[e] = (Expr) {
				.op = EXPR_UNKNOWN_SYMBOL,
				.unknown_sym = name
			};
		}
		
		tokens_next(s);
		return e;
	} else if (tokens_get(s)->type == TOKEN_NUMBER) {
		Token* t = tokens_get(s);
		
		size_t len = t->end - t->start;
		assert(len < 15);
		
		char temp[16];
		memcpy(temp, t->start, len);
		temp[len] = '\0';
		
		long long i = atoll(temp);
		
		ExprIndex e = push_expr_arena(1);
		expr_arena.data[e] = (Expr) {
			.op = EXPR_NUM,
			.num = i
		};
		
		tokens_next(s);
		return e;
	} else {
		generic_error(s, "Could not parse expression!");
	}
}

static ExprIndex parse_expr_l1(TokenStream* restrict s) {
	if (tokens_get(s)->type == '(') {
		tokens_next(s);
		
		if (is_typename(s)) {
			TypeIndex type = parse_typename(s);
			expect(s, ')');
			
			// TODO(NeGate): Handle compound literal
			ExprIndex base = parse_expr_l0(s);
			ExprIndex e = push_expr_arena(1);
			
			expr_arena.data[e].op = EXPR_CAST;
			expr_arena.data[e].cast.type = type;
			expr_arena.data[e].cast.src = base;
			return e;
		}
		
		tokens_prev(s);
	}
	
	ExprIndex e = parse_expr_l0(s);
	
	// after any of the: [] () . ->
	// it'll restart and take a shot at matching another
	// piece of the expression.
	try_again: {
		if (tokens_get(s)->type == '[') {
			ExprIndex base = e;
			e = push_expr_arena(1);
			
			tokens_next(s);
			ExprIndex index = parse_expr(s);
			expect(s, ']');
			
			expr_arena.data[e].op = EXPR_SUBSCRIPT;
			expr_arena.data[e].subscript.base = base;
			expr_arena.data[e].subscript.index = index;
			goto try_again;
		}
		
		// Member access
		if (tokens_get(s)->type == '.') {
			tokens_next(s);
			if (tokens_get(s)->type != TOKEN_IDENTIFIER) {
				generic_error(s, "Expected identifier after member access a.b");
			}
			
			Token* t = tokens_get(s);
			Atom name = atoms_put(t->end - t->start, t->start);
			
			ExprIndex base = e;
			e = push_expr_arena(1);
			expr_arena.data[e] = (Expr) {
				.op = EXPR_DOT,
				.dot = { base, name }
			};
			
			tokens_next(s);
			goto try_again;
		}
		
		// Function call
		if (tokens_get(s)->type == '(') {
			tokens_next(s);
			
			ExprIndex target = e;
			e = push_expr_arena(1);
			
			size_t param_count = 0;
			void* params = tls_save();
			
			while (tokens_get(s)->type != ')') {
				if (param_count) {
					expect(s, ',');
				}
				
				*((ExprIndex*) tls_push(sizeof(ExprIndex))) = parse_expr(s);
				param_count++;
			}
			
			if (tokens_get(s)->type != ')') {
				generic_error(s, "Unclosed parameter list!");
			}
			tokens_next(s);
			
			ExprIndexIndex start = push_expr_ref_arena(param_count);
			memcpy(&expr_ref_arena.data[start], params, param_count * sizeof(ExprIndex));
			
			expr_arena.data[e] = (Expr) {
				.op = EXPR_CALL,
				.call = { target, start, start + param_count }
			};
			
			tls_restore(params);
			goto try_again;
		}
		
		// post fix, you can only put one and just after all the other operators
		// in this precendence.
		if (tokens_get(s)->type == TOKEN_INCREMENT || tokens_get(s)->type == TOKEN_DECREMENT) {
			bool is_inc = tokens_get(s)->type == TOKEN_INCREMENT;
			tokens_next(s);
			
			ExprIndex src = e;
			
			e = push_expr_arena(1);
			expr_arena.data[e].op = is_inc ? EXPR_POST_INC : EXPR_POST_DEC;
			expr_arena.data[e].unary_op.src = src;
		}
		
		return e;
	}
}

// deref* address&
static ExprIndex parse_expr_l2(TokenStream* restrict s) {
	if (tokens_get(s)->type == '&') {
		tokens_next(s);
		
		ExprIndex value = parse_expr_l1(s);
		
		ExprIndex e = push_expr_arena(1);
		expr_arena.data[e] = (Expr) {
			.op = EXPR_ADDR,
			.unary_op.src = value
		};
		return e;
	}
	
	int derefs = 0;
	while (tokens_get(s)->type == '*') {
		tokens_next(s);
		derefs++;
	}
	
	ExprIndex e = parse_expr_l1(s);
	
	while (derefs--) {
		ExprIndex base = e;
		e = push_expr_arena(1);
		
		expr_arena.data[e] = (Expr) {
			.op = EXPR_DEREF,
			.unary_op.src = base
		};
	}
	
	return e;
}

// * / %
static ExprIndex parse_expr_l3(TokenStream* restrict s) {
	ExprIndex lhs = parse_expr_l2(s);
	
	while (tokens_get(s)->type == TOKEN_TIMES ||
		   tokens_get(s)->type == TOKEN_SLASH ||
		   tokens_get(s)->type == TOKEN_PERCENT) {
		ExprIndex e = push_expr_arena(1);
		ExprOp op;
		switch (tokens_get(s)->type) {
			case TOKEN_TIMES: op = EXPR_TIMES; break;
			case TOKEN_SLASH: op = EXPR_SLASH; break;
			case TOKEN_PERCENT: op = EXPR_PERCENT; break;
			default: __builtin_unreachable();
		}
		tokens_next(s);
		
		ExprIndex rhs = parse_expr_l2(s);
		expr_arena.data[e] = (Expr) {
			.op = op,
			.bin_op = { lhs, rhs }
		};
		
		lhs = e;
	}
	
	return lhs;
}

// + -
static ExprIndex parse_expr_l4(TokenStream* restrict s) {
	ExprIndex lhs = parse_expr_l3(s);
	
	while (tokens_get(s)->type == TOKEN_PLUS ||
		   tokens_get(s)->type == TOKEN_MINUS) {
		ExprIndex e = push_expr_arena(1);
		ExprOp op;
		switch (tokens_get(s)->type) {
			case TOKEN_PLUS: op = EXPR_PLUS; break;
			case TOKEN_MINUS: op = EXPR_MINUS; break;
			default: __builtin_unreachable();
		}
		tokens_next(s);
		
		ExprIndex rhs = parse_expr_l3(s);
		expr_arena.data[e] = (Expr) {
			.op = op,
			.bin_op = { lhs, rhs }
		};
		
		lhs = e;
	}
	
	return lhs;
}

// >= > <= <
static ExprIndex parse_expr_l6(TokenStream* restrict s) {
	ExprIndex lhs = parse_expr_l4(s);
	
	while (tokens_get(s)->type == TOKEN_GREATER_EQUAL ||
		   tokens_get(s)->type == TOKEN_LESS_EQUAL || 
		   tokens_get(s)->type == TOKEN_GREATER ||
		   tokens_get(s)->type == TOKEN_LESS) {
		ExprIndex e = push_expr_arena(1);
		ExprOp op;
		switch (tokens_get(s)->type) {
			case TOKEN_LESS:          op = EXPR_CMPLT; break;
			case TOKEN_LESS_EQUAL:    op = EXPR_CMPLE; break;
			case TOKEN_GREATER:       op = EXPR_CMPGT; break;
			case TOKEN_GREATER_EQUAL: op = EXPR_CMPGE; break;
			default: __builtin_unreachable();
		}
		tokens_next(s);
		
		ExprIndex rhs = parse_expr_l4(s);
		expr_arena.data[e] = (Expr) {
			.op = op,
			.bin_op = { lhs, rhs }
		};
		
		lhs = e;
	}
	
	return lhs;
}

// == !=
static ExprIndex parse_expr_l7(TokenStream* restrict s) {
	ExprIndex lhs = parse_expr_l6(s);
	
	while (tokens_get(s)->type == TOKEN_NOT_EQUAL ||
		   tokens_get(s)->type == TOKEN_EQUALITY) {
		ExprIndex e = push_expr_arena(1);
		ExprOp op = tokens_get(s)->type == TOKEN_EQUALITY ? EXPR_CMPEQ : EXPR_CMPNE;
		tokens_next(s);
		
		ExprIndex rhs = parse_expr_l6(s);
		expr_arena.data[e] = (Expr) {
			.op = op,
			.bin_op = { lhs, rhs }
		};
		
		lhs = e;
	}
	
	return lhs;
}

// &&
static ExprIndex parse_expr_l11(TokenStream* restrict s) {
	ExprIndex lhs = parse_expr_l7(s);
	
	while (tokens_get(s)->type == TOKEN_DOUBLE_AND) {
		ExprIndex e = push_expr_arena(1);
		ExprOp op = EXPR_LOGICAL_AND;
		tokens_next(s);
		
		ExprIndex rhs = parse_expr_l6(s);
		expr_arena.data[e] = (Expr) {
			.op = op,
			.bin_op = { lhs, rhs }
		};
		
		lhs = e;
	}
	
	return lhs;
}

// ||
static ExprIndex parse_expr_l12(TokenStream* restrict s) {
	ExprIndex lhs = parse_expr_l11(s);
	
	while (tokens_get(s)->type == TOKEN_DOUBLE_OR) {
		ExprIndex e = push_expr_arena(1);
		ExprOp op = EXPR_LOGICAL_OR;
		tokens_next(s);
		
		ExprIndex rhs = parse_expr_l11(s);
		expr_arena.data[e] = (Expr) {
			.op = op,
			.bin_op = { lhs, rhs }
		};
		
		lhs = e;
	}
	
	return lhs;
}

// = += -= *= /= %= <<= >>= &= ^= |=
static ExprIndex parse_expr_l14(TokenStream* restrict s) {
	ExprIndex lhs = parse_expr_l12(s);
	
	while (tokens_get(s)->type == TOKEN_ASSIGN ||
		   tokens_get(s)->type == TOKEN_PLUS_EQUAL ||
		   tokens_get(s)->type == TOKEN_MINUS_EQUAL ||
		   tokens_get(s)->type == TOKEN_TIMES_EQUAL ||
		   tokens_get(s)->type == TOKEN_SLASH_EQUAL ||
		   tokens_get(s)->type == TOKEN_PERCENT_EQUAL ||
		   tokens_get(s)->type == TOKEN_AND_EQUAL ||
		   tokens_get(s)->type == TOKEN_OR_EQUAL ||
		   tokens_get(s)->type == TOKEN_XOR_EQUAL ||
		   tokens_get(s)->type == TOKEN_LEFT_SHIFT_EQUAL ||
		   tokens_get(s)->type == TOKEN_RIGHT_SHIFT_EQUAL) {
		ExprIndex e = push_expr_arena(1);
		
		ExprOp op;
		switch (tokens_get(s)->type) {
			case TOKEN_ASSIGN: op = EXPR_ASSIGN; break;
			case TOKEN_PLUS_EQUAL: op = EXPR_PLUS_ASSIGN; break;
			case TOKEN_MINUS_EQUAL: op = EXPR_MINUS_ASSIGN; break;
			case TOKEN_TIMES_EQUAL: op = EXPR_TIMES_ASSIGN; break;
			case TOKEN_SLASH_EQUAL: op = EXPR_SLASH_ASSIGN; break;
			case TOKEN_PERCENT_EQUAL: op = EXPR_PERCENT_ASSIGN; break;
			case TOKEN_AND_EQUAL: op = EXPR_AND_ASSIGN; break;
			case TOKEN_OR_EQUAL: op = EXPR_OR_ASSIGN; break;
			case TOKEN_XOR_EQUAL: op = EXPR_XOR_ASSIGN; break;
			case TOKEN_LEFT_SHIFT_EQUAL: op = EXPR_SHL_ASSIGN; break;
			case TOKEN_RIGHT_SHIFT_EQUAL: op = EXPR_SHR_ASSIGN; break;
			default: __builtin_unreachable();
		}
		tokens_next(s);
		
		ExprIndex rhs = parse_expr_l12(s);
		expr_arena.data[e] = (Expr) {
			.op = op,
			.bin_op = { lhs, rhs }
		};
		
		lhs = e;
	}
	
	return lhs;
}

static ExprIndex parse_expr(TokenStream* restrict s) {
	return parse_expr_l14(s);
}

////////////////////////////////
// TYPES
////////////////////////////////
static TypeIndex parse_type_suffix(TokenStream* restrict s, TypeIndex type, Atom name);

static Decl parse_declarator(TokenStream* restrict s, TypeIndex type) {
	// handle pointers
	while (tokens_get(s)->type == '*') {
		type = new_pointer(type);
		tokens_next(s);
		
		// TODO(NeGate): parse qualifiers
		switch (tokens_get(s)->type) {
			case TOKEN_KW_const:
			case TOKEN_KW_volatile:
			case TOKEN_KW_restrict: 
			tokens_next(s);
			break;
			default: break;
		}
	}
	
	if (tokens_get(s)->type == '(') {
		// TODO(NeGate): I don't like this code...
		// it essentially just skips over the stuff in the
		// parenthesis to do the suffix then comes back
		// for the parenthesis after wards, restoring back to
		// the end of the declarator when it's done.
		//
		// should be right after the (
		tokens_next(s);
		size_t saved = s->current;
		
		parse_declarator(s, TYPE_NONE);
		
		expect(s, ')');
		type = parse_type_suffix(s, type, NULL);
		
		size_t saved_end = s->current;
		s->current = saved;
		
		Decl d = parse_declarator(s, type);
		
		// inherit name
		if (!d.name) {
			TypeIndex t = d.type;
			
			if (type_arena.data[t].kind == KIND_PTR) {
				t = type_arena.data[d.type].ptr_to;
				
				if (type_arena.data[t].kind == KIND_FUNC) {
					d.name = type_arena.data[t].func.name;
				} else if (type_arena.data[t].kind == KIND_STRUCT) {
					d.name = type_arena.data[t].record.name;
				} else if (type_arena.data[t].kind == KIND_UNION) {
					d.name = type_arena.data[t].record.name;
				}
			}
		}
		
		s->current = saved_end;
		return d;
	}
	
	Atom name = NULL;
	Token* t = tokens_get(s);
	if (t->type == TOKEN_IDENTIFIER) {
		name = atoms_put(t->end - t->start, t->start);
		tokens_next(s);
	}
	
	type = parse_type_suffix(s, type, name);
	return (Decl){ type, name };
}

// it's like a declarator with a skin fade,
// int*        int[16]       const char* restrict
static TypeIndex parse_abstract_declarator(TokenStream* restrict s, TypeIndex type) {
	// handle pointers
	while (tokens_get(s)->type == '*') {
		type = new_pointer(type);
		tokens_next(s);
		
		// TODO(NeGate): parse qualifiers
		switch (tokens_get(s)->type) {
			case TOKEN_KW_const:
			case TOKEN_KW_volatile:
			case TOKEN_KW_restrict: 
			tokens_next(s);
			break;
			default: break;
		}
	}
	
	if (tokens_get(s)->type == '(') {
		// TODO(NeGate): I don't like this code...
		// it essentially just skips over the stuff in the
		// parenthesis to do the suffix then comes back
		// for the parenthesis after wards, restoring back to
		// the end of the declarator when it's done.
		//
		// should be right after the (
		tokens_next(s);
		size_t saved = s->current;
		
		parse_abstract_declarator(s, TYPE_NONE);
		
		expect(s, ')');
		type = parse_type_suffix(s, type, NULL);
		
		size_t saved_end = s->current;
		s->current = saved;
		
		TypeIndex d = parse_abstract_declarator(s, type);
		s->current = saved_end;
		return d;
	}
	
	Atom name = NULL;
	Token* t = tokens_get(s);
	if (t->type == TOKEN_IDENTIFIER) {
		name = atoms_put(t->end - t->start, t->start);
		tokens_next(s);
	}
	
	type = parse_type_suffix(s, type, name);
	return type;
}

static TypeIndex parse_typename(TokenStream* restrict s) {
	// TODO(NeGate): Check if attributes are set, they shouldn't
	// be in this context.
	Attribs attr = { 0 };
	TypeIndex type = parse_declspec(s, &attr);
	return parse_abstract_declarator(s, type);
}

static TypeIndex parse_type_suffix(TokenStream* restrict s, TypeIndex type, Atom name) {
	// type suffixes like array [] and function ()
	if (tokens_get(s)->type == '(') {
		tokens_next(s);
		
		// TODO(NeGate): implement (void) param
		TypeIndex return_type = type;
		
		type = new_func();
		type_arena.data[type].func.name = name;
		type_arena.data[type].func.return_type = return_type;
		
		size_t arg_count = 0;
		void* args = tls_save();
		
		while (tokens_get(s)->type != ')') {
			if (arg_count) {
				expect(s, ',');
			}
			
			Attribs arg_attr = { 0 };
			TypeIndex arg_base_type = parse_declspec(s, &arg_attr);
			
			Decl arg_decl = parse_declarator(s, arg_base_type);
			TypeIndex arg_type = arg_decl.type;
			
			if (type_arena.data[arg_type].kind == KIND_ARRAY) {
				// Array parameters are desugared into pointers
				arg_type = new_pointer(type_arena.data[arg_type].array_of);
			} else if (type_arena.data[arg_type].kind == KIND_FUNC) {
				// Function parameters are desugared into pointers
				arg_type = new_pointer(arg_type);
			}
			
			// TODO(NeGate): Error check that no attribs are set
			*((Arg*)tls_push(sizeof(Arg))) = (Arg) {
				.type = arg_type,
				.name = arg_decl.name
			};
			arg_count++;
		}
		
		if (tokens_get(s)->type != ')') {
			generic_error(s, "Unclosed parameter list!");
		}
		tokens_next(s);
		
		ArgIndex start = push_arg_arena(arg_count);
		memcpy(&arg_arena.data[start], args, arg_count * sizeof(Arg));
		
		type_arena.data[type].func.arg_start = start;
		type_arena.data[type].func.arg_end = start + arg_count;
		
		tls_restore(args);
	} else if (tokens_get(s)->type == '[') {
		do {
			tokens_next(s);
			
			// TODO(NeGate): Implement array typedecl properly
			long long count;
			
			Token* t = tokens_get(s);
			if (t->type == ']') {
				count = 0; 
				tokens_next(s);
			} else if (t->type == TOKEN_NUMBER) {
				size_t len = t->end - t->start;
				assert(len < 15);
				
				char temp[16];
				memcpy(temp, t->start, len);
				temp[len] = '\0';
				
				count = atoll(temp);
				tokens_next(s);
				
				expect(s, ']');
			} else {
				abort();
			}
			
			type = new_array(type, count);
		} while (tokens_get(s)->type == '[');
	}
	
	return type;
}

// https://github.com/rui314/chibicc/blob/90d1f7f199cc55b13c7fdb5839d1409806633fdb/parse.c#L381
static TypeIndex parse_declspec(TokenStream* restrict s, Attribs* attr) {
	enum {
		VOID     = 1 << 0,
		BOOL     = 1 << 2,
		CHAR     = 1 << 4,
		SHORT    = 1 << 6,
		INT      = 1 << 8,
		LONG     = 1 << 10,
		FLOAT    = 1 << 12,
		DOUBLE   = 1 << 14,
		OTHER    = 1 << 16,
		SIGNED   = 1 << 17,
		UNSIGNED = 1 << 18,
	};
	
	int counter = 0;
	TypeIndex type = TYPE_NONE;
	
	bool is_atomic = false;
	bool is_const = false;
	do {
		switch (tokens_get(s)->type) {
			case TOKEN_KW_void: counter += VOID; break;
			case TOKEN_KW_Bool: counter += BOOL; break;
			case TOKEN_KW_char: counter += CHAR; break;
			case TOKEN_KW_short: counter += SHORT; break;
			case TOKEN_KW_int: counter += INT; break;
			case TOKEN_KW_long: counter += LONG; break;
			case TOKEN_KW_float: counter += FLOAT; break;
			case TOKEN_KW_double: counter += DOUBLE; break;
			
			case TOKEN_KW_unsigned: counter |= UNSIGNED; break;
			case TOKEN_KW_signed: counter |= SIGNED; break;
			
			case TOKEN_KW_static: attr->is_static = true; break;
			case TOKEN_KW_typedef: attr->is_typedef = true; break;
			case TOKEN_KW_inline: attr->is_inline = true; break;
			case TOKEN_KW_Thread_local: attr->is_tls = true; break;
			
			case TOKEN_KW_Atomic: is_atomic = true; break;
			case TOKEN_KW_const: is_const = true; break;
			case TOKEN_KW_auto: break;
			
			case TOKEN_KW_struct: {
				if (counter) goto done;
				tokens_next(s);
				
				Atom name = NULL;
				Token* t = tokens_get(s);
				if (tokens_get(s)->type == TOKEN_IDENTIFIER) {
					name = atoms_put(t->end - t->start, t->start);
					tokens_next(s);
				}
				
				if (tokens_get(s)->type == '{') {
					tokens_next(s);
					
					type = new_struct();
					type_arena.data[type].record.name = name;
					counter += OTHER;
					
					size_t member_count = 0;
					Member* members = tls_save();
					
					int offset = 0;
					
					// struct/union are aligned to the biggest member alignment
					int align = 0;
					
					while (tokens_get(s)->type != '}') {
						Attribs member_attr = { 0 };
						TypeIndex member_base_type = parse_declspec(s, &member_attr);
						
						Decl decl = parse_declarator(s, member_base_type);
						TypeIndex member_type = decl.type;
						
						if (type_arena.data[member_type].kind == KIND_FUNC) {
							generic_error(s, "Naw dawg");
						}
						
						int member_align = type_arena.data[member_type].align;
						int member_size = type_arena.data[member_type].size;
						
						offset = align_up(offset, member_align);
						
						// TODO(NeGate): Error check that no attribs are set
						tls_push(sizeof(Member));
						members[member_count++] = (Member) {
							.type = member_type,
							.name = decl.name,
							.offset = offset,
							.align = member_align
						};
						
						offset += member_size;
						if (member_align > align) {
							align = member_align;
						}
						
						expect(s, ';');
					}
					
					if (tokens_get(s)->type != '}') {
						generic_error(s, "Unclosed member list!");
					}
					
					offset = align_up(offset, align);
					type_arena.data[type].size = offset;
					type_arena.data[type].align = align;
					
					MemberIndex start = push_arg_arena(member_count);
					memcpy(&member_arena.data[start], members, member_count * sizeof(Member));
					
					type_arena.data[type].record.kids_start = start;
					type_arena.data[type].record.kids_end = start + member_count;
					
					tls_restore(members);
				} else {
					// must be a forward decl
					abort();
				}
				break;
			}
			
			case TOKEN_IDENTIFIER: {
				if (counter) goto done;
				
				Token* t = tokens_get(s);
				size_t len = t->end - t->start;
				
				int i = typedef_count;
				while (i--) {
					size_t typedef_len = strlen((const char*)typedef_names[i]);
					
					if (len == typedef_len &&
						memcmp(t->start, typedef_names[i], len) == 0) {
						type = typedefs[i];
						counter += OTHER;
						break;
					}
				}
				
				if (counter) break;
				
				// if not a typename, this isn't a typedecl
				goto done;
			}
			default: goto done;
		}
		
		switch (counter) {
			case 0: break; // not resolved yet
			case VOID:
			type = TYPE_VOID;
			break;
			case BOOL:
			type = TYPE_BOOL;
			break;
			case CHAR:
			case SIGNED + CHAR:
			type = TYPE_CHAR;
			break;
			case UNSIGNED + CHAR:
			type = TYPE_UCHAR;
			break;
			case SHORT:
			case SHORT + INT:
			case SIGNED + SHORT:
			case SIGNED + SHORT + INT:
			type = TYPE_SHORT;
			break;
			case UNSIGNED + SHORT:
			case UNSIGNED + SHORT + INT:
			type = TYPE_USHORT;
			break;
			case INT:
			case SIGNED:
			case SIGNED + INT:
			type = TYPE_INT;
			break;
			case UNSIGNED:
			case UNSIGNED + INT:
			type = TYPE_UINT;
			break;
			case LONG:
			case LONG + INT:
			case LONG + LONG:
			case LONG + LONG + INT:
			case SIGNED + LONG:
			case SIGNED + LONG + INT:
			case SIGNED + LONG + LONG:
			case SIGNED + LONG + LONG + INT:
			type = TYPE_LONG;
			break;
			case UNSIGNED + LONG:
			case UNSIGNED + LONG + INT:
			case UNSIGNED + LONG + LONG:
			case UNSIGNED + LONG + LONG + INT:
			type = TYPE_ULONG;
			break;
			case FLOAT:
			type = TYPE_FLOAT;
			break;
			case DOUBLE:
			case LONG + DOUBLE:
			type = TYPE_DOUBLE;
			break;
			case OTHER:
			assert(type);
			break;
			default:
			generic_error(s, "invalid type");
			break;
		}
		
		tokens_next(s);
	} while (true);
	
	done:
	if (type == 0) {
		generic_error(s, "Unknown typename");
	}
	
	if (is_atomic || is_const) {
		type = copy_type(type);
		type_arena.data[type].is_atomic = is_atomic;
		type_arena.data[type].is_const = is_const;
	}
	
	return type;
}

static bool is_typename(TokenStream* restrict s) {
	Token* t = tokens_get(s);
	
	switch (t->type) {
		case TOKEN_KW_void:
		case TOKEN_KW_char:
		case TOKEN_KW_short:
		case TOKEN_KW_int:
		case TOKEN_KW_long:
		case TOKEN_KW_float:
		case TOKEN_KW_double:
		case TOKEN_KW_Bool:
		case TOKEN_KW_signed:
		case TOKEN_KW_unsigned:
		case TOKEN_KW_static:
		case TOKEN_KW_typedef:
		case TOKEN_KW_inline:
		case TOKEN_KW_Thread_local:
		case TOKEN_KW_auto:
		return true;
		
		case TOKEN_IDENTIFIER: {
			size_t len = t->end - t->start;
			
			int i = typedef_count;
			while (i--) {
				size_t typedef_len = strlen((const char*)typedef_names[i]);
				
				if (len == typedef_len &&
					memcmp(t->start, typedef_names[i], len) == 0) {
					return true;
				}
			}
			
			return false;
		}
		default:
		return false;
	}
}

////////////////////////////////
// ERRORS
////////////////////////////////
static _Noreturn void generic_error(TokenStream* restrict s, const char* msg) {
	//int loc = lexer_get_location(s);
	//printf("error on line %d: %s\n", loc, msg);
	abort();
}

static void expect(TokenStream* restrict s, char ch) {
	if (tokens_get(s)->type != ch) {
		//int loc = lexer_get_location(s);
		//printf("error on line %d: expected '%c' got '%.*s'", loc, ch, (int)(l->token_end - l->token_start), l->token_start);
		abort();
	}
	
	tokens_next(s);
}
