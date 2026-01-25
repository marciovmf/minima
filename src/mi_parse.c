#include "mi_parse.h"
#include "mi_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

//----------------------------------------------------------
// Lexer (C-ish)
//----------------------------------------------------------


typedef struct MiLexer
{
  const char* src;
  size_t      length;
  size_t      pos;
  int         line;
  int         column;
} MiLexer;

static void s_lexer_init(MiLexer* lx, const char* src, size_t len)
{
  lx->src = src;
  lx->length = len;
  lx->pos = 0;
  lx->line = 1;
  lx->column = 1;
}

static bool s_lexer_is_eof(const MiLexer* lx)
{
  return lx->pos >= lx->length;
}

static char s_lexer_peek(const MiLexer* lx)
{
  if (s_lexer_is_eof(lx))
  {
    return '\0';
  }
  return lx->src[lx->pos];
}

static char s_lexer_peek_off(const MiLexer* lx, size_t off)
{
  size_t p = lx->pos + off;
  if (p >= lx->length)
  {
    return '\0';
  }
  return lx->src[p];
}

static char s_lexer_advance(MiLexer* lx)
{
  if (s_lexer_is_eof(lx))
  {
    return '\0';
  }

  char c = lx->src[lx->pos];
  lx->pos = lx->pos + 1;

  if (c == '\n')
  {
    lx->line = lx->line + 1;
    lx->column = 1;
  }
  else
  {
    lx->column = lx->column + 1;
  }

  return c;
}

static bool s_is_ident_start(char c)
{
  return (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c == '_');
}

static bool s_is_ident_part(char c)
{
  return s_is_ident_start(c) || (c >= '0' && c <= '9');
}

static MiToken s_make_token(MiTokenKind kind, const char* start, size_t length, int line, int column)
{
  MiToken tok;
  tok.kind = kind;
  tok.lexeme.ptr = start;
  tok.lexeme.length = length;
  tok.line = line;
  tok.column = column;
  return tok;
}

static MiToken s_make_error_token(const char* msg, int line, int column)
{
  MiToken tok;
  tok.kind = MI_TOK_ERROR;
  tok.lexeme.ptr = msg;
  tok.lexeme.length = strlen(msg);
  tok.line = line;
  tok.column = column;
  return tok;
}

static void s_lexer_skip_ws_and_comments(MiLexer* lx)
{
  for (;;)
  {
    char c = s_lexer_peek(lx);

    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
    {
      s_lexer_advance(lx);
      continue;
    }

    // line comment
    if (c == '/' && s_lexer_peek_off(lx, 1) == '/')
    {
      s_lexer_advance(lx);
      s_lexer_advance(lx);
      while (!s_lexer_is_eof(lx) && s_lexer_peek(lx) != '\n')
      {
        s_lexer_advance(lx);
      }
      continue;
    }

    // block comment
    if (c == '/' && s_lexer_peek_off(lx, 1) == '*')
    {
      s_lexer_advance(lx);
      s_lexer_advance(lx);
      while (!s_lexer_is_eof(lx))
      {
        if (s_lexer_peek(lx) == '*' && s_lexer_peek_off(lx, 1) == '/')
        {
          s_lexer_advance(lx);
          s_lexer_advance(lx);
          break;
        }
        s_lexer_advance(lx);
      }
      continue;
    }

    break;
  }
}

static XSlice s_lex_unescape_string_in_place(XSlice slice)
{
  char* r = (char*)slice.ptr;
  char* w = (char*)slice.ptr;
  const char* end = slice.ptr + slice.length;
  size_t len = 0;

  while (r < end)
  {
    if (r[0] == '\\' && r + 1 < end)
    {
      char e = r[1];
      if (e == 'n')
      {
        *w++ = '\n';
        r += 2;
        len++;
        continue;
      }
      if (e == 't')
      {
        *w++ = '\t';
        r += 2;
        len++;
        continue;
      }
      if (e == 'r')
      {
        *w++ = '\r';
        r += 2;
        len++;
        continue;
      }
      if (e == '\\' || e == '"')
      {
        *w++ = e;
        r += 2;
        len++;
        continue;
      }
    }

    *w++ = *r++;
    len++;
  }

  return x_slice_init(slice.ptr, len);
}

