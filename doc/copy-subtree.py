#!/usr/bin/env python3

import os
import sys
import shutil

subdir = os.environ["MESON_SUBDIR"]

input_path = os.path.join(os.environ["MESON_SOURCE_ROOT"], subdir, sys.argv[1])
output_path = os.path.join(os.environ["MESON_BUILD_ROOT"], subdir, sys.argv[2])

os.makedirs(os.path.dirname(output_path), exist_ok=True)

shutil.copytree(input_path, output_path)
