# The CalcIDE compiler and VM

CalcIDE compiles a C dialect ("Tiny-C") to compact bytecode, which a virtual machine runs with runtime checks. The compiler is about 3,000 lines in `cc/cc.c`, written in the same dialect. It compiles itself: the copy the native compiler builds and the copy that copy builds are byte-identical.

## Contents

- [Using it](#using-it)
- [The language](#the-language)
- [The preprocessor](#the-preprocessor)
- [Built-in library](#built-in-library)
- [PicoCalc functions](#picocalc-functions)
- [Runtime errors](#runtime-errors)
- [Memory model](#memory-model)
- [Self-hosting](#self-hosting)
- [How it works](#how-it-works)
- [Differences from standard C](#differences-from-standard-c)

## Using it

```
cc hello.c                 -> hello.cbc
cc hello.c -o /bin/hi.cbc  -> choose the output
cc big.c -v                -> sizes and compiler memory
run hello.c                -> compile and run
run hello.cbc a b c        -> argv[1..3] = "a" "b" "c"
```

In the editor, **F9** compiles and **F5** compiles and runs. After a compile error the cursor lands on the reported line. Error messages look like this:

```
/projects/game/life.c:42: error: undeclared identifier 'neighbors'
```

The compiler stops at the first error. Calling an undeclared function is allowed (it's assumed to return `int`), but it prints a warning.

## The language

### Types

| Type | Size | Notes |
|---|---|---|
| `char`, `signed char`, `unsigned char` | 1 | plain `char` is signed |
| `short`, `unsigned short` | 2 | |
| `int`, `unsigned`, `long`, `unsigned long` | 4 | |
| `long long` | 4 | accepted, but 32-bit |
| `float`, `double`, `long double` | 4 | all IEEE single precision |
| pointers, function pointers | 4 | pointers to pointers to any depth |
| arrays | | any number of dimensions, `T a[] = {...}` sizes from the initializer |
| `struct`, `union` | | nested, self-referential, arrays of structs, structs in unions |
| `enum` | 4 | constants with explicit values |
| `void`, `_Bool` | | `bool`, `true`, `false` are predefined |

Declarators work in full: `int (*fp)(int, int)`, `int (*table[4])(void)`, `int (*getop(char c))(int, int)`, `char (*p)[8]` and `typedef`s of any of these. `const`, `volatile`, `register`, `auto` and `inline` are accepted and ignored. `static` and `extern` work at file scope and in blocks, including static locals with initializers.

### Expressions

All C operators are supported, with C precedence and associativity:

```
()  []  ->  .  ++  --           (postfix)
++  --  +  -  !  ~  *  &  sizeof  (type)
*  /  %     +  -     <<  >>     <  <=  >  >=     ==  !=
&  ^  |     &&  ||   ?:         =  +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=
,
```

- Arithmetic uses the usual conversions: char/short promote to int, and anything mixed with float becomes float. Unsigned comparison, division and right shift are used where C requires them.
- Pointer arithmetic scales by element size, and pointer minus pointer gives an element count.
- `&&` and `||` short-circuit.
- Constant expressions fold at compile time, including floating point. That is how array sizes, `case` labels, `enum` values and global initializers are evaluated.

### Statements

`if`/`else`, `while`, `do`/`while`, `for` (including `for (int i = 0; ...)`), `switch`/`case`/`default` (with fall-through), `break`, `continue`, `return`, `goto` and labels, blocks with declarations anywhere, and the expression and empty statements.

### Functions

- Parameters and return values can be any type, including **structs by value**.
- Recursion is limited only by the stack (`vm_stack_kb`, default 32 KB).
- Prototypes, forward references, and calls to functions defined later all work.
- Function pointers can be stored, passed, kept in tables and called.
- `main` can be `int main(void)` or `int main(int argc, char **argv)`.
- A program may define its own function with the same name as a builtin; its version wins.

### Initializers

- Scalars, strings into `char` arrays, and nested brace lists for arrays and structs, with missing elements set to zero.
- Global initializers may be any expression. Constants are stored in the program file; anything else runs before `main`.

### Comments

`// line`, `/* block */` and **`## line`**. As a result, `##` token pasting is not available in macros.

## The preprocessor

| Directive | Notes |
|---|---|
| `#include "file.h"` | Searched in the including file's directory, then `/include/`, then `/calcide/include/`. |
| `#include <stdio.h>` | The standard headers are built in, so any `<...>` that isn't found on the card is silently accepted. |
| `#include <picocalc.h>` | Built in. It adds the key codes and colour names (also available as `<conio.h>` and `<calcide.h>`). |
| `#define NAME text` | Object-like macros. |
| `#define F(a, b) text` | Function-like macros, up to 16 parameters. Arguments are fully expanded first, so nesting works. |
| `#undef` | |
| `#if`, `#elif`, `#else`, `#endif` | Full integer expressions, including `defined(X)` and `defined X`. |
| `#ifdef`, `#ifndef` | |
| `#error`, `#pragma`, `#line`, `#warning` | `#error` stops compilation; the others are ignored. |
| `\` at the end of a line | Continues a line. |

These are predefined: `__CALCIDE__`, `__LINE__`, `NULL`, `EOF`, `bool`/`true`/`false`, `SEEK_SET`/`SEEK_CUR`/`SEEK_END`, `RAND_MAX`, `INT_MAX`, `INT_MIN`, `UINT_MAX`, `CHAR_BIT`, `CHAR_MAX`, `CHAR_MIN`, `FLT_MAX`, `FLT_MIN`, `FLT_EPSILON`, `M_PI`, `M_E`, `EXIT_SUCCESS`, `EXIT_FAILURE`, `stdin`, `stdout` and `stderr`.

## Built-in library

Everything below is always declared, so `#include` lines are optional. The full list with exact prototypes is in `cc/prelude.h`.

**stdio:** `printf`, `sprintf`, `snprintf`, `fprintf`, `putchar`, `puts`, `getchar`, `gets`, `fgets`, `scanf`, `sscanf`, `fopen`, `fclose`, `fgetc`/`getc`, `fputc`/`putc`, `fputs`, `fread`, `fwrite`, `fseek`, `ftell`, `rewind`, `feof`, `fflush`, `remove`, `rename`, `mkdir`, `fsize`.

- `printf` supports `%d %i %u %x %X %o %c %s %p %f %e %g %%`, with flags `- + space # 0`, widths and precisions (including `*`), and the `l`/`h` modifiers (accepted and ignored).
- `scanf`/`sscanf` support `%d %i %u %x %o %c %s %f %e %g %[...] %n`, with widths and `*`.
- Files live on the SD card, with paths relative to the shell's current directory. Mode is `"r"`, `"w"` or `"a"`, optionally with `+`. Up to 8 files can be open at once, and all are closed when the program ends.
- `stdin` reads a line at a time with the line editor, `stdout` is the console, and `stderr` is the console in red.

**string:** `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strcasecmp`, `strchr`, `strrchr`, `strstr`, `strdup`, `strtok`, `memcpy`, `memmove`, `memset`, `memcmp`.

**ctype:** `isdigit`, `isalpha`, `isalnum`, `isspace`, `isupper`, `islower`, `isxdigit`, `ispunct`, `isprint`, `toupper`, `tolower`.

**stdlib:** `malloc`, `calloc`, `realloc`, `free`, `atoi`, `atof`, `strtol`, `strtof`, `abs`/`labs`, `rand`, `srand`, `qsort` (your comparison function is called from the VM), `exit`, `abort`, `assert`, `heap_free()` (bytes of free heap).

**math** (all `float`): `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sqrt`, `pow`, `exp`, `log`, `log10`, `floor`, `ceil`, `fabs`, `fmod`, `round`, `fmin`, `fmax`. The `f` suffixed forms (`sqrtf`, ...) are aliases.

## PicoCalc functions

| Function | |
|---|---|
| `void cls(void)` | clear the screen |
| `void gotoxy(int x, int y)`, `int wherex(void)`, `int wherey(void)` | text cursor position (0-based) |
| `void setcolor(int fg, int bg)`, `textcolor(fg)`, `textbackground(bg)` | colours 0–15 (`BLACK`, `RED`, ... `BRIGHT_WHITE` in `<picocalc.h>`) |
| `void cursor(int on)` | show or hide the cursor |
| `int screen_cols(void)`, `int screen_rows(void)` | 40 or 64 columns, 32 rows |
| `void refresh(void)` | push pending text to the panel now |
| `int getkey(void)` | wait for a key, without echo |
| `int inkey(void)` | the next key, or -1 if none (doesn't wait) |
| `int kbhit(void)` | is a key waiting? |
| `void beep(int hz, int ms)` | a tone (`ms = 0`: start it and return; `beep(0, 0)` stops it) |
| `void delay(int ms)`, `void sleep(int s)` | wait (Ctrl+C still works) |
| `int millis(void)`, `int clock(void)`, `int time(0)` | ms since the program started; seconds since boot |
| `void backlight(int n)`, `void kbdlight(int n)` | 0–255 |
| `int battery(void)`, `int battery_charging(void)` | percent (or -1); 1 if charging |
| `int cpu_mhz(void)` | CPU clock |
| `float temperature(void)`, `int cpu_temp(void)` | chip temperature in °C |
| `void pixel(x, y, color)`, `fillrect(x, y, w, h, color)`, `line(x0, y0, x1, y1, color)` | direct drawing on the 320×320 panel (RGB565) |
| `int rgb(int r, int g, int b)` | make an RGB565 colour |
| `int pf_read(char *name, void *buf, int max)` | read a page-file object; returns its length, or -1 |
| `int pf_write(char *name, void *buf, int len)` | write one (keep it rare: flash wears) |

`<picocalc.h>` defines `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_ENTER`, `KEY_BACKSPACE`, `KEY_TAB`, `KEY_ESC`, `KEY_DEL`, `KEY_INSERT`, `KEY_HOME`, `KEY_END`, `KEY_PGUP`, `KEY_PGDN`, `KEY_BREAK`, `KEY_F1`…`KEY_F10`, and the 16 colour names.

Text output also understands ANSI escapes: `printf("\033[1;33mwarning\033[0m\n")`.

## Runtime errors

The VM checks every memory access and every call. Any of these stops the program with a message naming the function and source line:

| Message | Cause |
|---|---|
| `null pointer dereference` | reading or writing through `NULL` (or near it) |
| `memory access out of bounds` | a pointer outside the program's memory |
| `stack overflow (recursion too deep?)` | the stack ran into the heap |
| `expression stack overflow` | extremely deep recursion inside expressions |
| `division by zero`, `division by zero (modulo)` | integer `/` or `%` by 0 (floats give inf/NaN as usual) |
| `free() of an invalid or already freed pointer` | double free, or freeing something `malloc` didn't return |
| `call through a null function pointer` | |
| `assertion failed`, `abort() called` | |
| `break (Ctrl+C)` | you pressed Ctrl+C or Brk |

`malloc` returns `NULL` when memory runs out, as in C. Open files are closed and sound stops whenever a program ends, however it ends.

## Memory model

A program sees one flat block of memory:

```
0 .. 63        trap zone (null pointers land here)
64 ..          string literals, initialised data, uninitialised data
...            heap: malloc/free (first fit, neighbours merge)
... top        stack: frames grow down; vm_stack_kb (default 32 KB)
```

The block is as large as free RAM allows (`vm_mem_kb = 0`), which is about 400 KB on an RP2350. Code runs in place from the loaded file, or straight from flash for `pf:` programs. Expression temporaries use a separate operand stack of 1024 cells.

## Self-hosting

`install` puts the compiler's source on the SD card. Then:

```
cc /calcide/cc/cc.c -o /bin/cc.cbc       # the firmware's native compiler builds it
run /bin/cc.cbc prog.c -o prog.cbc       # the compiler, running on the VM
run /bin/cc.cbc /calcide/cc/cc.c -o /bin/cc2.cbc
```

`cc.cbc` and `cc2.cbc` are identical; the test suite checks this on every run. Compiling the compiler needs about 320 KB of VM memory, so it fits on an RP2350 but not an RP2040. The same `cc.c` is also compiled by gcc into the firmware, through `cc/ccport.h`, as the fast native `cc` command. The two compilers produce identical output.

## How it works

- **Single pass, no syntax tree.** Expressions emit stack-machine code as they are parsed. An lvalue leaves its address on the stack, and the load is emitted only when the value is needed, so `a = b`, `a += b`, `&a` and `a.b.c` all fall out of the same code.
- **Constant folding** happens as code is emitted, by watching for runs of immediate values. The same mechanism evaluates array sizes, `case` labels, `enum` values and global initializers.
- **Global initializers** that aren't constants become small code fragments, chained to run before `main`.
- **The preprocessor** is part of the lexer, with token-level macro expansion and recycled buffers. Compiling the compiler (3,000 lines plus 124 built-in declarations) takes 128 KB of compiler memory.
- **The bytecode** has 79 opcodes, including fused forms for common local loads. A `.cbc` file holds a 48-byte header, the code, the initialised data, a line table, a function table and the source file names. The last three exist so runtime errors can name a line. `tools/cbcdis.py` disassembles a `.cbc`.
- **The VM** is a switch interpreter over a 32-bit operand stack. Floats are carried as their IEEE bit patterns. Every load and store is bounds checked.

## Differences from standard C

- `double` is the same as `float` (32-bit), and `long long` is 32-bit.
- There are no variadic functions of your own (`...` works only for the builtins), no bit-fields (the width is parsed and ignored), no designated initializers, no compound literals, and no VLAs.
- In macros, `#` (stringize) and `##` (paste) are not supported, because `##` starts a comment.
- Function arguments are evaluated left to right. C leaves the order unspecified, but gcc goes right to left, so code that depends on it behaves differently.
- `main`'s return value is the program's exit code. A runtime error gives exit code -1.
