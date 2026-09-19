"""cffi integration for the weft Python binding (see build.py for the cdef).

The classic cffi entry: setuptools compiles the extension described by
build.py:ffibuilder. `python3 setup.py build_ext --inplace` builds the
in-tree extension for running the battery from a checkout;
`pip install .` builds and installs it.
"""
from setuptools import setup

setup(cffi_modules=["build.py:ffibuilder"])