static MiToken s_lexer_next(MiLexer* lx)
{
  s_lexer_skip_ws_and_comments(lx);

  if (s_lexer_is_eof(lx))
  {
    return s_make_token(MI_TOK_EOF, lx->src + lx->pos, 0, lx->line, lx->column);
  }

  int line = lx->line;
  int column = lx->column;
  const char* start = lx->src + lx->pos;
  char c = s_lexer_advance(lx);

  // statement separator
  if (c == ';')
  {
    return s_make_token(MI_TOK_SEMICOLON, start, 1, line, column);
  }

  // punctuation
  switch (c)
  {
    case '(' : return s_make_token(MI_TOK_LPAREN, start, 1, line, column);
    case ')' : return s_make_token(MI_TOK_RPAREN, start, 1, line, column);
    case '{' : return s_make_token(MI_TOK_LBRACE, start, 1, line, column);
    case '}' : return s_make_token(MI_TOK_RBRACE, start, 1, line, column);
    case '[' : return s_make_token(MI_TOK_LBRACKET, start, 1, line, column);
    case ']' : return s_make_token(MI_TOK_RBRACKET, start, 1, line, column);
    case ',' : return s_make_token(MI_TOK_COMMA, start, 1, line, column);

    case ':' :
               if (s_lexer_peek(lx) == ':')
               {
                 s_lexer_advance(lx);
                 return s_make_token(MI_TOK_DOUBLE_COLON, start, 2, line, column);
               }
               return s_make_token(MI_TOK_COLON, start, 1, line, column);

    case '+' : return s_make_token(MI_TOK_PLUS, start, 1, line, column);
    case '-' : return s_make_token(MI_TOK_MINUS, start, 1, line, column);
    case '*' : return s_make_token(MI_TOK_STAR, start, 1, line, column);
    case '/' : return s_make_token(MI_TOK_SLASH, start, 1, line, column);

    case '=' :
               if (s_lexer_peek(lx) == '=')
               {
                 s_lexer_advance(lx);
                 return s_make_token(MI_TOK_EQEQ, start, 2, line, column);
               }
               return s_make_token(MI_TOK_EQ, start, 1, line, column);

    case '!' :
               if (s_lexer_peek(lx) == '=')
               {
                 s_lexer_advance(lx);
                 return s_make_token(MI_TOK_BANGEQ, start, 2, line, column);
               }
               return s_make_token(MI_TOK_NOT, start, 1, line, column);

    case '<' :
               if (s_lexer_peek(lx) == '=')
               {
                 s_lexer_advance(lx);
                 return s_make_token(MI_TOK_LTEQ, start, 2, line, column);
               }
               return s_make_token(MI_TOK_LT, start, 1, line, column);

    case '>' :
               if (s_lexer_peek(lx) == '=')
               {
                 s_lexer_advance(lx);
                 return s_make_token(MI_TOK_GTEQ, start, 2, line, column);
               }
               return s_make_token(MI_TOK_GT, start, 1, line, column);

    case '&' :
               if (s_lexer_peek(lx) == '&')
               {
                 s_lexer_advance(lx);
                 return s_make_token(MI_TOK_AND, start, 2, line, column);
               }
               return s_make_error_token("Unexpected '&'", line, column);

    case '|' :
               if (s_lexer_peek(lx) == '|')
               {
                 s_lexer_advance(lx);
                 return s_make_token(MI_TOK_OR, start, 2, line, column);
               }
               return s_make_error_token("Unexpected '|'", line, column);

    case '"':
               {
                 const char* s = lx->src + lx->pos;
                 int start_line = line;
                 int start_col = column;

                 while (!s_lexer_is_eof(lx) && s_lexer_peek(lx) != '"')
                 {
                   if (s_lexer_peek(lx) == '\\' && s_lexer_peek_off(lx, 1) != '\0')
                   {
                     s_lexer_advance(lx);
                     s_lexer_advance(lx);
                     continue;
                   }
                   s_lexer_advance(lx);
                 }

                 if (s_lexer_is_eof(lx))
                 {
                   return s_make_error_token("Unterminated string literal", start_line, start_col);
                 }

                 size_t len = (size_t)((lx->src + lx->pos) - s);
                 (void)s_lexer_advance(lx); // closing """

                 // Note: String lexem does not include quotes.
                 MiToken tok = s_make_token(MI_TOK_STRING, s, len, start_line, start_col);
                 return tok;
               }
  }

  // number
  if (c >= '0' && c <= '9')
  {
    bool is_float = false;
    while (s_lexer_peek(lx) >= '0' && s_lexer_peek(lx) <= '9')
    {
      s_lexer_advance(lx);
    }
    if (s_lexer_peek(lx) == '.' && (s_lexer_peek_off(lx, 1) >= '0' && s_lexer_peek_off(lx, 1) <= '9'))
    {
      is_float = true;
      s_lexer_advance(lx);
      while (s_lexer_peek(lx) >= '0' && s_lexer_peek(lx) <= '9')
      {
        s_lexer_advance(lx);
      }
    }
    size_t len = (size_t)((lx->src + lx->pos) - start);
    return s_make_token(is_float ? MI_TOK_FLOAT : MI_TOK_INT, start, len, line, column);
  }

  // identifier / keyword
  if (s_is_ident_start(c))
  {
    while (s_is_ident_part(s_lexer_peek(lx)))
    {
      s_lexer_advance(lx);
    }
    size_t len = (size_t)((lx->src + lx->pos) - start);
    XSlice s = x_slice_init(start, len);

    if (x_slice_eq_cstr(s, "func")) return s_make_token(MI_TOK_FUNC, start, len, line, column);
    if (x_slice_eq_cstr(s, "return")) return s_make_token(MI_TOK_RETURN, start, len, line, column);
    if (x_slice_eq_cstr(s, "let")) return s_make_token(MI_TOK_LET, start, len, line, column);
if (x_slice_eq_cstr(s, "if")) return s_make_token(MI_TOK_IF, start, len, line, column);
if (x_slice_eq_cstr(s, "else")) return s_make_token(MI_TOK_ELSE, start, len, line, column);
if (x_slice_eq_cstr(s, "while")) return s_make_token(MI_TOK_WHILE, start, len, line, column);
if (x_slice_eq_cstr(s, "foreach")) return s_make_token(MI_TOK_FOREACH, start, len, line, column);
    if (x_slice_eq_cstr(s, "true")) return s_make_token(MI_TOK_TRUE, start, len, line, column);
    if (x_slice_eq_cstr(s, "false")) return s_make_token(MI_TOK_FALSE, start, len, line, column);
    if (x_slice_eq_cstr(s, "void")) return s_make_token(MI_TOK_VOID, start, len, line, column);

    return s_make_token(MI_TOK_IDENTIFIER, start, len, line, column);
  }

  return s_make_error_token("Unexpected character", line, column);
}

//----------------------------------------------------------
// Parser (Seam A): emits current MiExpr / MiScript
//----------------------------------------------------------

typedef struct MiParser
{
  MiLexer  lx;
  XArena*  arena;
  MiToken  current;
  MiToken  previous;
  bool     had_error;

  int      error_line;
  int      error_column;
  XSlice   error_message;
} MiParser;

static MiTypeKind s_parse_type_name(MiParser* p, MiToken type_tok);
static MiFuncTypeSig* s_parse_func_type_sig(MiParser* p, MiToken func_tok);
static void s_parse_type_spec(MiParser* p, MiTypeKind* out_kind, MiFuncTypeSig** out_func_sig);
static void s_parser_set_error(MiParser* p, const char* msg, MiToken at)
{
  if (p->had_error)
  {
    return;
  }

  p->had_error = true;
  p->error_line = at.line;
  p->error_column = at.column;
  p->error_message = x_slice_init(msg, strlen(msg));
}

static MiToken s_parser_peek(MiParser* p)
{
  return p->current;
}

static MiToken s_parser_prev(MiParser* p)
{
  return p->previous;
}

static MiToken s_parser_advance(MiParser* p)
{
  p->previous = p->current;
  p->current = s_lexer_next(&p->lx);

  if (p->current.kind == MI_TOK_ERROR)
  {
    s_parser_set_error(p, (const char*)p->current.lexeme.ptr, p->current);
  }
  return p->previous;
}

static bool s_parser_match(MiParser* p, MiTokenKind kind)
{
  if (p->current.kind == kind)
  {
    (void)s_parser_advance(p);
    return true;
  }
  return false;
}

static bool s_parser_expect(MiParser* p, MiTokenKind kind, const char* msg)
{
  if (p->current.kind == kind)
  {
    (void)s_parser_advance(p);
    return true;
  }
  s_parser_set_error(p, msg, p->current);
  return false;
}

static MiExpr* s_new_expr(MiParser* p, MiExprKind kind, MiToken tok, bool can_fold)
{
  // Because we do not own the source buffer memory, we duplicate identifiers
  // and strings into the parser arena.
  if ((kind == MI_EXPR_STRING_LITERAL || kind == MI_EXPR_VAR) && tok.lexeme.ptr && tok.lexeme.length > 0)
  {
    char* owned = x_arena_slicedup(p->arena, tok.lexeme.ptr, tok.lexeme.length, true);
    if (!owned)
    {
      s_parser_set_error(p, "Out of memory", tok);
      return NULL;
    }
    tok.lexeme.ptr = owned;
  }

  MiExpr* e = (MiExpr*)x_arena_alloc_zero(p->arena, sizeof(MiExpr));
  if (!e)
  {
    s_parser_set_error(p, "Out of memory", tok);
    return NULL;
  }
  e->kind = kind;
  e->token = tok;
  e->can_fold = can_fold;
  return e;
}

