from sys import exit
from distutils.core import setup, Extension
from setuptools.command.test import test as TestCommand
from os.path import realpath, dirname, join as path_join
from sys import argv as sys_argv

mod_name = 'LossyQueue'
mod_name_dbg = mod_name + '_debug'

mod_dir = dirname(realpath(sys_argv[0]))
src_dir = '../src/'

compile_args = [f'-I{src_dir}',]
debug_cflags = ['-g3', '-O0', '-DDEBUG_MOD']
mod_common_args = {
    'sources': ['LossyQueue_mod.c', src_dir + 'SPMCQueue.c'],
    'extra_compile_args': compile_args
}
mod_debug_args = mod_common_args.copy()
mod_debug_args['extra_compile_args'] = mod_debug_args['extra_compile_args'] + debug_cflags

module1 = Extension(mod_name, **mod_common_args)
module2 = Extension(mod_name_dbg, **mod_debug_args)

class PyTest(TestCommand):
    user_options = [('pytest-args=', 'a', "Arguments to pass to pytest")]

    def initialize_options(self):
        TestCommand.initialize_options(self)
        self.pytest_args = []

    def run_tests(self):
        import pytest
        errno = pytest.main(self.pytest_args)
        exit(errno)

setup (name = 'SPMCQueue',
       version = '1.0',
       description = 'This is a package for LossyQueue module',
       ext_modules = [module1, module2],
       tests_require=['pytest'],
       cmdclass={'test': PyTest},
)

