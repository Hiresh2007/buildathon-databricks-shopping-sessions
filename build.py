"""Build the native pipeline against the Arrow libraries from the pinned wheel."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import sys

root = Path(importlib.util.find_spec('pyarrow').origin).parent.resolve()
pattern = '.so.*' if sys.platform.startswith('linux') else '.*.dylib'
libs = [next(root.glob('lib' + name + pattern)) for name in ('parquet', 'arrow')]
cmd = [*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17', '-pthread',
       *shlex.split(os.environ.get('CXXFLAGS', '-O3')), '-I', str(root / 'include'),
       'sessions.cpp', *libs, '-Wl,-rpath,' + str(root), '-o', 'sessions']
subprocess.run(cmd, check=True)
