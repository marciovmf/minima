# Minima

Minima is a small, embeddable scripting language that aims for **high expressiveness** without losing **predictability**, and with a **very light, cheap runtime**.

Minima is designed to be embedded anywhere you need the power of a scripting language but cannot afford the weight of Lua or the complexity of larger interpreters. It aims to be expressive in the spirit of Tcl or Lisp—composable, flexible, and pleasant to write—while stripping away the historical quirks, ambiguous rules, and conceptual overhead that often accompany them. The goal is simple: keep what is good, fix what is bad, and provide a predictable, lightweight language that feels sane to use and cheap to run.

---

At the top level, Minima is **command-based**:

```text
cmd_name :: arg1 arg2 ...
```

- `cmd_name` is evaluated and must produce a string (the command name).
- `::` separates the command name from its arguments.
- Commands are separated by newlines or `;`.

Structure and expressions use:

- `(...)` for expressions and command expressions.
- `{...}` for block literals (code-as-data, never auto-executed).
- `[...]` for lists and dictionaries or indexing arrays.
- `$`     Expands the value of a variable.

---

## Data Types

```minima
# --------- Primitive values ---------

set :: n_int    42          ; int literal
set :: n_float  3.14        ; float literal
set :: s_text   "hello"     ; string literal
set :: b_true   true        ; boolean literal (true/false)
set :: b_false  false
set :: v_void   ()          ; void literal (unit value)


# --------- Composite values ---------

set :: l_list [1 2 3]                ; list of values
set :: l_mixed [1 "two" true]        ; lists may mix types (host decides how to use)

set :: d_dict ["a": 1 "b": 2]        ; dictionary (map) literal
set :: d_more ["x": 10 "y": (1 + 2)] ; values are expressions

set :: b_block {
  print :: "inside block"
  set   :: x 1
}                                    ; block literal (type: block, not auto-run)


# --------- Expressions and results ---------

set :: sum      (1 + 2 * 3)          ; arithmetic expression inside ()
set :: cmp_ok   (< 3 10)             ; comparison expression returning bool
set :: nested   ((1 + 2) * (3 + 4))  ; grouped arithmetic

# --------- Variables and access ---------

set :: x 10
print :: $x                          ; read variable x

set :: name "title"
print :: $("name")                   ; dynamic variable name: reads $name
                                     ; $("expression") evaluates to a name,
                                     ; then reads that variable


# --------- Indexing ---------

set :: xs [10 20 30]
print :: $xs[0]                      ; list read by index
print :: $xs[2]

# Lists do NOT grow by index:
# set :: xs[5] 99                    ; error in Minima, use list commands instead

set :: m ["answer": 42]
set :: m["extra"] 99                 ; dictionaries CAN grow by key
print :: $m["answer"]                ; dict read by key


# --------- Blocks as values ---------

set :: body {
  print :: "Hello from body"
}

# Blocks never auto-execute; a command like eval runs them:
eval :: $body                        ; runtime command, not syntax


# --------- Type stability ---------

set :: stable 10                     ; inferred type: int
set :: stable "oops"                 ; ERROR: variable type is fixed after first assignment

set :: strict:string                 ; explicit declaration
set :: strict "ok"                   ; must always be string
```

---

For the full, precise language definition (syntax, evaluation rules, and edge cases), [Minima spec document](minima.md)


