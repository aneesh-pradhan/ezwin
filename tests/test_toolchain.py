"""Integration checks for the enforced build policy (no disk-device access)."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ToolchainPolicy(unittest.TestCase):
    def reject(self, args=(), env=None, expected=""):
        with tempfile.TemporaryDirectory(prefix="ezwin-policy-") as build:
            process = subprocess.run(
                ["cmake", "-S", str(ROOT), "-B", build, "-G", "Ninja", *args],
                env={**os.environ, **(env or {})},
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            self.assertNotEqual(process.returncode, 0, process.stdout)
            self.assertIn(expected, " ".join(process.stdout.split()))

    @unittest.skipUnless(shutil.which("g++"), "GCC is not installed")
    def test_gcc_rejected(self):
        self.reject(["-DCMAKE_CXX_COMPILER=g++"], expected="requires the pinned Clang")

    @unittest.skipUnless(shutil.which("g++"), "GCC is not installed")
    def test_environment_gcc_rejected(self):
        self.reject(env={"CXX": "g++"}, expected="requires the pinned Clang")

    def test_older_standard_rejected(self):
        self.reject(["-DCMAKE_CXX_STANDARD=23"], expected="requires C++26")

    def test_gnu_extensions_rejected(self):
        self.reject(["-DCMAKE_CXX_EXTENSIONS=ON"], expected="without GNU C++ extensions")

    def test_environment_cannot_replace_toolchain(self):
        for variable, value in [
            ("CXXFLAGS", "-std=c++20"),
            ("CXXFLAGS", "-stdlib=libstdc++"),
            ("LDFLAGS", "-fuse-ld=bfd"),
            ("LDFLAGS", "--rtlib=libgcc"),
            ("LDFLAGS", "--unwindlib=libgcc"),
        ]:
            with self.subTest(variable=variable, value=value):
                self.reject(env={variable: value}, expected="managed by ezwin")

    def test_cmake_flags_cannot_replace_toolchain(self):
        for variable, value in [
            ("CMAKE_CXX_FLAGS", "-std=c++20"),
            ("CMAKE_CXX_FLAGS", "-stdlib=libstdc++"),
            ("CMAKE_EXE_LINKER_FLAGS", "-fuse-ld=bfd"),
            ("CMAKE_EXE_LINKER_FLAGS", "--rtlib=libgcc"),
            ("CMAKE_EXE_LINKER_FLAGS", "--unwindlib=libgcc"),
        ]:
            with self.subTest(variable=variable, value=value):
                self.reject([f"-D{variable}={value}"], expected="conflicts with the pinned")

    def test_missing_toolchain_rejected(self):
        with tempfile.TemporaryDirectory(prefix="ezwin-no-llvm-") as missing:
            version = (ROOT / ".llvm-version").read_text().strip()
            self.reject([f"-DLLVM_ROOT={missing}"], expected=f"LLVM {version} is required")

    def test_actual_compile_commands(self):
        commands = json.loads((ROOT / "build/llvm/compile_commands.json").read_text())
        self.assertGreaterEqual(len(commands), 5)
        for entry in commands:
            args = shlex.split(entry["command"])
            with self.subTest(file=entry["file"]):
                self.assertIn("clang", Path(args[0]).name)
                self.assertTrue(any(arg in ("-std=c++26", "-std=c++2c") for arg in args))
                self.assertIn("-stdlib=libc++", args)
                self.assertTrue(any(arg.endswith("toolchain-check.hpp") for arg in args))

    def test_linked_runtime_and_linker(self):
        binary = ROOT / "build/llvm/ezwin"
        dependencies = subprocess.check_output(["ldd", str(binary)], text=True)
        self.assertNotIn("not found", dependencies)
        self.assertNotIn("libstdc++", dependencies)
        direct_dependencies = subprocess.check_output(
            ["readelf", "--dynamic", str(binary)], text=True)
        self.assertNotIn("libgcc", direct_dependencies)
        self.assertNotIn("libstdc++", direct_dependencies)
        cache = (ROOT / "build/llvm/CMakeCache.txt").read_text().splitlines()
        llvm_root = Path(next(line.split("=", 1)[1] for line in cache
                              if line.startswith("LLVM_ROOT:PATH="))).resolve()
        for runtime in ("libc++.so", "libc++abi.so", "libunwind.so"):
            self.assertIn(runtime, dependencies)
            line = next(line for line in dependencies.splitlines() if runtime in line)
            loaded = Path(line.split("=>", 1)[1].strip().split()[0]).resolve()
            self.assertTrue(loaded.is_relative_to(llvm_root), line)
        comments = subprocess.check_output(
            ["readelf", "--string-dump=.comment", str(binary)], text=True)
        version = (ROOT / ".llvm-version").read_text().strip()
        self.assertIn(f"Linker: LLD {version}", comments)


if __name__ == "__main__":
    unittest.main()
