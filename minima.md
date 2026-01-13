# Minima   
Minimal DSL language specification   
 --- 
# Minima Language Specification — Development Guide (Version 1.23)   
This document is the authoritative reference for both USING and IMPLEMENTING the Minima language.   
# Overview   
Minima is a minimal, command-driven, expression-capable scripting language.   
Core principles:   
1. At the script level (file and {} blocks), each line is a COMMAND.   
2. Commands may be separated by newline or by ';'.   
3. A command has the form: E0 :: E1 E2 ... En   
4. E0 is evaluated to obtain the command name.   
5. E0 must evaluate to a string.   
6. The '::' separator is mandatory for providing arguments.   
7. E1..En are argument AST nodes. Commands decide how/when to evaluate them.   
8. ( ... ) creates a subexpression, which is a command call ONLY if it contains '::'.   
9. { ... } creates a block literal (never executed automatically).   
10. Variables have fixed types. Minima has no implicit type conversions.   
   
# 1. Script Structure   
## 1.1 Script-Level Grammar   
A Minima script file or a { ... } block is a sequence of commands.   
Where each command is: E0 :: E1 E2 ... En
There is no standalone expression mode at the script level.   
If a line lacks '::', it is treated as a command with no arguments. If the expression does not evaluate to a string naming a valid command, it produces a runtime error.   
# 2. Expressions   
Expressions appear in subexpressions, list/dict literals, or as command arguments.   
Expression types:   
1. Literals (integer, float, string, boolean, void)   
2. Variable access ($x, $x[1])   
3. Infix operations (1 + 2)   
4. List/Dict literals [ ... ]   
5. Block literals { ... }   
6. Command expressions (E0 :: E1 ...) inside parentheses.   
   
# 3. Command Expressions   
A command expression is triggered by the '::' separator.   
Evaluation:   
1. Evaluate E0. Result must be a string.   
2. Look up the command with that name.   
3. Pass E1..En as AST nodes to the command.   
4. Command returns a value.   
   
Examples:   
print :: "hello"
set :: x 10
(set :: cmd "print") ; ($cmd :: "message")   
# 4. Parentheses and Parsing Rules   
Parentheses have two meanings:   
1. Group expression (expr)   
2. Introduce a command expression if '::' is present.   
   
Rule: Inside ( ... ), if the '::' separator is present, the expression to its left is the command head. If '::' is absent, it is a standard value expression.   
Examples:   
(1 + 1)         => normal arithmetic expression
(print :: "x")  => command expression
((rand ::) + 1) => arithmetic using command result   
# 5. The Critical Case: (rand + 1) vs (rand :: + 1)   
The parser uses the '::' separator to distinguish between arithmetic and commands:   
1. (rand + 1)   
    - No '::' present.   
    - Parsed as Arithmetic Expression: Variable(rand) + Literal(1).   
2. (rand :: + 1)   
    - '::' present.   
    - Parsed as Command Expression: Command head "rand", Arguments ["+", "1"].   
   
This mandatory separator allows commands to use arbitrary tokens (like '+') as arguments while keeping standard math intuitive.   
# 6. Variables   
Variables are explicitly or implicitly typed and "frozen" once assigned.   
- set :: x:int (Declaration)   
- set :: x 10 (Inferred)   
- set :: x "foo" (ERROR: Type mismatch)   
   
# 7. List Semantics   
7.1 Strictly Evaluated Literals   
[a, b, c] evaluates a, b, c immediately (except blocks {}).   
Result type:   
- if all elements are pairs -> dict   
- otherwise -> list   
   
7.2 Lists DO NOT grow via indexing.   
Writing to out-of-bounds index is an ERROR. Reading returns void.   
7.3 Only explicit list commands can grow lists:
list :: append l value
list :: expand listA listB   
# 8. Dictionary Semantics   
Dictionaries MAY grow via indexing.
Missing key reads return void.   
Keys may be any value type.   
# 9. Blocks { ... }   
Blocks are literal AST nodes.   
- NEVER executed automatically.   
- Executed only through commands such as 'eval :: { ... }'.   
- Contents follow script rules (each line is a command).   
   
# 10. Physical vs Logical Lines   
Minima uses indentation forgiveness to support multi-line structures.   
1. Command termination: Newline, semicolon, or EOF outside of groupings.   
2. Indentation forgiveness: Empty lines after '(', '[', '{', or a comma are ignored.   
3. Logical continuity:
foreach :: item $list {
print :: $item
}   
   
# 11. Standard Library Commands   
These live in the runtime and require the '::' separator.   
- set :: name value   
- print :: arg1 arg2   
- list :: append list value   
- eval :: block   
   
# 12. Errors   
12.1 Command name evaluation error:
Literals like '5' or lists like '[1,2,3]' at the start of a command line produce errors unless they evaluate to a valid command string.
12.2 Invalid command arguments:
If 'rand' does not accept args, 'rand :: + 1' results in a runtime error.
12.3 Type mismatch:
Static/Runtime error if fixed type variables are reassigned different types.   

# 13. Grammar

