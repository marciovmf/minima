#ifndef MINIMA_PARSE_H
#define MINIMA_PARSE_H

#include <stdx_common.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdx_string.h>
#include <stdx_arena.h>


//----------------------------------------------------------
// Tokens
//----------------------------------------------------------

typedef enum MiTokenKind
{
  MI_TOK_EOF = 0,
  MI_TOK_IDENTIFIER,
  MI_TOK_INCLUDE,
  MI_TOK_IMPORT,
  MI_TOK_FUNC,
  MI_TOK_RETURN,
  MI_TOK_LET,
  MI_TOK_IF,
  MI_TOK_ELSE,
  MI_TOK_WHILE,
  MI_TOK_FOREACH,
  MI_TOK_INT,
  MI_TOK_FLOAT,
  MI_TOK_STRING,
  MI_TOK_TRUE,
  MI_TOK_FALSE,
  MI_TOK_VOID,
  MI_TOK_AND,
  MI_TOK_OR,
  MI_TOK_NOT,
  MI_TOK_LPAREN,      // (
  MI_TOK_RPAREN,      // )
  MI_TOK_LBRACKET,    // [
  MI_TOK_RBRACKET,    // ]
  MI_TOK_LBRACE,      // {
  MI_TOK_RBRACE,      // }
  MI_TOK_COMMA,       // ,
  MI_TOK_COLON,       // :
  MI_TOK_DOLLAR,      // $
  MI_TOK_SEMICOLON,   // ';' statement terminator
  MI_TOK_PLUS,        // + 
  MI_TOK_MINUS,       // - 
  MI_TOK_STAR,        // * 
  MI_TOK_SLASH,       // / 
  MI_TOK_EQ,          // =
  MI_TOK_EQEQ,        // ==
  MI_TOK_BANGEQ,      // !=
  MI_TOK_LT,          // < 
  MI_TOK_GT,          // > 
  MI_TOK_LTEQ,        // <=
  MI_TOK_GTEQ,        // >=
  MI_TOK_DOUBLE_COLON,// ::
  MI_TOK_ERROR        // internal error token
} MiTokenKind;


// Backwards compatibility (older code used MI_TOK_NEWLINE for ';')
#define MI_TOK_NEWLINE MI_TOK_SEMICOLON
typedef struct MiToken
{
  MiTokenKind kind;
  XSlice      lexeme;     // view into source
  int         line;
  int         column;
} MiToken;

//----------------------------------------------------------
// AST
//----------------------------------------------------------

struct MiExpr;
struct MiCommand;
struct MiScript;

typedef struct MiExprPair
{
  struct MiExpr   *left;
  struct MiExpr   *right;
} MiExprPair;

typedef struct MiExprList
{
  struct MiExpr   *expr;
  struct MiExprList *next;
} MiExprList;

typedef struct MiCommandList
{
  struct MiCommand   *command;
  struct MiCommandList *next;
} MiCommandList;

typedef enum MiExprKind
{
  MI_EXPR_INT_LITERAL,
  MI_EXPR_FLOAT_LITERAL,
  MI_EXPR_STRING_LITERAL,
  MI_EXPR_BOOL_LITERAL,
  MI_EXPR_VOID_LITERAL,
  MI_EXPR_VAR,              // $x or $("x") (dynamic not implementado ainda)
  MI_EXPR_INDEX,            // target[index]
  MI_EXPR_UNARY,
  MI_EXPR_BINARY,
  MI_EXPR_LIST,             // [a, b, c]
  MI_EXPR_DICT,             // [k = v, ...]  (pairs are only valid inside dict literals)
  MI_EXPR_PAIR,             // k = v (only produced inside dict literals)
  MI_EXPR_BLOCK,            // { script }
MI_EXPR_QUAL,             // target::member
MI_EXPR_COMMAND            // head_expr : arg_expr*  (when used in an expression)
} MiExprKind;

//----------------------------------------------------------
// Types (front-end only)
//----------------------------------------------------------

typedef enum MiTypeKind
{
  MI_TYPE_VOID = 0,
  MI_TYPE_BOOL,
  MI_TYPE_INT,
  MI_TYPE_FLOAT,
  MI_TYPE_STRING,
  MI_TYPE_LIST,
  MI_TYPE_DICT,
  MI_TYPE_BLOCK,
  MI_TYPE_FUNC,
  MI_TYPE_ANY
} MiTypeKind;

