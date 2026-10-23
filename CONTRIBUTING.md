# Contributing

Thanks for looking at the project. Bug reports, fixes and ideas are welcome as issues and pull requests.

## Building and testing
See the [README](README.md) for the build and the three test suites. Before opening a pull request, build Debug and Release x64 and run:

```
bin\x64\Debug\HarnessTests.exe -tse=windowed,audio
set HARNESS_EXE=%CD%\bin\x64\Debug\AppleWinHarness.exe
python -m unittest discover -s tests/python
python -m unittest discover -s tests/adapter
```

CI runs the same commands on every push and pull request. Behaviour changes should come with a test; the design records are in [docs/decisions.md](docs/decisions.md) and the protocol is in [docs/api_spec.md](docs/api_spec.md).

## Licensing of contributions
The project is split (decision D6, [LICENSE](LICENSE)):

- Everything you write goes in under the **MIT License**, and each source file starts with `SPDX-License-Identifier: MIT`. By sending a contribution you agree to license it that way.
- Code **copied from AppleWin** (`external/AppleWin`) is GPL-2.0-or-later, not MIT. If you copy even a few lines, say where from in a comment, keep AppleWin's copyright, set the file's SPDX header to `GPL-2.0-or-later` (or `MIT AND GPL-2.0-or-later` if most of the file is yours), and add the file to `APPLEWIN_DERIVED` in `tests/python/test_licensing.py`.
- Do not add disk images, ROM images or other copyrighted game or system files. The tests use the images that ship in the AppleWin submodule.
- A new third-party dependency needs its license text in [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

## Notes
- The `external/AppleWin` submodule is pinned and unmodified; the harness does not patch AppleWin.
- Development used AI coding assistants. Their configuration is not part of the repository.
- Optional: the `cpp-pro` Claude Code skill from [Jeffallan/claude-skills](https://github.com/Jeffallan/claude-skills) (MIT) was used while writing the C++.
