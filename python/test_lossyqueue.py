import sys
import unittest
from LossyQueue import LossyQueue

class foo():
    def __init__(self, i):
        self.i = i

    def __del__(self):
        sys.stderr.write(f'foo.__del__({self.i})\n')
        sys.stderr.flush()

class TestLossyQueue(unittest.TestCase):

    def test_queue(self):
        queue = LossyQueue(10)
        queue.put('test')
        value = queue.get()
        self.assertEqual(value, 'test')
        for i in range(0, 10000):
            sys.stderr.write(f'queue.put(foo({i}))\n')
            sys.stderr.flush()
            queue.put(foo(i))
        self.assertEqual(queue.get().i, 9991)

if __name__ == '__main__':
    unittest.main()
