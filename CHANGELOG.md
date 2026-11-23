# Changelog

All notable changes to this project are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Analysis of the USA `default.xbe`: sections, XDK libraries, kernel imports and functions.
- Comparison with the first *Otogi*: shared kernel imports, shared library code, and where the first game's fixes land in this one (`scripts/xbe_compare.py`).
- A check that no game file or personal detail is ever committed (`scripts/check_no_game_data.py`, run on every push).