static MiExprList* s_expr_list_append(MiParser* p, MiExprList* list, MiExpr* expr)
{
  MiExprList* node = (MiExprList*)x_arena_alloc_zero(p->arena, sizeof(MiExprList));
  if (!node)
  {
    s_parser_set_error(p, "Out of memory", p->current);
    return list;
  }
  node->expr = expr;
  node->next = NULL;

  if (!list)
  {
    return node;
  }

  MiExprList* it = list;
  while (it->next)
  {
    it = it->next;
  }
  it->next = node;
  return list;
}

static MiCommandList* s_command_list_append(MiParser* p, MiCommandList* list, MiCommand* cmd)
{
  MiCommandList* node = (MiCommandList*)x_arena_alloc_zero(p->arena, sizeof(MiCommandList));
  if (!node)
  {
    s_parser_set_error(p, "Out of memory", p->current);
    return list;
  }
  node->command = cmd;
  node->next = NULL;

  if (!list)
  {
    return node;
  }

  MiCommandList* it = list;
  while (it->next)
  {
    it = it->next;
  }
  it->next = node;
  return list;
}

static MiScript* s_new_script(MiParser* p)
{
  MiScript* s = (MiScript*)x_arena_alloc_zero(p->arena, sizeof(MiScript));
  if (!s)
  {
    s_parser_set_error(p, "Out of memory", p->current);
    return NULL;
  }
  return s;
}

static MiCommand* s_new_command(MiParser* p, MiExpr* head, int argc, MiExprList* args, MiToken at)
{
  MiCommand* c = (MiCommand*)x_arena_alloc_zero(p->arena, sizeof(MiCommand));
  if (!c)
  {
    s_parser_set_error(p, "Out of memory", at);
    return NULL;
  }
  c->head = head;
  c->argc = argc;
  c->args = args;
  return c;
}

//----------------------------------------------------------
// Expression parsing (precedence climbing)
//----------------------------------------------------------

static MiExpr* s_parse_expr(MiParser* p);
static MiCommand* s_parse_if_stmt(MiParser* p, MiToken if_tok);
static MiCommand* s_parse_while_stmt(MiParser* p, MiToken while_tok);
static MiCommand* s_parse_foreach_stmt(MiParser* p, MiToken foreach_tok);
static MiCommand* s_parse_block_stmt(MiParser* p, MiExpr* block_expr);
static MiExpr* s_parse_stmt_as_block_expr(MiParser* p);
static MiCommand* s_parse_stmt_command(MiParser* p);
static MiScript* s_parse_script(MiParser* p, bool stop_at_rbrace);

static MiExpr* s_ident_as_string(MiParser* p, MiToken ident)
{
  MiExpr* e = s_new_expr(p, MI_EXPR_STRING_LITERAL, ident, true);
  if (!e)
  {
    return NULL;
  }
  e->as.string_lit.value = e->token.lexeme;
  return e;
}


static MiToken s_fake_token(const char* cstr)
{
  MiToken t;
  memset(&t, 0, sizeof(t));
  t.kind = MI_TOK_STRING;
  t.lexeme.ptr = cstr;
  t.lexeme.length = strlen(cstr);
  t.line = 0;
  t.column = 0;
  return t;
}

static MiExpr* s_cstr_as_string(MiParser* p, const char* cstr)
{
  MiToken t = s_fake_token(cstr);
  MiExpr* e = s_new_expr(p, MI_EXPR_STRING_LITERAL, t, true);
  if (!e) return NULL;
  e->as.string_lit.value = e->token.lexeme;
  return e;
}

static MiExpr* s_ident_as_var(MiParser* p, MiToken ident)
{
  MiExpr* e = s_new_expr(p, MI_EXPR_VAR, ident, false);
  if (!e)
  {
    return NULL;
  }
  e->as.var.is_indirect = false;
  e->as.var.name = ident.lexeme;
  e->as.var.name_expr = NULL;
  return e;
}

