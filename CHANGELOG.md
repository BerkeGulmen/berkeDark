# Changelog

All notable changes to the "berkeDark" extension will be documented in this file.

## [Unreleased]

## [1.3.0] - 2026-11-10

### Added
- One source: every file that carries a colour (this theme, the browser theme, the terminal's files, the AI tools' themes, the colour codes in the README files) is written from `pool/pool.json` by `pool/tools/generate.py`
- Claude Code, in the terminal: a reply's text gets berkeDark's colours for heading lines, labels, list marks, task marks, web addresses, tables, rules and quotes; for inline code, the paths of files and the words of fenced code; for command lines in shell mode and in tool rows; and for the dim words offered in the input box
- Claude Code: a diff's added and removed rows stand on the theme's own grounds
- Themes for Qwen Code and Codex in `cli`, and the setting that gives Grok the terminal's colours

### Changed
- Punctuation in code is ice blue (`#b2d9ff`), as on the terminal
- The integrated terminal's sixteen colours are the same as the terminal palette's
- The cursor is orange (`#ff9e60`), as in the terminal
- Claude Code: dates and times are `#e87fe0`; the working mark, errors and a removed row's mark are rose `#ff5c7c`; the text typed into the input box is `#3fff7f`

### Fixed
- Terminal colour interpreter: the digit of an older number could be written back over the blank between two words
- Terminal colour interpreter: the cells of emoji sequences are counted as the terminal counts them
- Terminal colour interpreter: in a diff, an apostrophe inside a comment of several rows no longer starts a "string"
- The README named a wrong colour for comments (`#7f7f7f`); it is `#707070`

## [1.2.1] - 2026-11-09

### Changed
- The README's screenshots now show the current theme, and the repeated Python preview is gone

### Fixed
- Terminal colour interpreter: writes outside its arrays with insert and delete line below the scrolling region, and overlong numeric parameters
- Terminal colour interpreter: follows the terminal more exactly with cursor save and restore, automatic wrapping off, invalid scrolling regions and origin mode

## [1.2.0] - 2026-11-08

### Added
- Claude Code: berkeDark's look for its full-screen interface, set up by `terminal/install.sh`: syntax colours in diffs, a box and a white mark on the rows you sent
- Numbers, dates and times in plain text get their own colours in full-screen programs
- oh-my-zsh theme in `terminal/oh-my-zsh`

### Changed
- The terminal colour interpreter applies its Claude Code marks and diff colours only under Claude Code

## [1.1.0] - 2026-11-07

### Added
- berkeDark is now a family: the browser theme and the terminal colour interpreter live in this repository

### Changed
- Refined accent tones and moved the interface greys closer to the deep blue base
- New license terms: PolyForm Noncommercial 1.0.0, PolyForm Internal Use 1.0.0 or a commercial license. Versions up to 1.0.11 stay under the AGPL-3.0

### Fixed
- README: palette table, settings example, installation steps, license links

## [1.0.11] - 2026-06-12

### Changed
- Refined editor foreground to a warm cream (`#ffe8c2`) for improved readability
- Updated terminal background and ANSI black to a deeper tone (`#060d08`)
- Aligned terminal foreground with the new editor foreground (`#ffe8c2`)

## [1.0.0] - 2024

### Added
- Initial release
- Dark theme inspired by Monokai Pro
- Complete workbench color theming
- Syntax highlighting for major languages
- Semantic highlighting support
- Terminal ANSI color support
- Git decoration colors
- Bracket pair colorization
