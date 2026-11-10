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