// Optional function signature used in type annotations like func(int)->void.
typedef struct MiFuncTypeSig
{
  MiToken     func_tok;
  MiToken     lparen_tok;
  MiToken     rparen_tok;
  MiToken     ret_tok;
  MiTypeKind  ret_type;

  MiTypeKind* param_types;
  int         param_count;
  bool        is_variadic;
  MiTypeKind   variadic_type;
} MiFuncTypeSig;


typedef struct MiFuncParam
{
  MiToken    name_tok;
  XSlice     name;
  MiToken    type_tok;
  MiTypeKind type;
  MiFuncTypeSig* func_sig;
} MiFuncParam;

typedef struct MiFuncSig
{
  MiToken    name_tok;
  XSlice     name;
  MiFuncParam* params;
  int       param_count;
  MiToken    ret_tok;
  MiTypeKind ret_type;
  MiFuncTypeSig* ret_func_sig;
} MiFuncSig;

typedef struct MiExpr
{
  MiExprKind kind;
  MiToken    token;         // Main token fork debugging/error reporting
  bool       can_fold;      // known at compile time

  union
  {
    struct { int64_t value; } int_lit;
    struct { double  value; } float_lit;
    struct { XSlice  value; } string_lit;
    struct { bool    value; } bool_lit;

    struct
    {
      bool   is_indirect;
      XSlice name;              // variable name without $ when is_indirect == false
      struct MiExpr *name_expr; // expressão (string) quando is_indirect == true
    } var;

    struct
    {
      struct MiExpr *target;
      struct MiExpr *index;
    } index;

    struct
    {
      MiTokenKind    op;
      struct MiExpr *expr;
    } unary;

    struct
    {
      MiTokenKind    op;
      struct MiExpr *left;
      struct MiExpr *right;
    } binary;

    struct
    {
      MiExprList *items;
    } list;

    struct
    {
      MiExprList *items;
    } dict;

    struct
    {
      struct MiExpr *key;
      struct MiExpr *value;
    } pair;

    struct
    {
      struct MiScript *script;
    } block;

    struct
    {
      struct MiExpr *target;
      XSlice         member;
      MiToken        member_tok;
    } qual;

    struct
    {
      struct MiExpr *head;
      MiExprList    *args;
      unsigned int  argc;
    } command;
  } as;
} MiExpr;

typedef struct MiCommand
{
  MiExpr    *head;
  MiExprList *args;
  int       argc;

  /* Present only when this command originated from a typed `func` decl.
     The compiler still lowers it to cmd(name, params..., block) for runtime. */
  MiFuncSig *func_sig;

  /* include/import statement sugar:
     include "path" as name;
     import  "path" as name;
     The parser sets these fields and the compiler lowers it to:
     r0 = include(path)
     name = r0
     So 'as name' is not passed as runtime arguments. */
  bool      is_include_stmt;
  MiToken   include_alias_tok;
} MiCommand;

typedef struct MiScript
{
  MiCommandList *first;
  size_t         command_count;
} MiScript;

//----------------------------------------------------------
// Parser
//----------------------------------------------------------

typedef struct MiParseResult
{
  bool      ok;
  MiScript *script;       // Valid if of == true
  int       error_line;
  int       error_column;
  XSlice    error_message;
} MiParseResult;

void mi_parse_print_error(XSlice source, const MiParseResult* res);

/**
 * Parse a full Minima script (top-level or block).
 * @param source Pointer to UTF-8 source bytes.
 * @param source_len Length in bytes.
 * @param arena Arena used to allocate AST nodes.
 * @return MiParseResult with ok/script or error info.
 */
MiParseResult mi_parse_program(const char *source,
    size_t    source_len,
    XArena    *arena);

/**
 * Parse a full Minima script, optionally applying constant folding.
 * When fold_constants is true, the resulting AST is simplified in-place.
 */
MiParseResult mi_parse_program_ex(const char *source,
    size_t    source_len,
    XArena    *arena,
    bool      fold_constants);

//----------------------------------------------------------
// Debug print helpers
//----------------------------------------------------------

#if defined(_DEBUG) || defined(DEBUG)

/**
 * Pretty-print a full script AST to stdout.
 * @param script Script AST to print.
 */
void mi_ast_debug_print_script(const MiScript* script);

/**
 * Pretty-print a single command node.
 * @param cmd    Command AST node to print.
 * @param indent Initial indentation level.
 */
void mi_ast_debug_print_command(const MiCommand* cmd, int indent);

/**
 * Pretty-print an expression tree.
 * @param expr   Expression AST node to print.
 * @param indent Initial indentation level.
 */
void mi_ast_debug_print_expr(const MiExpr* expr, int indent);

#endif

#endif // MINIMA_PARSE_H
