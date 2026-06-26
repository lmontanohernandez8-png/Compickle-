from setuptools import Extension, setup
import platform

extra_compile_args = ["-O3"]

# GCC/Clang
if platform.machine():
    extra_compile_args.append("-march=native")
    extra_compile_args.append("-mtune=native")

setup(
    name="_compickle",
    ext_modules=[
        Extension(
            "_compickle",
            sources=["compickle.c"],
            extra_compile_args=extra_compile_args,
        )
    ],
)
