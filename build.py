"""Build the C++17 sessionizer and its native Parquet writer."""

import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train-v3", action="store_true", help="collect and apply a GCC x86-64-v3 profile")
    args = parser.parse_args()
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    flags = shlex.split(os.environ.get("CXXFLAGS", "-O3"))
    command = [*compiler, "-std=c++17", "-pthread", *flags]

    def compile_sessions(*options):
        subprocess.run([*command, *options, "sessions.cpp", "-o", "sessions"], check=True)

    if not args.train_v3:
        compile_sessions()
        return

    from training import write_training_data

    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        probe = work / "check-cpu"
        # The generic dispatcher uses the same check on the runtime machine.
        subprocess.run(
            [*compiler, "-x", "c++", "-", "-o", probe],
            input=b'int main() { return __builtin_cpu_supports("x86-64-v3") ? 0 : 1; }\n',
            check=True,
        )
        status = subprocess.run([probe]).returncode
        if status == 1:
            print("Training requires x86-64-v3; compiling the ordinary optimized binary.", flush=True)
            compile_sessions()
            return
        if status:
            raise RuntimeError(f"CPU probe exited with status {status}")

        profile = work / "counts"
        profile.mkdir()
        data = work / "input"
        write_training_data(data)
        compile_sessions(f"-fprofile-generate={profile}", "-fprofile-update=atomic")
        subprocess.run(["./sessions", data, work / "result"], env={**os.environ, "NCPU": "2"}, check=True)
        compile_sessions(f"-fprofile-use={profile}", "-fprofile-correction",
                         "-Werror=missing-profile", "-Werror=coverage-mismatch")
        print("Applied compiler profile from synthetic shopping records.", flush=True)


if __name__ == "__main__":
    main()