static MiExpr* s_parse_primary(MiParser* p)
{
  MiToken tok = s_parser_peek(p);

  if (s_parser_match(p, MI_TOK_INT))
  {
    MiExpr* e = s_new_expr(p, MI_EXPR_INT_LITERAL, s_parser_prev(p), true);
    if (!e) return NULL;
    char buf[64];
    size_t n = tok.lexeme.length < sizeof(buf) - 1 ? tok.lexeme.length : sizeof(buf) - 1;
    memcpy(buf, tok.lexeme.ptr, n);
    buf[n] = 0;
    e->as.int_lit.value = (int64_t)strtoll(buf, NULL, 10);
    return e;
  }

  if (s_parser_match(p, MI_TOK_FLOAT))
  {
    MiExpr* e = s_new_expr(p, MI_EXPR_FLOAT_LITERAL, s_parser_prev(p), true);
    if (!e) return NULL;
    char buf[64];
    size_t n = tok.lexeme.length < sizeof(buf) - 1 ? tok.lexeme.length : sizeof(buf) - 1;
    memcpy(buf, tok.lexeme.ptr, n);
    buf[n] = 0;
    e->as.float_lit.value = strtod(buf, NULL);
    return e;
  }

  if (s_parser_match(p, MI_TOK_STRING))
  {
    MiToken st = s_parser_prev(p);
    MiExpr* e = s_new_expr(p, MI_EXPR_STRING_LITERAL, st, true);
    if (!e) return NULL;
    e->as.string_lit.value = e->token.lexeme;
    return e;
  }

  if (s_parser_match(p, MI_TOK_TRUE) || s_parser_match(p, MI_TOK_FALSE))
  {
    MiToken bt = s_parser_prev(p);
    MiExpr* e = s_new_expr(p, MI_EXPR_BOOL_LITERAL, bt, true);
    if (!e) return NULL;
    e->as.bool_lit.value = (bt.kind == MI_TOK_TRUE);
    return e;
  }

  if (s_parser_match(p, MI_TOK_VOID))
  {
    MiToken vt = s_parser_prev(p);
    MiExpr* e = s_new_expr(p, MI_EXPR_VOID_LITERAL, vt, true);
    if (!e) return NULL;
    return e;
  }

  
  // list/dict literal:
  //   list: '[' [expr (',' expr)* [',']] ']'
  //   dict: '[' [pair (',' pair)* [',']] ']'
  //   empty dict: '[:]'  (disambiguates from empty list '[]')
  //   pair separators: ':' or '='  (both accepted)
  if (s_parser_match(p, MI_TOK_LBRACKET))
  {
    MiToken lt = s_parser_prev(p);

    // Special empty dict marker: [:]
    if (s_parser_match(p, MI_TOK_COLON))
    {
      if (!s_parser_expect(p, MI_TOK_RBRACKET, "Expected ']' after '[:'"))
      {
        return NULL;
      }

      MiExpr* e = s_new_expr(p, MI_EXPR_DICT, lt, false);
      if (!e) return NULL;
      e->as.dict.items = NULL;
      return e;
    }

    // Empty list: []
    if (s_parser_match(p, MI_TOK_RBRACKET))
    {
      MiExpr* e = s_new_expr(p, MI_EXPR_LIST, lt, false);
      if (!e) return NULL;
      e->as.list.items = NULL;
      return e;
    }

    // Parse first expression, then decide list vs dict based on next token.
    MiExpr* first = s_parse_expr(p);
    if (!first) return NULL;

    // Dict if the next token is ':' or '='.
    if (p->current.kind == MI_TOK_COLON || p->current.kind == MI_TOK_EQ)
    {
      MiExprList* entries = NULL;

      for (;;)
      {
        MiExpr* key = first;
        MiToken sep = p->current;
        if (!(s_parser_match(p, MI_TOK_COLON) || s_parser_match(p, MI_TOK_EQ)))
        {
          s_parser_set_error(p, "Expected ':' or '=' in dict entry", p->current);
          return NULL;
        }

        MiExpr* value = s_parse_expr(p);
        if (!value) return NULL;

        MiExpr* pair = s_new_expr(p, MI_EXPR_PAIR, sep, false);
        if (!pair) return NULL;
        pair->as.pair.key = key;
        pair->as.pair.value = value;

        entries = s_expr_list_append(p, entries, pair);

        if (s_parser_match(p, MI_TOK_COMMA))
        {
          // allow trailing comma
          if (s_parser_match(p, MI_TOK_RBRACKET))
          {
            break;
          }
          first = s_parse_expr(p);
          if (!first) return NULL;
          continue;
        }

        if (!s_parser_expect(p, MI_TOK_RBRACKET, "Expected ']' to close dict literal"))
        {
          return NULL;
        }
        break;
      }

      MiExpr* e = s_new_expr(p, MI_EXPR_DICT, lt, false);
      if (!e) return NULL;
      e->as.dict.items = entries;
      return e;
    }

    // Otherwise: list literal (we already parsed first).
    {
      MiExprList* items = NULL;
      items = s_expr_list_append(p, items, first);

      if (s_parser_match(p, MI_TOK_COMMA))
      {
        // allow trailing comma
        if (!s_parser_match(p, MI_TOK_RBRACKET))
        {
          for (;;)
          {
            MiExpr* item = s_parse_expr(p);
            if (!item) return NULL;
            items = s_expr_list_append(p, items, item);

            if (s_parser_match(p, MI_TOK_COMMA))
            {
              if (s_parser_match(p, MI_TOK_RBRACKET))
              {
                break;
              }
              continue;
            }

            if (!s_parser_expect(p, MI_TOK_RBRACKET, "Expected ']' to close list literal"))
            {
              return NULL;
            }
            break;
          }
        }
      }
      else
      {
        if (!s_parser_expect(p, MI_TOK_RBRACKET, "Expected ']' to close list literal"))
        {
          return NULL;
        }
      }

      MiExpr* e = s_new_expr(p, MI_EXPR_LIST, lt, false);
      if (!e) return NULL;
      e->as.list.items = items;
      return e;
    }
  }

  if (s_parser_match(p, MI_TOK_IDENTIFIER))
  {
    MiToken ident = s_parser_prev(p);
    /* If the identifier is immediately followed by '(', treat it as a command head.
       This preserves Minima's "command head must be string" rule for calls like foo(...).
NOTE: Calling a block-valued variable with x() will require a different syntax;
x() currently always resolves as a command call head.
*/
    if (s_parser_peek(p).kind == MI_TOK_LPAREN)
    {
      return s_ident_as_string(p, ident);
    }

    return s_ident_as_var(p, ident);
  }

  if (s_parser_match(p, MI_TOK_LPAREN))
  {
    MiExpr* e = s_parse_expr(p);
    if (!e) return NULL;
    if (!s_parser_expect(p, MI_TOK_RPAREN, "Expected ')'")) return NULL;
    return e;
  }

  if (s_parser_match(p, MI_TOK_LBRACE))
  {
    MiToken bt = s_parser_prev(p);
    MiScript* inner = s_parse_script(p, true);
    if (!inner) return NULL;
    if (!s_parser_expect(p, MI_TOK_RBRACE, "Expected '}'")) return NULL;

    MiExpr* e = s_new_expr(p, MI_EXPR_BLOCK, bt, false);
    if (!e) return NULL;
    e->as.block.script = inner;
    return e;
  }

  s_parser_set_error(p, "Expected expression", tok);
  return NULL;
}

static MiExpr* s_parse_call(MiParser* p)
{
  MiExpr* expr = s_parse_primary(p);
  if (!expr) return NULL;

  // call: primary '(' args ')'
  for (;;)
  {
    // function-style call: expr '(' args ')'
    if (s_parser_match(p, MI_TOK_LPAREN))
    {
      MiToken call_tok = s_parser_prev(p);
      MiExprList* args = NULL;
      unsigned int argc = 0;

      if (p->current.kind != MI_TOK_RPAREN)
      {
        for (;;)
        {
          MiExpr* a = s_parse_expr(p);
          if (!a) return NULL;
          args = s_expr_list_append(p, args, a);
          argc = argc + 1;

          if (!s_parser_match(p, MI_TOK_COMMA))
          {
            break;
          }
        }
      }

      if (!s_parser_expect(p, MI_TOK_RPAREN, "Expected ')' after call arguments"))
      {
        return NULL;
      }

      MiExpr* call = s_new_expr(p, MI_EXPR_COMMAND, call_tok, false);
      if (!call) return NULL;
      call->as.command.head = expr;
      call->as.command.args = args;
      call->as.command.argc = argc;
      expr = call;
      continue;
    }

    // indexing: expr '[' index ']'
    if (s_parser_match(p, MI_TOK_LBRACKET))
    {
      MiToken it = s_parser_prev(p);

      MiExpr* index = s_parse_expr(p);
      if (!index) return NULL;

      if (!s_parser_expect(p, MI_TOK_RBRACKET, "Expected ']' after index expression"))
      {
        return NULL;
      }

      MiExpr* ix = s_new_expr(p, MI_EXPR_INDEX, it, false);
      if (!ix) return NULL;
      ix->as.index.target = expr;
      ix->as.index.index = index;
      expr = ix;
      continue;
    }

    break;
  }

  return expr;
}

