# SPDX-License-Identifier: MIT
"""Licensing (D6) and the release bundle layout (spec: docs/specs/m6.md pass conditions 7 and 8; plan Review Focus 5).

Run: python -m unittest discover -s tests/python -v
The bundle checks run only when HARNESS_BUNDLE_DIR is set. It names the extracted bundle: the
applewin-harness-<version>-win64 directory that holds AppleWinHarness.exe (or the directory that contains it).
"""

import os
import re
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "adapter"))   # applewin_mcp.harness_client

from applewin_mcp.harness_client import REPO_ROOT

# Files that contain code copied from AppleWin (GPL-2.0-or-later) and so are not plain MIT. Add a file here, and give it
# the matching header, when code is copied from external/AppleWin into it.
APPLEWIN_DERIVED = {
    "src/host/applewin_host.cpp": "GPL-2.0-or-later",
    "src/host/virtual_sound.cpp": "MIT AND GPL-2.0-or-later",
    "src/host/emulator.cpp": "MIT AND GPL-2.0-or-later",
}
OWNED_ROOTS = ("src", "tests", "adapter", "tools")
OWNED_SUFFIXES = (".cpp", ".h", ".py", ".ps1")
EXCLUDED_DIRS = {"third_party", "external", "fixtures", "bin", "obj", "x64", "__pycache__", "build", ".vs"}
LICENSE_FILES = ("LICENSE", "LICENSE-GPL-2.0.txt", "NOTICE.txt", "THIRD-PARTY-NOTICES.txt")
THIRD_PARTY = ("AppleWin", "zlib", "minizip", "libyaml", "nlohmann", "mcp")
FORBIDDEN_IN_BUNDLE = (".dsk", ".do", ".po", ".nib", ".woz", ".pdb", ".lib", ".obj", ".idb", ".ilk")
BUNDLE_FILES = {
    "AppleWinHarness.exe", "adapter/requirements.txt", "adapter/README.md", "adapter/VERSION", "adapter/examples/basic_count.md",
    "docs/api_spec.md", "README.md", "LICENSE", "LICENSE-GPL-2.0.txt", "NOTICE.txt", "THIRD-PARTY-NOTICES.txt",
}
BUNDLE_ADAPTER_FILES = {"adapter/applewin_mcp/" + n for n in
                        ("__init__.py", "__main__.py", "harness_client.py", "server.py")}


