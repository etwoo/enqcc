# enqcc, Eric's Not-Quite-C Compiler

This repo contains my work-in-progress on Nora Sandler's
[Writing a C Compiler](https://nostarch.com/writing-c-compiler)
book.

This implementation covers Chapters 1-17 of Sandler's book. This
excludes Part II, Chapter 18 (Structures) and Part III, Chapters
19-20 (Optimizing IR, Register Allocation).

Supported:

- unary operators: `~`, `-`
- binary operators: `+`, `-`, `*`, `/`, `%`
- bitwise operators: `&`, `|`, `^`, `<<`. `>>`
- logical operators: `!`, `&&`, `||`
- relational operators: `==`, `!=`, `<`. `>`, `<=`, `>=`
- compound arithmetic assignment: `+=`, `-=`, `*=`, `/=`, `%=`
- compound bitwise assignment: `&=`, `|=`, `^=`, `<<=`, `>>=`
- pre/post increment, pre/post decrement: `++`, `--`
- conditional (ternary) expressions
- `if` statements
- compound statements (blocks, scopes)
- loops: `for`, `while`, `do`, `break`, `continue`
- labeled statements and `goto`
- `switch` statements
- functions
- local variables
- file-scope variables
- storage class specifiers: `extern`, `static`
- numeric types: `int`, `unsigned`, `long`, `long unsigned`, `double`
- pointers, arrays, and pointer arithmetic
- `char` and null-terminated C strings
- `sizeof`

Unsupported:

- `typedef`
- function pointer type
- single precision floating-point type `float`
- extended precision floating-point type `long double`
- compound literal expression, like `int *p = (int []){2, 4};`
- non-constant expression as static initializer, like `static int *ptr = &x;`
- pointer arithmetic with `void` pointer and `sizeof` operations on `void`
- `struct`
- `union`
- variable access via global offset table (GOT)