static MiExpr* s_parse_unary(MiParser* p)
{
  if (s_parser_match(p, MI_TOK_NOT) || s_parser_match(p, MI_TOK_MINUS) || s_parser_match(p, MI_TOK_PLUS))
  {
    MiToken op = s_parser_prev(p);
    MiExpr* rhs = s_parse_unary(p);
    if (!rhs) return NULL;

    MiExpr* e = s_new_expr(p, MI_EXPR_UNARY, op, false);
    if (!e) return NULL;
    e->as.unary.op = op.kind;
    e->as.unary.expr = rhs;
    return e;
  }

  return s_parse_call(p);
}

static int s_prec(MiTokenKind k)
{
  switch (k)
  {
    case MI_TOK_STAR:
    case MI_TOK_SLASH:
      return 6;
    case MI_TOK_PLUS:
    case MI_TOK_MINUS:
      return 5;
    case MI_TOK_LT:
    case MI_TOK_LTEQ:
    case MI_TOK_GT:
    case MI_TOK_GTEQ:
      return 4;
    case MI_TOK_EQEQ:
    case MI_TOK_BANGEQ:
      return 3;
    case MI_TOK_AND:
      return 2;
    case MI_TOK_OR:
      return 1;
    default:
      return 0;
  }
}

static MiExpr* s_parse_binary_rhs(MiParser* p, MiExpr* left, int min_prec)
{
  for (;;)
  {
    MiToken op = s_parser_peek(p);
    int prec = s_prec(op.kind);
    if (prec < min_prec)
    {
      break;
    }

    (void)s_parser_advance(p);
    MiExpr* right = s_parse_unary(p);
    if (!right) return NULL;

    MiToken next = s_parser_peek(p);
    int next_prec = s_prec(next.kind);
    if (next_prec > prec)
    {
      right = s_parse_binary_rhs(p, right, prec + 1);
      if (!right) return NULL;
    }

    MiExpr* bin = s_new_expr(p, MI_EXPR_BINARY, op, false);
    if (!bin) return NULL;
    bin->as.binary.op = op.kind;
    bin->as.binary.left = left;
    bin->as.binary.right = right;
    left = bin;
  }

  return left;
}

static MiExpr* s_parse_expr(MiParser* p)
{
  MiExpr* left = s_parse_unary(p);
  if (!left) return NULL;
  return s_parse_binary_rhs(p, left, 1);
}

//----------------------------------------------------------
// Statements -> MiCommand (Seam A lowering)
//----------------------------------------------------------

static MiCommand* s_stmt_to_command(MiParser* p, MiExpr* expr)
{
  if (!expr)
  {
    return NULL;
  }

  if (expr->kind != MI_EXPR_COMMAND)
  {
    s_parser_set_error(p, "Expected a function call statement", expr->token);
    return NULL;
  }

  return s_new_command(p, expr->as.command.head, (int)expr->as.command.argc, expr->as.command.args, expr->token);
}

static MiCommand* s_parse_func_decl(MiParser* p)
{
  MiToken func_tok = s_parser_prev(p);

  if (!s_parser_expect(p, MI_TOK_IDENTIFIER, "Expected function name after 'func'"))
  {
    return NULL;
  }
  MiToken name_tok = s_parser_prev(p);

  if (!s_parser_expect(p, MI_TOK_LPAREN, "Expected '(' after function name"))
  {
    return NULL;
  }

  // Allocate and fill a signature for typechecking.
  MiFuncSig* sig = (MiFuncSig*)x_arena_alloc_zero(p->arena, sizeof(MiFuncSig));
  if (!sig)
  {
    s_parser_set_error(p, "Out of memory", p->current);
    return NULL;
  }
  sig->name_tok = name_tok;
  sig->name = name_tok.lexeme;
  sig->params = NULL;
  sig->param_count = 0;
  sig->ret_tok = func_tok;
  sig->ret_type = MI_TYPE_VOID;

  MiExprList* args = NULL;
  int argc = 0;

  // arg0: function name (string)
  MiExpr* name_expr = s_new_expr(p, MI_EXPR_STRING_LITERAL, name_tok, true);
  if (!name_expr) return NULL;
  name_expr->as.string_lit.value = name_expr->token.lexeme;
  args = s_expr_list_append(p, args, name_expr);
  argc = argc + 1;

  if (p->current.kind != MI_TOK_RPAREN)
  {
    for (;;)
    {
      if (!s_parser_expect(p, MI_TOK_IDENTIFIER, "Expected parameter name"))
      {
        return NULL;
      }
      MiToken pt = s_parser_prev(p);

      // Optional :type
      MiTypeKind param_type = MI_TYPE_ANY;
MiFuncTypeSig* param_func_sig = NULL;
MiToken type_tok = pt;
if (s_parser_match(p, MI_TOK_COLON))
{
  type_tok = s_parser_peek(p);
  s_parse_type_spec(p, &param_type, &param_func_sig);
  if (p->had_error)
  {
    return NULL;
  }
}
if (p->had_error)
        {
          return NULL;
        }
      }

      // Record parameter in signature.
      MiFuncParam* params_new = (MiFuncParam*)x_arena_alloc_zero(p->arena, sizeof(MiFuncParam) * (size_t)(sig->param_count + 1));
      if (!params_new)
      {
        s_parser_set_error(p, "Out of memory", p->current);
        return NULL;
      }
      for (int i = 0; i < sig->param_count; i++)
      {
        params_new[i] = sig->params[i];
      }
      params_new[sig->param_count].name_tok = pt;
      params_new[sig->param_count].name = pt.lexeme;
      params_new[sig->param_count].type_tok = type_tok;
      params_new[sig->param_count].type = param_type;
      params_new[sig->param_count].func_sig = param_func_sig;
      sig->params = params_new;
      sig->param_count = sig->param_count + 1;

      MiExpr* pe = s_new_expr(p, MI_EXPR_STRING_LITERAL, pt, true);
      if (!pe) return NULL;
      pe->as.string_lit.value = pe->token.lexeme;
      args = s_expr_list_append(p, args, pe);
      argc = argc + 1;

      if (s_parser_match(p, MI_TOK_COMMA))
      {
        continue;
      }
      break;
    }
  }

  if (!s_parser_expect(p, MI_TOK_RPAREN, "Expected ')' after parameters"))
  {
    return NULL;
  }

  // Optional return type: -> Type
  if (s_parser_match(p, MI_TOK_MINUS))
  {
    if (!s_parser_expect(p, MI_TOK_GT, "Expected '>' after '-' in return type"))
    {
      return NULL;
    }

    if (!(p->current.kind == MI_TOK_IDENTIFIER || p->current.kind == MI_TOK_VOID))
    {
      s_parser_set_error(p, "Expected return type name after '->'", p->current);
      return NULL;
    }
    MiToken rt = s_parser_advance(p);
    sig->ret_tok = rt;
    sig->ret_type = s_parse_type_name(p, rt);
    if (p->had_error)
    {
      return NULL;
    }
  }

  // body block
  if (!s_parser_expect(p, MI_TOK_LBRACE, "Expected '{' to start function body"))
  {
    return NULL;
  }

  MiScript* body = s_parse_script(p, true);
  if (!body) return NULL;

  if (!s_parser_expect(p, MI_TOK_RBRACE, "Expected '}' after function body"))
  {
    return NULL;
  }

  MiExpr* block = s_new_expr(p, MI_EXPR_BLOCK, func_tok, false);
  if (!block) return NULL;
  block->as.block.script = body;
  args = s_expr_list_append(p, args, block);
  argc = argc + 1;

  // cmd("name", "a", "b", { ... })
  MiToken cmd_tok;
  cmd_tok.kind = MI_TOK_IDENTIFIER;
  cmd_tok.lexeme = x_slice_init("cmd", 3);
  cmd_tok.line = func_tok.line;
  cmd_tok.column = func_tok.column;

  MiExpr* head = s_new_expr(p, MI_EXPR_STRING_LITERAL, cmd_tok, true);
  if (!head) return NULL;
  head->as.string_lit.value = head->token.lexeme;

  MiCommand* out = s_new_command(p, head, argc, args, func_tok);
  if (!out) return NULL;
  out->func_sig = sig;
  return out;
}

