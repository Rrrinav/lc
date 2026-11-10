#!/usr/bin/env python3
"""Build ./bin/lc and run the lambda-calculus test suite.

Test cases live in ./tests as flat file pairs:

    <name>.lc     source passed to the interpreter
    <name>.res    exact expected stdout (or use .out, see below)

Optional files for special cases:

    <name>.out    stdout substrings, one per line (for non-deterministic output)
    <name>.in     run the REPL with this stdin instead of a source file
    <name>.args   arguments to pass, one per line, instead of a source file
    <name>.err    substring expected on stderr
    <name>.code   expected exit code (default: 0, or 1 when .err exists)

Usage:
    python3 build.py             # build ./bin/lc and run the tests
    python3 build.py --build-only
    python3 build.py --test-only
    python3 build.py --compiler clang++ --filter capture
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "main.cpp"
TESTS = ROOT / "tests"
DEFAULT_BIN_DIR = ROOT / "bin"
BINARY_NAME = "lc"
BUILD_FLAGS = ("-Wall", "-Wextra", "-Wpedantic")
TEST_TIMEOUT_SECONDS = 10
INPUT_SUFFIXES = (".lc", ".in", ".args")


@dataclass(frozen=True)
class Test_case:
    name: str
    stem: Path


def build_binary(compiler: str, standard: str, bin_dir: Path) -> Path:
    bin_dir = bin_dir.resolve()
    bin_dir.mkdir(parents=True, exist_ok=True)
    binary = bin_dir / BINARY_NAME
    command = [compiler, str(SOURCE), "-o", str(binary), f"-std={standard}", *BUILD_FLAGS]

    print(f"[build] {' '.join(command)}")
    try:
        result = subprocess.run(command, cwd=ROOT, encoding="utf-8", capture_output=True)
    except FileNotFoundError:
        raise SystemExit(f"error: compiler not found: {compiler}")

    if result.returncode != 0:
        if result.stdout.strip():
            print(result.stdout.rstrip())
        if result.stderr.strip():
            print(result.stderr.rstrip(), file=sys.stderr)
        raise SystemExit(f"error: build failed with exit code {result.returncode}")

    if result.stderr.strip():
        print(f"[warnings]\n{result.stderr.rstrip()}", file=sys.stderr)

    return binary


def discover_cases() -> list[Test_case]:
    if not TESTS.is_dir():
        raise SystemExit(f"error: tests directory not found: {TESTS}")

    names = {
        path.stem
        for path in TESTS.iterdir()
        if path.is_file() and path.suffix in INPUT_SUFFIXES
    }
    return [Test_case(name, TESTS / name) for name in sorted(names)]


def run_process(
    binary: Path,
    arguments: list[str],
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), *arguments],
        cwd=ROOT,
        input=input_text,
        encoding="utf-8",
        capture_output=True,
        timeout=TEST_TIMEOUT_SECONDS,
    )


def check_case(binary: Path, case: Test_case) -> list[str]:
    stem = case.stem
    result_path = stem.with_suffix(".res")
    output_path = stem.with_suffix(".out")
    if not result_path.is_file() and not output_path.is_file():
        return [f"missing {result_path.name} (or {output_path.name})"]

    expected_stdout = result_path.read_text(encoding="utf-8") if result_path.is_file() else None
    expected_stdout_parts = (
        [line for line in output_path.read_text(encoding="utf-8").splitlines() if line]
        if output_path.is_file()
        else []
    )

    error_path = stem.with_suffix(".err")
    expected_stderr = (
        error_path.read_text(encoding="utf-8").strip() if error_path.is_file() else ""
    )

    code_path = stem.with_suffix(".code")
    if code_path.is_file():
        code_text = code_path.read_text(encoding="utf-8").strip()
        if not code_text.lstrip("-").isdigit():
            return [f"invalid {code_path.name}: {code_text!r}"]
        expected_returncode = int(code_text)
    else:
        expected_returncode = 1 if error_path.is_file() else 0

    input_path = stem.with_suffix(".in")
    args_path = stem.with_suffix(".args")
    source_path = stem.with_suffix(".lc")

    inputs = [path for path in (input_path, args_path, source_path) if path.is_file()]
    if len(inputs) != 1:
        found = ", ".join(path.suffix for path in inputs) or "none"
        return [f"expected exactly one of .lc, .in, or .args; found {found}"]

    input_text = None
    arguments: list[str] = []
    if input_path.is_file():
        input_text = input_path.read_text(encoding="utf-8")
    elif args_path.is_file():
        arguments = [
            line.strip()
            for line in args_path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
    else:
        arguments = [str(source_path.resolve())]

    try:
        result = run_process(binary, arguments, input_text=input_text)
    except subprocess.TimeoutExpired:
        return [f"timed out after {TEST_TIMEOUT_SECONDS}s"]

    problems: list[str] = []
    if result.returncode != expected_returncode:
        problems.append(f"exit code {result.returncode}, expected {expected_returncode}")
    if expected_stdout is not None:
        if result.stdout != expected_stdout:
            problems.append(f"stdout was {result.stdout!r}, expected {expected_stdout!r}")
    else:
        for part in expected_stdout_parts:
            if part not in result.stdout:
                problems.append(f"stdout does not contain {part!r}")
    if expected_stderr:
        if expected_stderr not in result.stderr:
            problems.append(f"stderr {result.stderr!r} does not contain {expected_stderr!r}")
    elif result.stderr:
        problems.append(f"unexpected stderr {result.stderr!r}")
    return problems


def parse_arguments(arguments: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build the interpreter and run its tests.")
    parser.add_argument(
        "--compiler",
        default=os.environ.get("CXX", "g++"),
        help="C++ compiler to use (default: $CXX or g++)",
    )
    parser.add_argument("--std", default="c++23", help="C++ standard (default: c++23)")
    parser.add_argument(
        "--bin-dir",
        type=Path,
        default=DEFAULT_BIN_DIR,
        help=f"directory for the built binary (default: {DEFAULT_BIN_DIR})",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true", help="build the binary and skip tests")
    mode.add_argument(
        "--test-only",
        action="store_true",
        help="run tests against an existing binary without rebuilding",
    )
    parser.add_argument("--filter", default="", help="only run tests whose name contains this text")
    return parser.parse_args(arguments)


def main(arguments: list[str]) -> int:
    options = parse_arguments(arguments)

    if options.test_only:
        binary = options.bin_dir.resolve() / BINARY_NAME
        if not binary.is_file():
            raise SystemExit(f"error: binary not found: {binary} (build it first)")
    else:
        binary = build_binary(options.compiler, options.std, options.bin_dir)
        if options.build_only:
            print(f"[built] {binary}")
            return 0

    cases: list[tuple[str, Callable[[], list[str]]]] = [
        (case.name, lambda case=case: check_case(binary, case))
        for case in discover_cases()
    ]

    selected = [
        item for item in cases if not options.filter or options.filter.lower() in item[0].lower()
    ]
    if not selected:
        raise SystemExit(f"error: no test names contain {options.filter!r}")

    passed = 0
    failed = 0
    for name, check in selected:
        problems = check()
        if problems:
            failed += 1
            print(f"[fail] {name}")
            for problem in problems:
                print(f"       {problem}")
        else:
            passed += 1
            print(f"[pass] {name}")

    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
