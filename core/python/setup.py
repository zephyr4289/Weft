"""cffi integration for the weft Python binding (see build.py for the cdef).

The classic cffi entry: setuptools compiles the extension described by
build.py:ffibuilder. `python3 setup.py build_ext --inplace` builds the
in-tree extension for running the battery from a checkout;
`pip install .` builds and installs it.
"""
from setuptools import setup, find_packages

setup(
    name="weft",
    version="0.1.0",
    packages=find_packages(),
    cffi_modules=["build.py:ffibuilder"],
    install_requires=["cffi>=1.15"],
    setup_requires=["cffi>=1.15", "setuptools>=61.0"],
)