static MiTypeKind s_parse_type_name(MiParser* p, MiToken type_tok)
{
  XSlice s = type_tok.lexeme;

  // Accept keyword void.
  if (type_tok.kind == MI_TOK_VOID || (s.length == 4 && memcmp(s.ptr, "void", 4) == 0))
  {
    return MI_TYPE_VOID;
  }


  // Accept keyword func.
  if (type_tok.kind == MI_TOK_FUNC || (s.length == 4 && memcmp(s.ptr, "func", 4) == 0))
  {
    return MI_TYPE_FUNC;
  }

  // Builtin names are plain identifiers.
  if (s.length == 3 && memcmp(s.ptr, "int", 3) == 0) return MI_TYPE_INT;
  if (s.length == 5 && memcmp(s.ptr, "float", 5) == 0) return MI_TYPE_FLOAT;
  if (s.length == 4 && memcmp(s.ptr, "bool", 4) == 0) return MI_TYPE_BOOL;
  if (s.length == 6 && memcmp(s.ptr, "string", 6) == 0) return MI_TYPE_STRING;
  if (s.length == 4 && memcmp(s.ptr, "list", 4) == 0) return MI_TYPE_LIST;
  if (s.length == 4 && memcmp(s.ptr, "dict", 4) == 0) return MI_TYPE_DICT;
  if (s.length == 5 && memcmp(s.ptr, "block", 5) == 0) return MI_TYPE_BLOCK;
  if (s.length == 3 && memcmp(s.ptr, "any", 3) == 0) return MI_TYPE_ANY;

  s_parser_set_error(p, "Unknown type name", type_tok);
  return MI_TYPE_ANY;
}

static MiFuncTypeSig* s_parse_func_type_sig(MiParser* p, MiToken func_tok)
{
  /* func with signature must have '(' */
  if (p->current.kind != MI_TOK_LPAREN)
  {
    return NULL;
  }

  MiFuncTypeSig* sig = (MiFuncTypeSig*)x_arena_alloc_zero(p->arena, sizeof(MiFuncTypeSig));
  if (!sig)
  {
    s_parser_set_error(p, "Out of memory", func_tok);
    return NULL;
  }

  sig->func_tok = func_tok;
  sig->lparen_tok = s_parser_advance(p); /* consume '(' */

  /* Parse param types (can be empty). */
  MiTypeKind* tmp = NULL;
  int tmp_count = 0;

  if (p->current.kind != MI_TOK_RPAREN)
  {
    while (true)
    {
      if (!(p->current.kind == MI_TOK_IDENTIFIER || p->current.kind == MI_TOK_VOID || p->current.kind == MI_TOK_FUNC))
      {
        s_parser_set_error(p, "Expected type name in func signature", p->current);
        return sig;
      }

      MiTypeKind t = s_parse_type_name(p, s_parser_advance(p));

      MiTypeKind* grown = (MiTypeKind*)x_arena_alloc(p->arena, sizeof(MiTypeKind) * (size_t)(tmp_count + 1));
      if (!grown)
      {
        s_parser_set_error(p, "Out of memory", func_tok);
        return sig;
      }
      if (tmp_count > 0 && tmp)
      {
        memcpy(grown, tmp, sizeof(MiTypeKind) * (size_t)tmp_count);
      }
      grown[tmp_count] = t;
      tmp = grown;
      tmp_count++;

      if (!s_parser_match(p, MI_TOK_COMMA))
      {
        break;
      }
    }
  }

  sig->rparen_tok = s_parser_expect(p, MI_TOK_RPAREN, "Expected ')' after func signature parameter list");

  sig->param_types = tmp;
  sig->param_count = tmp_count;

  /* Optional -> return type, default void. */
  sig->ret_type = MI_TYPE_VOID;
  sig->ret_tok = sig->rparen_tok;
  if (s_parser_match(p, MI_TOK_MINUS) && s_parser_match(p, MI_TOK_GT))
  {
    if (!(p->current.kind == MI_TOK_IDENTIFIER || p->current.kind == MI_TOK_VOID || p->current.kind == MI_TOK_FUNC))
    {
      s_parser_set_error(p, "Expected return type after '->' in func type", p->current);
      return sig;
    }
    sig->ret_tok = s_parser_advance(p);
    sig->ret_type = s_parse_type_name(p, sig->ret_tok);
  }

  return sig;
}

static void s_parse_type_spec(MiParser* p, MiTypeKind* out_kind, MiFuncTypeSig** out_func_sig)
{
  *out_kind = MI_TYPE_ANY;
  *out_func_sig = NULL;

  if (p->current.kind == MI_TOK_FUNC)
  {
    MiToken func_tok = s_parser_advance(p);
    *out_kind = MI_TYPE_FUNC;
    *out_func_sig = s_parse_func_type_sig(p, func_tok);
    return;
  }

  if (p->current.kind == MI_TOK_VOID || p->current.kind == MI_TOK_IDENTIFIER)
  {
    MiToken t = s_parser_advance(p);
    *out_kind = s_parse_type_name(p, t);
    return;
  }

  s_parser_set_error(p, "Expected type name", p->current);
}

