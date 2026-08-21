# Kaleidoscope LLVM Frontend — Codespaces Edition

This project implements the core Kaleidoscope language frontend described in the official LLVM "My First Language Frontend" tutorial, using modern LLVM ORC JIT APIs.

## Implemented

- Lexical analysis: identifiers, numbers, keywords, operators, comments
- Recursive-descent parser
- Operator-precedence parsing
- AST nodes for numbers, variables, binary expressions, calls, `if/then/else`, and `for`
- LLVM IR code generation
- ORC LLJIT execution
- Function definitions and external declarations
- **Original extension: `print(expr)`**

## Example

```text
ready> print(10 + 20)
30
=> 30
ready> def add(a b) a + b
ready> add(5, 7)
=> 12
```

## Build in GitHub Codespaces

Open this repository in a Codespace. The `.devcontainer` setup installs the LLVM/C++ toolchain.

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/kaleidoscope
```

## Language examples

```text
1 + 2 * 3

print(42)

def add(a b)
  a + b

add(10, 20)

if 1 then 100 else 200

for i = 1, 5, 1 in print(i)
```

## Original extension

The tutorial normally demonstrates calling externally provided functions. This implementation exposes a built-in `print(expr)` convenience function that returns the printed value, allowing programs such as:

```text
print(2 * 21)
```

The parser treats this as a normal function call, while code generation binds it to the native helper `ks_print`.
