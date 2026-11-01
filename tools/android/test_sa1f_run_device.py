"""CPU tests of device runner verdicts; no adb calls."""
import unittest
from sa1f_run_device import evaluate_steps

COORDS={'touch-tap':(75,65),'touch-region':(25,65),'ui-tap':(200,210),'ui-hold':(200,210)}
def logs():
    result={}
    for name,(x,y) in COORDS.items():
        result[name]=(f'SA1f UI pointer id=1 event=pointer_down x={x} y={y} target=true effects=1\n'
                      f'SA1f pointer() x={x} y={y} inside=true\n'
                      f'SA1f UI pointer id=1 event=pointer_up x={x} y={y} target=true effects=2\n'
                      'SA1f pointer() x=0 y=0 inside=false\n').encode()
    return result
class VerdictTests(unittest.TestCase):
    def verdict(self, steps): return all(evaluate_steps(steps,COORDS).values())
    def test_complete(self): self.assertTrue(self.verdict(logs()))
    def test_fix6_each_ui_operation_requires_paired_release(self):
        for label in ('ui-tap','ui-hold'):
            for missing in ('pointer_down','pointer_up'):
                with self.subTest(label=label,missing=missing):
                    steps=logs(); steps[label]=b'\n'.join(l for l in steps[label].splitlines() if missing.encode() not in l)
                    self.assertFalse(self.verdict(steps))
            with self.subTest(label=label,missing='release_capture'):
                steps=logs(); steps[label]=steps[label].replace(b'target=true effects=2',b'target=true effects=0')
                self.assertFalse(self.verdict(steps))
    def test_fix7_coordinates_and_final_outside_per_operation(self):
        for label,(x,y) in COORDS.items():
            for bad in (f'x=0 y=0 inside=true',f'x={x+1} y={y} inside=true',f'x={x} y={y+1} inside=true'):
                with self.subTest(label=label,bad=bad):
                    steps=logs(); steps[label]=steps[label].replace(f'x={x} y={y} inside=true'.encode(),bad.encode()); self.assertFalse(self.verdict(steps))
            with self.subTest(label=label,missing='inside=false'):
                steps=logs(); steps[label]=steps[label].replace(b'inside=false',b'inside=true'); self.assertFalse(self.verdict(steps))
if __name__=='__main__': unittest.main()
