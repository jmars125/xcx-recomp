"""Native-UI hook validation and generator-injection tests; no game input needed."""
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'recomp'))
import native_ui as h

class Validation(unittest.TestCase):
    def module(self):
        return SimpleNamespace(name='main', base=h.BASE, nso=SimpleNamespace(build_id=h.BUILD_ID),
                               word=lambda a: h.WORDS[a - h.BASE])
    def test_wrong_build_and_layout(self):
        for field, value in [('name', 'sdk'), ('base', 0)]:
            m = self.module(); setattr(m, field, value)
            with self.assertRaises(ValueError): h.validate(m, b'')
        m = self.module(); m.nso.build_id = 'unsupported'
        with self.assertRaises(ValueError): h.validate(m, b'')
    def test_wrong_hash(self):
        with self.assertRaises(ValueError): h.validate(self.module(), b'unsupported')
    def test_every_original_instruction(self):
        with patch.object(h.hashlib, 'sha256', return_value=SimpleNamespace(hexdigest=lambda: h.NSO_SHA256)):
            h.validate(self.module(), b'fixture')
            for off in h.WORDS:
                m = self.module()
                m.word = lambda a, off=off: h.WORDS[a-h.BASE] ^ (1 if a-h.BASE == off else 0)
                with self.assertRaises(ValueError): h.validate(m, b'fixture')
    def test_overlapping_patch(self):
        h.validate_patches(SimpleNamespace(sites={'main': {0x1234: None}}))
        for off in h.WORDS:
            with self.assertRaises(ValueError):
                h.validate_patches(SimpleNamespace(sites={'main': {off: None}}))
    def test_hook_entries(self):
        self.assertEqual(len(h.HOOKS), 11)
        self.assertEqual(len({entry for _, _, entry, _ in h.HOOKS}), 11)
        self.assertIn(h.SQUAD_INPUT_ONLINE - 4, h.WORDS)   # the second guarded call site of the online query
        for hook_id, name, entry, caller in h.HOOKS:
            self.assertEqual(h.entry_hook(h.BASE + entry), f'if (rt_native_ui_hook({hook_id}, c)) return;')
            self.assertIn(entry, h.WORDS)
            self.assertIn(caller - 4, h.WORDS)   # the call instruction that makes the guarded return address
        self.assertIsNone(h.entry_hook(h.BASE + 0x1234))

if __name__ == '__main__': unittest.main()
