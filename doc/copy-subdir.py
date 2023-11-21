#!/usr/bin/env python3

import os, sys, shutil

subdir = os.getenv('MESON_SUBDIR')

input_path = os.path.join(os.getenv('MESON_SOURCE_ROOT'), subdir, sys.argv[1])
output_path = os.path.join(os.getenv('MESON_BUILD_ROOT'), subdir, sys.argv[2])

os.makedirs(os.path.dirname(output_path), exist_ok=True)

shutil.copyfile(input_path, output_path)
