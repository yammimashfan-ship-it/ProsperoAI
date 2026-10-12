# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RemoteModelTests(unittest.TestCase):
    def test_discovery_and_streamed_replies(self):
        compiler = os.environ.get("HOST_CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler or not (ROOT / ".deps/llama.cpp/vendor/nlohmann/json.hpp").exists():
            self.skipTest("C++ compiler and make deps required")
        with tempfile.TemporaryDirectory(prefix="prospero-remote-") as directory:
            binary = Path(directory) / "remote"
            subprocess.run([compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
                            "-I" + str(ROOT / "vulkan"), "-I" + str(ROOT / "include"),
                            "-I" + str(ROOT / ".deps/llama.cpp/vendor"),
                            str(ROOT / "tests/remote_model_test.cpp"),
                            str(ROOT / "vulkan/remote_model.cpp"), "-o", str(binary)],
                           check=True)
            subprocess.run([str(binary), str(Path(directory) / "models")], check=True)


if __name__ == "__main__":
    unittest.main()
