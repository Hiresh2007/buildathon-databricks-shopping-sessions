"""Build the C++17 sessionizer and its native Parquet writer."""

import os
import shlex
import subprocess

compiler = shlex.split(os.environ.get("CXX", "c++"))
flags = shlex.split(os.environ.get("CXXFLAGS", "-O3"))
subprocess.run([*compiler, "-std=c++17", "-pthread", *flags, "sessions.cpp", "-o", "sessions"], check=True)
