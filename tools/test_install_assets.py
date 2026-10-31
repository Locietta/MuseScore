import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import install_assets as assets


class InstallTests(unittest.TestCase):
    def test_install_verify_and_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = root / 'manifest.json'
            data = b'unchanged font/profile/instrument bytes\x00\xff'
            entry = {'source': 'fonts/test', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            manifest.write_text(json.dumps({'version': 1, 'files': {'fonts/test': entry}}))
            destination = root / 'installed'
            with patch.object(assets, 'MANIFEST', manifest), patch.object(assets.subprocess, 'check_output', return_value=data):
                assets.install(destination)
                assets.install(destination, verify=True)
                self.assertEqual((destination / 'fonts/test').read_bytes(), data)
                with self.assertRaises(FileExistsError):
                    assets.install(destination)
                (destination / 'fonts/test').write_bytes(b'corrupt')
                with self.assertRaises(ValueError):
                    assets.install(destination, verify=True)
                (destination / 'fonts/test').unlink()
                with self.assertRaises(FileNotFoundError):
                    assets.install(destination, verify=True)
            with patch.object(assets, 'MANIFEST', manifest), patch.object(assets.subprocess, 'check_output', return_value=b'corrupt'):
                with self.assertRaises(ValueError):
                    assets.install(root / 'failed')
                self.assertFalse((root / 'failed').exists())
                self.assertFalse(list(root.glob('.engrave-assets-*')))

    def test_path_validation(self):
        for name in ('', '../escape', '/absolute', 'C:/drive', 'x\\y', 'a/../../b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                assets.relative(name)


if __name__ == '__main__':
    unittest.main()