def owned_files():
    """Source files the harness owns: tracked or not-yet-tracked (and not ignored) files under the owned roots."""
    try:
        out = subprocess.run(["git", "-C", REPO_ROOT, "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True, timeout=60).stdout
        candidates = [p for p in out.decode("utf-8").split("\0") if p]
    except (OSError, subprocess.SubprocessError):
        candidates = []
        for root in OWNED_ROOTS:
            for dirpath, _, names in os.walk(os.path.join(REPO_ROOT, root)):
                candidates += [os.path.relpath(os.path.join(dirpath, n), REPO_ROOT) for n in names]
    found = []
    for rel in candidates:
        parts = rel.replace("\\", "/").split("/")
        if parts[0] not in OWNED_ROOTS or not rel.endswith(OWNED_SUFFIXES):
            continue
        if EXCLUDED_DIRS.intersection(parts[:-1]):
            continue
        if os.path.isfile(os.path.join(REPO_ROOT, rel)):
            found.append(rel.replace("\\", "/"))
    return sorted(found)


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def spdx_identifier(rel):
    """The SPDX-License-Identifier expression in a file's first 5 lines, or None."""
    with open(os.path.join(REPO_ROOT, rel), encoding="utf-8", errors="replace") as f:
        for _ in range(5):
            m = re.search(r"SPDX-License-Identifier: ([A-Za-z0-9.+ -]+?)\"?\s*$", f.readline())
            if m:
                return m.group(1)
    return None


class SourceHeaderTest(unittest.TestCase):
    def test_owned_files_are_found_in_every_root(self):
        files = owned_files()
        for root in ("src", "tests", "adapter"):
            self.assertTrue([f for f in files if f.startswith(root + "/")], "no owned files under " + root)

    def test_every_owned_source_file_has_an_spdx_header(self):
        missing = []
        for rel in owned_files():
            if not spdx_identifier(rel):
                missing.append(rel)
        self.assertEqual(missing, [], "{} files lack an SPDX-License-Identifier in their first 5 lines".format(len(missing)))

    def test_files_with_code_copied_from_applewin_carry_the_gpl_identifier(self):
        for rel, want in APPLEWIN_DERIVED.items():
            with self.subTest(file=rel):
                self.assertEqual(spdx_identifier(rel), want)

    def test_only_the_applewin_derived_files_are_not_plain_mit(self):
        others = {rel: spdx_identifier(rel) for rel in owned_files() if rel not in APPLEWIN_DERIVED}
        self.assertEqual({rel: ident for rel, ident in others.items() if ident != "MIT"}, {})


class LicenseFilesTest(unittest.TestCase):
    def test_license_files_exist_at_the_root(self):
        for name in LICENSE_FILES:
            with self.subTest(file=name):
                self.assertTrue(os.path.isfile(os.path.join(REPO_ROOT, name)), name + " is missing")

    def test_license_files_have_the_right_content(self):
        self.assertIn("MIT License", read_text(os.path.join(REPO_ROOT, "LICENSE")))
        gpl = read_text(os.path.join(REPO_ROOT, "LICENSE-GPL-2.0.txt"))
        self.assertIn("GNU GENERAL PUBLIC LICENSE", gpl)
        self.assertIn("Version 2", gpl)
        notices = read_text(os.path.join(REPO_ROOT, "THIRD-PARTY-NOTICES.txt"))
        for name in THIRD_PARTY:
            with self.subTest(component=name):
                self.assertIn(name, notices)

    def test_notice_gives_the_source_offer_with_the_pinned_applewin_commit(self):
        commit = subprocess.run(["git", "-C", REPO_ROOT, "rev-parse", "HEAD:external/AppleWin"],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout.strip()
        self.assertRegex(commit, r"^[0-9a-f]{40}$")
        notice = read_text(os.path.join(REPO_ROOT, "NOTICE.txt"))
        self.assertIn(commit, notice)
        self.assertIn("GPL", notice)

    def test_the_repository_tracks_no_disk_images(self):
        out = subprocess.run(["git", "-C", REPO_ROOT, "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True, timeout=60).stdout
        files = [p for p in out.decode("utf-8").split(chr(0)) if p]
        self.assertTrue(files)
        images = [p for p in files if p.lower().endswith((".dsk", ".do", ".po", ".nib", ".woz"))
                  and not p.replace("\\", "/").startswith("external/")]
        self.assertEqual(images, [], "disk images in the repository")

    def test_version_file(self):
        self.assertEqual(read_text(os.path.join(REPO_ROOT, "VERSION")).strip(), "0.1.0")


# ---- Pass conditions 7 and 8: the bundle (only when HARNESS_BUNDLE_DIR is set) ----

def bundle_dir():
    d = os.environ.get("HARNESS_BUNDLE_DIR")
    if not d:
        return None
    d = os.path.abspath(d)
    if not os.path.isfile(os.path.join(d, "AppleWinHarness.exe")):
        children = [os.path.join(d, c) for c in os.listdir(d) if os.path.isfile(os.path.join(d, c, "AppleWinHarness.exe"))]
        if len(children) == 1:
            return children[0]
    return d


@unittest.skipUnless(os.environ.get("HARNESS_BUNDLE_DIR"), "set HARNESS_BUNDLE_DIR to the extracted bundle")
class BundleTest(unittest.TestCase):
    def files(self):
        root = bundle_dir()
        out = set()
        for dirpath, dirnames, names in os.walk(root):
            dirnames[:] = [d for d in dirnames if d != "__pycache__"]
            out.update(os.path.relpath(os.path.join(dirpath, n), root).replace("\\", "/") for n in names)
        return out

    def test_the_bundle_has_exactly_the_expected_layout(self):
        files = self.files()
        self.assertTrue(BUNDLE_FILES <= files, "missing: {}".format(sorted(BUNDLE_FILES - files)))
        adapter = {f for f in files if f.startswith("adapter/applewin_mcp/")}
        self.assertTrue(BUNDLE_ADAPTER_FILES <= adapter, "missing: {}".format(sorted(BUNDLE_ADAPTER_FILES - adapter)))
        self.assertTrue(all(f.endswith(".py") for f in adapter), sorted(adapter))
        extra = files - BUNDLE_FILES - adapter
        self.assertEqual(sorted(extra), [], "files outside the layout")

    def test_the_bundle_has_no_disk_images_symbols_or_test_binaries(self):
        for f in self.files():
            with self.subTest(file=f):
                self.assertFalse(f.lower().endswith(FORBIDDEN_IN_BUNDLE), f)
                self.assertNotIn("HarnessTests", f)

    def test_the_bundle_license_files_match_the_repository(self):
        root = bundle_dir()
        for name in LICENSE_FILES:
            with self.subTest(file=name):
                got = read_text(os.path.join(root, name))
                want = read_text(os.path.join(REPO_ROOT, name))
                if name == "NOTICE.txt":   # the bundle records the release commit in place of the placeholder
                    self.assertNotIn("{{", got)
                    head, tail = want.split("{{RELEASE_COMMIT}}")
                    self.assertRegex(got, "^" + re.escape(head) + r"[0-9a-f]{40}( \(tag [^)]+\))?" + re.escape(tail) + "$")
                else:
                    self.assertEqual(got, want)

    def test_the_bundle_version_file_matches_the_repository(self):
        self.assertEqual(read_text(os.path.join(bundle_dir(), "adapter", "VERSION")).strip(),
                         read_text(os.path.join(REPO_ROOT, "VERSION")).strip())

    def test_the_example_config_points_into_the_bundle(self):
        text = read_text(os.path.join(bundle_dir(), "adapter", "examples", "basic_count.md"))
        bs = chr(92) * 2   # the config is JSON, so a backslash in a path is written twice
        self.assertIn("<BUNDLE>" + bs + "AppleWinHarness.exe", text)
        self.assertNotIn("bin" + bs + "x64" + bs + "Release", text)
        self.assertNotIn("<REPO>" + bs + "bin", text)

    def test_the_bundled_exe_is_present_and_not_trivially_small(self):
        self.assertGreater(os.path.getsize(os.path.join(bundle_dir(), "AppleWinHarness.exe")), 100000)


if __name__ == "__main__":
    unittest.main()