static MiCommand* s_parse_return_stmt(MiParser* p)
{
  MiToken rt = s_parser_prev(p);
  MiExprList* args = NULL;
  int argc = 0;

  if (p->current.kind != MI_TOK_SEMICOLON)
  {
    MiExpr* e = s_parse_expr(p);
    if (!e) return NULL;
    args = s_expr_list_append(p, args, e);
    argc = 1;
  }

  if (!s_parser_expect(p, MI_TOK_SEMICOLON, "Expected ';' after return"))
  {
    return NULL;
  }

  MiToken head_tok;
  head_tok.kind = MI_TOK_IDENTIFIER;
  head_tok.lexeme = x_slice_init("return", 6);
  head_tok.line = rt.line;
  head_tok.column = rt.column;

  MiExpr* head = s_new_expr(p, MI_EXPR_STRING_LITERAL, head_tok, true);
  if (!head) return NULL;
  head->as.string_lit.value = head->token.lexeme;

  return s_new_command(p, head, argc, args, rt);
}

static MiCommand* s_parse_assignment_stmt(MiParser* p, MiExpr* lhs)
{
  MiToken eq = s_parser_prev(p);

  MiExpr* lvalue = NULL;

  if (!lhs)
  {
    s_parser_set_error(p, "Assignment target is missing", eq);
    return NULL;
  }

  if (lhs->kind == MI_EXPR_VAR && !lhs->as.var.is_indirect)
  {
    // bare name assignment: lower to set("name", rhs)
    MiExpr* name = s_new_expr(p, MI_EXPR_STRING_LITERAL, lhs->token, true);
    if (!name) return NULL;
    name->as.string_lit.value = lhs->as.var.name;
    lvalue = name;
  }
  else if (lhs->kind == MI_EXPR_INDEX)
  {
    // indexed assignment: lower to set(<index-expr>, rhs)
    lvalue = lhs;
  }
  else
  {
    s_parser_set_error(p, "Invalid assignment target", eq);
    return NULL;
  }

  MiExpr* rhs = s_parse_expr(p);
  if (!rhs) return NULL;

  if (!s_parser_expect(p, MI_TOK_SEMICOLON, "Expected ';' after assignment"))
  {
    return NULL;
  }

  MiExprList* args = NULL;
  int argc = 0;

  // arg0: lvalue (string name or index expression)
  args = s_expr_list_append(p, args, lvalue);
  args = s_expr_list_append(p, args, rhs);
  argc = 2;

  MiToken head_tok;
  head_tok.kind = MI_TOK_IDENTIFIER;
  head_tok.lexeme = x_slice_init("set", 3);
  head_tok.line = eq.line;
  head_tok.column = eq.column;

  MiExpr* head = s_new_expr(p, MI_EXPR_STRING_LITERAL, head_tok, true);
  if (!head) return NULL;
  head->as.string_lit.value = head->token.lexeme;

  return s_new_command(p, head, argc, args, eq);
}


static MiCommand* s_parse_block_stmt(MiParser* p, MiExpr* block_expr)
{
  // A standalone { ... } block is a statement; lower it to a call of the block.
  // This matches existing "call block" semantics in the VM/compiler.
  MiExprList* args = NULL;
  args = s_expr_list_append(p, args, block_expr);

  MiExpr* head = s_cstr_as_string(p, "call");
  if (!head) return NULL;

  return s_new_command(p, head, 1, args, block_expr->token);
}

static MiExpr* s_parse_stmt_as_block_expr(MiParser* p)
{
  // If the next token starts a literal block, parse it directly as an expression.
  if (s_parser_peek(p).kind == MI_TOK_LBRACE)
  {
    return s_parse_primary(p); // primary() handles { script }
  }

  // Otherwise parse exactly one statement command, and wrap it into a block.
  MiCommand* one = s_parse_stmt_command(p);
  if (!one) return NULL;

  MiScript* scr = s_new_script(p);
  if (!scr) return NULL;

  MiCommandList* node = (MiCommandList*)x_arena_alloc(p->arena, sizeof(MiCommandList));
  if (!node) return NULL;
  node->command = one;
  node->next = NULL;

  scr->first = node;
  scr->command_count = 1;

    MiToken bt = one->head ? one->head->token : s_fake_token("{");
  MiExpr* blk = s_new_expr(p, MI_EXPR_BLOCK, bt, false);
  if (!blk) return NULL;
  blk->as.block.script = scr;
  return blk;
}

static MiCommand* s_parse_if_stmt(MiParser* p, MiToken if_tok)
{
  // Lowering target expected by compiler special-form:
  // if :: <cond> <then_block> ("elseif" <cond> <block>)* ("else" <block>)?
  if (!s_parser_expect(p, MI_TOK_LPAREN, "Expected '(' after 'if'")) return NULL;
  MiExpr* cond = s_parse_expr(p);
  if (!cond) return NULL;
  if (!s_parser_expect(p, MI_TOK_RPAREN, "Expected ')' after if condition")) return NULL;

  MiExpr* then_blk = s_parse_stmt_as_block_expr(p);
  if (!then_blk) return NULL;

  MiExprList* args = NULL;
  int argc = 0;

  args = s_expr_list_append(p, args, cond);     argc++;
  args = s_expr_list_append(p, args, then_blk); argc++;

  // Parse an else chain. "else if" is not sugar: it's else + statement, but
  // the compiler wants it flattened as "elseif" legs.
  while (s_parser_match(p, MI_TOK_ELSE))
  {
    if (s_parser_match(p, MI_TOK_IF))
    {
      MiToken elseif_tok = s_parser_prev(p); // token of "if" after else
      // Parse the nested if leg and append as: "elseif", <cond>, <block>
      if (!s_parser_expect(p, MI_TOK_LPAREN, "Expected '(' after 'if'")) return NULL;
      MiExpr* c2 = s_parse_expr(p);
      if (!c2) return NULL;
      if (!s_parser_expect(p, MI_TOK_RPAREN, "Expected ')' after if condition")) return NULL;

      MiExpr* b2 = s_parse_stmt_as_block_expr(p);
      if (!b2) return NULL;

      MiExpr* kw = s_cstr_as_string(p, "elseif");
      if (!kw) return NULL;

      args = s_expr_list_append(p, args, kw); argc++;
      args = s_expr_list_append(p, args, c2); argc++;
      args = s_expr_list_append(p, args, b2); argc++;

      // Continue loop: if the nested leg has an else, it will appear next in the token stream.
      // We do NOT consume it here; the while loop will.
      (void)elseif_tok;
      continue;
    }

    // else <stmt>
    MiExpr* else_blk = s_parse_stmt_as_block_expr(p);
    if (!else_blk) return NULL;

    MiExpr* kw = s_cstr_as_string(p, "else");
    if (!kw) return NULL;

    args = s_expr_list_append(p, args, kw);       argc++;
    args = s_expr_list_append(p, args, else_blk); argc++;
    break; // else terminates the chain
  }

  MiExpr* head = s_cstr_as_string(p, "if");
  if (!head) return NULL;

  return s_new_command(p, head, argc, args, if_tok);
}

