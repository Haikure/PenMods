#!/usr/bin/env python3
"""Host-only packaging and first-migration regression tests; no device writes."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

TOOL = Path(__file__).with_name("package_update.py").resolve()
INSTALLER = TOOL.parent / "update/install-launcher.sh"
LAYOUTS = {
    "core": ["libPenMods.so", "libPenModsResources.so"],
    "player": ["FFmpegPlayer/libffmpegplayerplugin.so", "FFmpegPlayer/qmldir"],
    "mathjax": ["mathjax-server"],
    "ppocr": ["libyocr.so", "models/PP_OCRv5_mobile_rec.ncnn.param", "models/PP_OCRv5_mobile_rec.ncnn.bin"],
}


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="penmods-package-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def source(self, component):
        source = self.root / component
        source.mkdir(exist_ok=True)
        for name in LAYOUTS[component]:
            target = source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            payload = (name + "\n").encode() * 32
            if name.endswith(".so") or name == "mathjax-server":
                header = bytearray(64)
                header[:6] = b"\x7fELF\x02\x01"
                header[16] = 3 if name.endswith(".so") else 2
                header[18] = 183
                payload = header + payload
            target.write_bytes(payload)
        return source

    def package(self, component="core", extra=(), expect=True):
        source = self.root / component
        if not source.exists():
            self.source(component)
        command = [sys.executable, "-B", str(TOOL), "--component", component,
                   "--version", "2.7.0", "--source", str(source),
                   "--output", str(self.root / "package.zip"),
                   "--descriptor", str(self.root / "catalog.json"),
                   "--download", "https://example.org/package.zip", "--model", "YDP02X", "--channel", "dev"]
        result = subprocess.run(command + list(extra), text=True, capture_output=True)
        if expect:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def test_all_layouts_hashes_permissions_and_determinism(self):
        for component, names in LAYOUTS.items():
            with self.subTest(component=component):
                first = self.package(component)
                payload = (self.root / "package.zip").read_bytes()
                self.assertEqual(first["sha256"], hashlib.sha256(payload).hexdigest())
                self.assertEqual(first["size"], len(payload))
                with zipfile.ZipFile(self.root / "package.zip") as archive:
                    self.assertEqual(archive.namelist(), sorted(names))
                    self.assertEqual(first["unpacked_size"], sum(i.file_size for i in archive.infolist()))
                    for entry in archive.infolist():
                        self.assertEqual(entry.compress_type, zipfile.ZIP_DEFLATED)
                        self.assertEqual(entry.date_time, (1980, 1, 1, 0, 0, 0))
                        self.assertEqual((entry.external_attr >> 16) & 0o777,
                                         0o755 if entry.filename == "mathjax-server" else 0o644)
                if component == "core":
                    self.assertEqual(first["health_protocol"], 1)
                    self.assertEqual(first["md5"], hashlib.md5(payload).hexdigest())
                self.assertEqual(first, self.package(component))
                self.assertEqual(payload, (self.root / "package.zip").read_bytes())
                self.assertEqual(dict(first, version="2.7.0"), json.loads((self.root / "catalog.json").read_text()))

    def test_missing_resource_library_and_wrong_ocr_model_are_rejected(self):
        source = self.source("core")
        (source / "libPenModsResources.so").unlink()
        self.package(expect=False)
        source = self.source("ppocr")
        for name in ("param", "bin"):
            (source / f"models/PP_OCRv5_mobile_rec.ncnn.{name}").rename(source / f"models/other.ncnn.{name}")
        self.package("ppocr", expect=False)

    def test_symlinks_special_files_urls_and_bad_requirements_are_rejected(self):
        source = self.source("core")
        (source / "link").symlink_to(source / "libPenMods.so")
        self.package(expect=False)
        (source / "link").unlink()
        os.mkfifo(source / "fifo")
        self.package(expect=False)
        (source / "fifo").unlink()
        for address in ("http://example.org/a", "https://user:pass@example.org/a", "https://example.org/a#fragment"):
            self.package(extra=("--download", address), expect=False)
        requirements = self.root / "requires.json"
        for value in (["not an object"], {"core": {"min": "not-a-version"}}, {"core": {"shell": "run"}}):
            requirements.write_text(json.dumps(value))
            self.package(extra=("--requires", str(requirements)), expect=False)

    def test_oversized_sparse_file_is_rejected_before_compression(self):
        source = self.source("core")
        with (source / "libPenMods.so").open("r+b") as stream:
            stream.truncate(192 * 1024 * 1024 + 1)
        result = self.package(expect=False)
        self.assertIn("192 MiB", result.stderr)

    def bootstrap(self, helper=None):
        fake = self.root / "fake-helper"
        fake.write_text("#!/bin/sh\nset -eu\nprintf '%s\\n' \"$*\" >> \"$BOOTSTRAP_LOG\"\ncase \"$*\" in *' init '*) [ \"\u0024{FAIL_INIT:-0}\" = 0 ] ;; *' stage '*) [ \"\u0024{FAIL_STAGE:-0}\" = 0 ] ;; esac\n")
        helper = Path(helper) if helper else fake
        return self.package(extra=("--bootstrap-helper", str(helper), "--bootstrap-installer", str(INSTALLER),
                                   "--bootstrap-output", str(self.root / "bootstrap.zip"),
                                   "--bootstrap-download", "https://example.org/bootstrap.zip",
                                   "--bootstrap-descriptor", str(self.root / "bootstrap.json")))

    def extract_bootstrap(self, helper=None):
        metadata = self.bootstrap(helper)
        path = self.root / "bootstrap.zip"
        data = path.read_bytes()
        self.assertEqual(metadata["bootstrap"]["sha256"], hashlib.sha256(data).hexdigest())
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(set(archive.namelist()), {"_do_update.sh", "penmods-update", "install-launcher.sh", "core-package.zip", "core-descriptor.json"})
            descriptor = json.loads(archive.read("core-descriptor.json"))
            self.assertEqual(descriptor["version"], "2.7.0")
            self.assertEqual(descriptor["health_protocol"], 1)
            stage = self.root / "unpacked"
            archive.extractall(stage)
        return stage

    def migration(self, stage, fail_init=False, fail_stage=False, known_layout=True, disabled=False):
        prefix = self.root / "filesystem"
        launcher = prefix / "usr/bin/runDictPen"
        launcher.parent.mkdir(parents=True, exist_ok=True)
        launcher.write_text('#!/bin/sh\n' + ('stdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1\n' if known_layout else 'unknown launcher\n'))
        module = prefix / "userdata/PenMods"
        module.mkdir(parents=True, exist_ok=True)
        library = module / "libPenMods.so"
        baseline = (self.root / "core/libPenMods.so").read_bytes() + b"old running main library"
        if library.is_symlink():
            library.unlink()
        library.write_bytes(baseline)
        if disabled:
            marker = prefix / "userdisk/PenMods/update-system/disabled"
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.write_text("uninstalled\n")
        sync = self.root / "bin/sync"
        sync.parent.mkdir(exist_ok=True)
        sync.write_text("#!/bin/sh\nexit 0\n")
        sync.chmod(0o755)
        log = self.root / "helper-calls"
        env = dict(os.environ, PENMODS_BOOTSTRAP_PREFIX=str(prefix), BOOTSTRAP_LOG=str(log),
                   FAIL_INIT=str(int(fail_init)), FAIL_STAGE=str(int(fail_stage)),
                   PATH=str(sync.parent) + ":" + os.environ["PATH"])
        # A stale marker from an earlier old updater extraction must never report success.
        (stage / "INSTALL_SUCCESSFULLY").touch()
        result = subprocess.run(["sh", str(stage / "_do_update.sh")], text=True, capture_output=True, env=env)
        self.assertEqual(library.read_bytes(), baseline)
        self.assertFalse(library.is_symlink())
        return result, launcher, log

    def test_bootstrap_migration_is_deterministic_and_never_overwrites_main_library(self):
        stage = self.extract_bootstrap()
        before = (self.root / "bootstrap.zip").read_bytes()
        self.bootstrap()
        self.assertEqual(before, (self.root / "bootstrap.zip").read_bytes())
        result, launcher, log = self.migration(stage)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((stage / "INSTALL_SUCCESSFULLY").exists())
        calls = log.read_text().splitlines()
        self.assertIn("init --snapshot-only", calls[0])
        self.assertIn(" stage ", calls[1])
        self.assertIn("penmods-update run --", launcher.read_text())
        self.assertTrue(launcher.with_name("runDictPen.pre-penmods-update").exists())

    def test_bootstrap_failures_do_not_hook_or_leave_success_marker(self):
        stage = self.extract_bootstrap()
        for init, staging in ((True, False), (False, True)):
            with self.subTest(init=init, staging=staging):
                result, launcher, _ = self.migration(stage, fail_init=init, fail_stage=staging)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("penmods-update run --", launcher.read_text())
                self.assertFalse((stage / "INSTALL_SUCCESSFULLY").exists())
        result, launcher, _ = self.migration(stage, known_layout=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(launcher.read_text(), "#!/bin/sh\nunknown launcher\n")
        self.assertFalse((stage / "INSTALL_SUCCESSFULLY").exists())


    @unittest.skipUnless(os.environ.get("PENMODS_UPDATE_TEST_HELPER"), "set PENMODS_UPDATE_TEST_HELPER to built host helper")
    def test_native_bootstrap_stages_without_switching_active_files(self):
        helper = Path(os.environ["PENMODS_UPDATE_TEST_HELPER"]).resolve()
        stage = self.extract_bootstrap(helper)
        result, launcher, _ = self.migration(stage)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((stage / "INSTALL_SUCCESSFULLY").exists())
        store = self.root / "filesystem/userdisk/PenMods/update-system"
        status = subprocess.run([str(helper), "--root", str(store), "--prefix", str(self.root / "filesystem"), "status"],
                                text=True, capture_output=True, check=True)
        status = json.loads(status.stdout)
        self.assertEqual(status["components"]["core"]["version"], "0.0.0")
        self.assertEqual(status["pending_components"]["core"]["version"], "2.7.0")
        self.assertIsNotNone(status["pending"])
        self.assertIsNone(status["trial"])
        self.assertFalse((store / "active").exists())
        self.assertIn("penmods-update run --", launcher.read_text())


    def test_reinstall_keeps_bypass_until_staging_and_hook_succeed(self):
        stage = self.extract_bootstrap()
        marker = self.root / "filesystem/userdisk/PenMods/update-system/disabled"
        for init, staging, layout in ((True, False, True), (False, True, True), (False, False, False)):
            with self.subTest(init=init, stage=staging, layout=layout):
                result, _, log = self.migration(stage, fail_init=init, fail_stage=staging, known_layout=layout, disabled=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(marker.exists())
                self.assertFalse((stage / "INSTALL_SUCCESSFULLY").exists())
                self.assertIn("init --reinstall-snapshot", log.read_text())
                self.assertNotIn(" enable", log.read_text())

    @unittest.skipUnless(os.environ.get("PENMODS_UPDATE_TEST_HELPER"), "set PENMODS_UPDATE_TEST_HELPER to built host helper")
    def test_native_reinstall_rearms_only_after_new_snapshot_stage_and_hook(self):
        helper = Path(os.environ["PENMODS_UPDATE_TEST_HELPER"]).resolve()
        stage = self.extract_bootstrap(helper)
        result, _, _ = self.migration(stage)
        self.assertEqual(result.returncode, 0, result.stderr)
        prefix = self.root / "filesystem"
        store = prefix / "userdisk/PenMods/update-system"
        old = json.loads(subprocess.check_output([str(helper), "--root", str(store), "--prefix", str(prefix), "status"], text=True))
        result, _, _ = self.migration(stage, disabled=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        state = json.loads(subprocess.check_output([str(helper), "--root", str(store), "--prefix", str(prefix), "status"], text=True))
        self.assertFalse(state["disabled"])
        self.assertNotEqual(state["active"], old["active"])
        self.assertIsNotNone(state["pending"])
        self.assertEqual(state["pending_components"]["core"]["version"], "2.7.0")
        self.assertIsNone(state["trial"])
        self.assertFalse((store / "active").exists())
        self.assertTrue((stage / "INSTALL_SUCCESSFULLY").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
