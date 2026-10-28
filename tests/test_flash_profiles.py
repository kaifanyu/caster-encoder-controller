import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts import flash_windows


class FlashProfileTests(unittest.TestCase):
    def test_windows_flash_uses_each_wheel_profile_and_rejects_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            python = repo / "python.exe"
            esptool = repo / "esptool.py"
            python.touch()
            esptool.touch()
            with patch.object(flash_windows, "__file__", str(repo / "scripts" / "flash_windows.py")), \
                    patch.object(flash_windows, "windows_path", side_effect=str), \
                    patch.object(flash_windows, "linux_path", side_effect=Path), \
                    patch.object(flash_windows.subprocess, "run") as run:
                for wheel in ("front_left", "front_right"):
                    for role in ("H1", "H2", "CENTER", "ORDINARY", "SWIVEL", "WHEEL"):
                        build = (repo / "build" / "ordinary_caster" / wheel if role == "ORDINARY"
                                 else repo / "build" / wheel / role)
                        if role in ("SWIVEL", "WHEEL"):
                            build = repo / "build/ordinary_caster" / wheel / role
                        build.mkdir(parents=True, exist_ok=True)
                        cache = build / "CMakeCache.txt"
                        cache.write_text(f"CASTER_WHEEL:STRING={wheel}\nCASTER_ROLE:STRING={role}\n")
                        binary = build / "application.bin"
                        binary.touch()
                        (build / "flasher_args.json").write_text(json.dumps({
                            "extra_esptool_args": {"chip": "esp32c6"},
                            "write_flash_args": ["--flash_mode", "dio"],
                            "flash_files": {"0x10000": binary.name},
                        }))
                        args = [wheel, role, "--port", "COM6", "--windows-python", str(python),
                                "--esptool-path", str(esptool)]
                        output = io.StringIO()
                        with contextlib.redirect_stdout(output):
                            flash_windows.main([*args, "--dry-run"])
                        run.assert_not_called()
                        self.assertIn(str(binary), output.getvalue())
                        with contextlib.redirect_stdout(io.StringIO()):
                            flash_windows.main(args)
                        self.assertEqual(run.call_args_list[0].args[0],
                                         [str(repo / "scripts/idf.sh"), wheel, role, "build"])
                        self.assertIn(str(binary), run.call_args_list[1].args[0])
                        run.reset_mock()
                        # Even a matching wheel must not flash another family's image.
                        wrong_role = "CENTER" if role == "ORDINARY" else "ORDINARY"
                        cache.write_text(f"CASTER_WHEEL:STRING={wheel}\nCASTER_ROLE:STRING={wrong_role}\n")
                        with self.assertRaisesRegex(ValueError, "profile does not match"):
                            flash_windows.main([*args, "--skip-build"])
                        run.assert_not_called()
                        other_wheel = "front_right" if wheel == "front_left" else "front_left"
                        cache.write_text(f"CASTER_WHEEL:STRING={other_wheel}\nCASTER_ROLE:STRING={role}\n")
                        with self.assertRaisesRegex(ValueError, "profile does not match"):
                            flash_windows.main([*args, "--dry-run"])
                        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