static MiCommand* s_parse_while_stmt(MiParser* p, MiToken while_tok)
{
  if (!s_parser_expect(p, MI_TOK_LPAREN, "Expected '(' after 'while'")) return NULL;
  MiExpr* cond = s_parse_expr(p);
  if (!cond) return NULL;
  if (!s_parser_expect(p, MI_TOK_RPAREN, "Expected ')' after while condition")) return NULL;

  MiExpr* body = s_parse_stmt_as_block_expr(p);
  if (!body) return NULL;

  MiExprList* args = NULL;
  args = s_expr_list_append(p, args, cond);
  args = s_expr_list_append(p, args, body);

  MiExpr* head = s_cstr_as_string(p, "while");
  if (!head) return NULL;

  return s_new_command(p, head, 2, args, while_tok);
}

static MiCommand* s_parse_foreach_stmt(MiParser* p, MiToken foreach_tok)
{
  if (!s_parser_expect(p, MI_TOK_LPAREN, "Expected '(' after 'foreach'")) return NULL;

  if (!s_parser_expect(p, MI_TOK_IDENTIFIER, "Expected loop variable name in foreach(...)"))
  {
    return NULL;
  }
  MiToken var_tok = s_parser_prev(p);

  if (!s_parser_expect(p, MI_TOK_COMMA, "Expected ',' after foreach variable")) return NULL;

  MiExpr* list_expr = s_parse_expr(p);
  if (!list_expr) return NULL;

  if (!s_parser_expect(p, MI_TOK_RPAREN, "Expected ')' after foreach header")) return NULL;

  // foreach requires a literal body block (compiler expects a block expr)
  if (!s_parser_expect(p, MI_TOK_LBRACE, "Expected '{' to start foreach body")) return NULL;
  // parse_primary expects we've already consumed '{'? It matches on LBRACE.
  // We'll reconstruct: step back isn't possible, so manually parse inner script here.
  MiToken bt = s_parser_prev(p);
  MiScript* inner = s_parse_script(p, true);
  if (!inner) return NULL;
  if (!s_parser_expect(p, MI_TOK_RBRACE, "Expected '}' after foreach body")) return NULL;

  MiExpr* body = s_new_expr(p, MI_EXPR_BLOCK, bt, false);
  if (!body) return NULL;
  body->as.block.script = inner;

  MiExpr* varname = s_ident_as_string(p, var_tok);
  if (!varname) return NULL;

  MiExprList* args = NULL;
  args = s_expr_list_append(p, args, varname);
  args = s_expr_list_append(p, args, list_expr);
  args = s_expr_list_append(p, args, body);

  MiExpr* head = s_cstr_as_string(p, "foreach");
  if (!head) return NULL;

  return s_new_command(p, head, 3, args, foreach_tok);
}

static MiCommand* s_parse_stmt_command(MiParser* p)
{
  MiToken tok = s_parser_peek(p);

  if (tok.kind == MI_TOK_LBRACE)
  {
    MiExpr* blk = s_parse_primary(p);
    if (!blk) return NULL;
    return s_parse_block_stmt(p, blk);
  }

  if (s_parser_match(p, MI_TOK_IF))
  {
    return s_parse_if_stmt(p, s_parser_prev(p));
  }

  if (s_parser_match(p, MI_TOK_WHILE))
  {
    return s_parse_while_stmt(p, s_parser_prev(p));
  }

  if (s_parser_match(p, MI_TOK_FOREACH))
  {
    return s_parse_foreach_stmt(p, s_parser_prev(p));
  }

  if (s_parser_match(p, MI_TOK_FUNC))
  {
    return s_parse_func_decl(p);
  }

  if (s_parser_match(p, MI_TOK_RETURN))
  {
    return s_parse_return_stmt(p);
  }

  if (s_parser_match(p, MI_TOK_LET))
  {
    if (!s_parser_expect(p, MI_TOK_IDENTIFIER, "Expected identifier after 'let'"))
    {
      return NULL;
    }
    MiExpr* lhs = s_ident_as_var(p, s_parser_prev(p));
    if (!lhs) return NULL;
    if (!s_parser_expect(p, MI_TOK_EQ, "Expected '=' after identifier"))
    {
      return NULL;
    }
    return s_parse_assignment_stmt(p, lhs);
  }

  // expression statement or assignment
  MiExpr* expr = s_parse_expr(p);
  if (!expr) return NULL;

  if (s_parser_match(p, MI_TOK_EQ))
  {
    return s_parse_assignment_stmt(p, expr);
  }

  if (!s_parser_expect(p, MI_TOK_SEMICOLON, "Expected ';' after statement"))
  {
    return NULL;
  }

  return s_stmt_to_command(p, expr);
}

static MiScript* s_parse_script(MiParser* p, bool stop_at_rbrace)
{
  MiScript* scr = s_new_script(p);
  if (!scr)
  {
    return NULL;
  }

  MiCommandList* list = NULL;
  size_t count = 0;

  while (!p->had_error)
  {
    MiToken tok = s_parser_peek(p);

    if (tok.kind == MI_TOK_EOF)
    {
      break;
    }

    if (stop_at_rbrace && tok.kind == MI_TOK_RBRACE)
    {
      break;
    }

    // Allow stray semicolons as empty statements.
    if (tok.kind == MI_TOK_SEMICOLON)
    {
      (void)s_parser_advance(p);
      continue;
    }

    // Parse exactly one statement and append.
    MiCommand* cmd = s_parse_stmt_command(p);
    if (!cmd)
    {
      break;
    }

    list = s_command_list_append(p, list, cmd);
    count = count + 1;
  }

  scr->first = list;
  scr->command_count = count;
  return scr;
}

//----------------------------------------------------------
// Public API
//----------------------------------------------------------

MiParseResult mi_parse_program(const char* source, size_t length, XArena* arena)
{
  return mi_parse_program_ex(source, length, arena, true);
}

MiParseResult mi_parse_program_ex(const char* source, size_t length, XArena* arena, bool _)
{
  (void)_; // legacy arg from old parser

  MiParseResult out;
  memset(&out, 0, sizeof(out));
  out.ok = false;

  if (!source || !arena)
  {
    out.error_message = x_slice_init("Invalid arguments", 17);
    return out;
  }

  MiParser p;
  memset(&p, 0, sizeof(p));
  p.arena = arena;
  s_lexer_init(&p.lx, source, length);

  p.current = s_lexer_next(&p.lx);
  if (p.current.kind == MI_TOK_ERROR)
  {
    out.error_line = p.current.line;
    out.error_column = p.current.column;
    out.error_message = p.current.lexeme;
    return out;
  }

  MiScript* script = s_parse_script(&p, false);
  if (p.had_error || !script)
  {
    out.error_line = p.error_line;
    out.error_column = p.error_column;
    out.error_message = p.error_message;
    return out;
  }

  out.ok = true;
  out.script = script;
  return out;
}