```
<program> ::= <script> <EOF>

<script> ::= <opt_newlines> <command_seq>

<command_seq> ::= <command_line> <command_seq>
              |  ε

<command_line> ::= <command> <opt_newlines>

<opt_newlines> ::= <NEWLINE> <opt_newlines>
                |  ε


<command> ::= <command_expr>


/* -----------------------------
   Expressions (full precedence)
   ----------------------------- */

<expr> ::= <pair_expr>

<pair_expr> ::= <or_expr> <pair_tail>

<pair_tail> ::= ":" <expr>
             |  ε

<or_expr> ::= <and_expr> <or_tail>

<or_tail> ::= "or" <and_expr> <or_tail>
           |  ε

<and_expr> ::= <equality_expr> <and_tail>

<and_tail> ::= "and" <equality_expr> <and_tail>
            |  ε

<equality_expr> ::= <comparison_expr> <equality_tail>

<equality_tail> ::= "==" <comparison_expr> <equality_tail>
                 |  "!=" <comparison_expr> <equality_tail>
                 |  ε

<comparison_expr> ::= <additive_expr> <comparison_tail>

<comparison_tail> ::= "<"  <additive_expr> <comparison_tail>
                   |  "<=" <additive_expr> <comparison_tail>
                   |  ">"  <additive_expr> <comparison_tail>
                   |  ">=" <additive_expr> <comparison_tail>
                   |  ε

<additive_expr> ::= <multiplicative_expr> <additive_tail>

<additive_tail> ::= "+" <multiplicative_expr> <additive_tail>
                 |  "-" <multiplicative_expr> <additive_tail>
                 |  ε

<multiplicative_expr> ::= <unary_expr> <multiplicative_tail>

<multiplicative_tail> ::= "*" <unary_expr> <multiplicative_tail>
                       |  "/" <unary_expr> <multiplicative_tail>
                       |  "%" <unary_expr> <multiplicative_tail>
                       |  ε

<unary_expr> ::= "-"   <unary_expr>
              |  "+"   <unary_expr>
              |  "not" <unary_expr>
              |  <postfix_expr>


/* -----------------------------------------
   Postfix (indexing + command call postfix)
   ----------------------------------------- */

<postfix_expr> ::= <primary_expr> <index_chain> <command_call_opt>

<index_chain> ::= <indexing> <index_chain>
               |  ε

<indexing> ::= "[" <group_newlines> <expr> <group_newlines> "]"

<command_call_opt> ::= "::" <arg_seq>
                    |  ε

<arg_seq> ::= <arg_expr> <arg_seq>
           |  ε


/* -------------------------------------------------------
   Arg expressions (word-level):
   In code: args are parsed with s_parse_expr_core(), which
   disallows top-level infix ops, but you can still use full
   <expr> by parenthesizing:  foo :: (1 + 2)
   ------------------------------------------------------- */

<arg_expr> ::= <unary_expr>
/* SEMANTIC RULE:
   When parsing command arguments after '::', the parser stops
   at NEWLINE / EOF / ')' / '}' / ']' and also refuses to start
   an arg unless the next token begins an expression.
   Also: top-level infix is effectively gated behind '(' ... ')'.
*/


/* -----------------------------
   Primary expressions / literals
   ----------------------------- */

<primary_expr> ::= <int_lit>
                |  <float_lit>
                |  <string_lit>
                |  <bool_lit>
                |  <bareword_string>
                |  <var_expr>
                |  <group_expr>
                |  <list_or_dict_lit>
                |  <block_lit>

<bool_lit> ::= "true"
            |  "false"

<bareword_string> ::= <IDENT>
/* NOTE: in minima.c, IDENT used as an expression becomes a string literal node. */

<var_expr> ::= "$" <IDENT>
            |  "$" "(" <expr> <group_newlines> ")"

<group_expr> ::= "(" <group_newlines> <group_contents> ")"

<group_contents> ::= <expr> <group_newlines>
                  |  ε
/* NOTE: "()" is a void literal. */

<list_or_dict_lit> ::= "[" <group_newlines> <list_items> "]"

<list_items> ::= <elements_opt> <group_newlines>

<elements_opt> ::= <elements>
                |  ε

<elements> ::= <expr> <elements_tail> <trailing_comma_opt>

<elements_tail> ::= "," <group_newlines> <expr> <elements_tail>
                 |  ε

<trailing_comma_opt> ::= ","
                      |  ε

/* SEMANTIC RULE:
   The parser returns a DICT node only if every element parsed in [...]
   is a pair (i.e., each element has the form <expr> ":" <expr>).
   Otherwise it returns a LIST node (and pairs may still appear as elements).
*/

<block_lit> ::= "{" <script> "}"


/* -----------------------------
   Newline handling inside groups
   ----------------------------- */

<group_newlines> ::= <NEWLINE> <group_newlines>
                  |  ε
/* NOTE: implementation allows NEWLINE immediately after '(' / '[' / '{' / ',' and before closing tokens. */


/* -----------------------------
   Lexical terminals (tokens)
   ----------------------------- */

<int_lit> ::= <INT>
<float_lit> ::= <FLOAT>
<string_lit> ::= <STRING>

<IDENT> ::= token(MI_TOK_IDENTIFIER)
<INT>   ::= token(MI_TOK_INT)
<FLOAT> ::= token(MI_TOK_FLOAT)
<STRING>::= token(MI_TOK_STRING)

<NEWLINE> ::= token(MI_TOK_NEWLINE)
<EOF>     ::= token(MI_TOK_EOF)


/* -----------------------------
   Command expression
   ----------------------------- */

<command_expr> ::= <postfix_expr_head> "::" <arg_seq>

<postfix_expr_head> ::= <postfix_expr>
/* NOTE: In practice, the head can be something complex if you wrap it in (...),
   because '(' <expr> ')' produces an expression that can then take the '::' postfix. */

```

# 14. FAQ   
Q1: Why is (rand + 1) arithmetic?
Because it lacks '::'. The separator is the explicit trigger for command arguments.   
Q2: How do I call a command with no arguments?
Standalone: rand. Inside subexpression: (rand ::).   
Q3: Can I build my own keywords?
Yes. Commands like 'foreach :: var list { block }' feel like native constructs because they receive the block as an AST node.   
