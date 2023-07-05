from sys import exit
from distutils.core import setup, Extension
from setuptools.command.test import test as TestCommand

mod_name = 'LossyQueue'
mod_name_dbg = mod_name + '_debug'

mod_common_args = {'sources': ['LossyQueue_mod.c', '../SPSCQueue.c']}
mod_debug_args = {'extra_compile_args': ['-g3', '-O0', '-DDEBUG_MOD']}

module1 = Extension(mod_name, **mod_common_args)
module2 = Extension(mod_name_dbg, **mod_common_args, **mod_debug_args)

class PyTest(TestCommand):
    user_options = [('pytest-args=', 'a', "Arguments to pass to pytest")]

    def initialize_options(self):
        TestCommand.initialize_options(self)
        self.pytest_args = []

    def run_tests(self):
        import pytest
        errno = pytest.main(self.pytest_args)
        exit(errno)

setup (name = 'SPSCQueue',
       version = '1.0',
       description = 'This is a package for LossyQueue module',
       ext_modules = [module1, module2],
       tests_require=['pytest'],
       cmdclass={'test': PyTest},
)

