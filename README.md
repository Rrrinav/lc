# Lambda calculus

A simple lambda calculus library for learning purposes.

## Build

```sh
# Build ./bin/lc and run the test suite (Python 3, standard library only)
python3 build.py

# Build or test separately
python3 build.py --build-only
python3 build.py --test-only
```

`build.py` discovers cases in `tests/` as flat `<name>.lc` / `<name>.res`
pairs (see `tests/README.md`). It accepts `--compiler`, `--std`,
`--bin-dir`, and `--filter`. The binary is written to `bin/lc` by default.

## Usage

```sh
./bin/lc ./file.lc
./bin/lc ./file.lc --evaluator stack
./bin/lc ./file.lc --trace
./bin/lc
```

- The default evaluator is `recursive`, the original recursive tree walker.
- `--evaluator stack` uses the explicit-stack reducer. Both produce the same normal form.
- `--trace` uses the stack reducer and prints every whole-term reduction step:

```text
(\x.\y.x) y
-> \y'.y   (α: y -> y')
```

The final line is the normal form. `print` steps are annotated with `(print)`.

The C++ API exposes both implementations through the same template:

```cpp
lc::Evaluator<lc::recursive_tree_walker>{ctx}(ast);
lc::Evaluator<lc::explicit_stack_reducer>{ctx}(ast);
lc::Evaluator<lc::explicit_stack_reducer>{ctx}.trace_execution(ast);
```

Source files live in `src/`. `src/main.cpp` is the CLI and parser; the evaluators
are separate headers so they can be studied side by side.

Terms/things Support:
- Variables
- Abstraction
- Application
- Beta reduction ("normal substitution")
- Alpha renaming
- Strongly typed AST node ids (`Strong_id<Tag>`)

Maybe TODOs:
- "Normal arithematic" which internally uses church numerals.
- proper printing
- Term variables

## References

Examples take inspiration from this tutorial: [A Tutorial Introduction to the Lambda Calculus](https://personal.utdallas.edu/~gupta/courses/apl/lambda.pdf) by [Rojas Raul](https://www.mi.fu-berlin.de/inf/groups/ag-ki/members/Professoren/rojas_raul.html).
