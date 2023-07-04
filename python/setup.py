from sys import exit
from distutils.core import setup, Extension
from setuptools.command.test import test as TestCommand

module1 = Extension('LossyQueue',
                    sources = ['LossyQueue_mod.c', '../SPSCQueue.c'])

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
       ext_modules = [module1],
       tests_require=['pytest'],
       cmdclass={'test': PyTest},
)

