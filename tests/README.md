# Test cases

Each case is a flat pair in this directory:

| File | Required | Meaning |
| --- | --- | --- |
| `<name>.lc` | yes* | Source passed to the interpreter |
| `<name>.res` | yes** | Exact expected stdout |
| `<name>.out` | no | Expected stdout substrings, one per line (alternative to `.res`) |
| `<name>.in` | no | REPL stdin, used instead of `<name>.lc` |
| `<name>.args` | no | Arguments to pass, one per line, used instead of `<name>.lc` |
| `<name>.flags` | no | Extra arguments appended (for example `--trace`) |
| `<name>.err` | no | Substring expected on stderr |
| `<name>.code` | no | Expected exit code (default: 0, or 1 when `.err` exists) |

\* Exactly one of `<name>.lc`, `<name>.in`, or `<name>.args` is required per case.

\** Either `<name>.res` or `<name>.out` is required per case.

Example:

```
tests/identity.lc   -> \x.x
tests/identity.res  -> \x.x
```

Build `./bin/lc` and run all cases with:

```sh
python3 build.py
```

Use `--test-only` to skip the build, `--filter <text>` to run a subset,
and `--help` for the other options.
