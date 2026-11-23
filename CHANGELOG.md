# Changelog

All notable changes to the "berkeDark" extension will be documented in this file.

## [1.4.1] - 2026-11-23

### Added
- README: before-and-after pictures for Claude Code (a reply and an edit) and Codex, placed before those of Qwen Code and Grok

## [1.4.0] - 2026-11-18

### Added
- `berkedark`: one command that installs, checks and uninstalls berkeDark for every surface (foot and the colour interpreter, Claude Code, Qwen Code, Codex, Grok, oh-my-zsh). It changes only berkeDark's own keys in settings files, checks what it wrote (valid JSON and TOML, foot's `--check-config`), changes nothing on a second run, and has `--dry-run` and `status`; tested by `tests/installer.sh`
- A settings file of your own, `~/.config/berkeDark/berkeDark.toml` (`./berkedark settings`): change a palette colour or the colour of one class, and every install writes berkeDark's files from the pool with your changes
- What uninstall guarantees:
  - every file it touches is kept as it was, once, the first time it changes it; a file untouched since the install comes back byte for byte, a file berkeDark placed goes
  - in a settings file only berkeDark's keys go back to what they were, so what the tools wrote since (Codex's project trust, a sign-in, a model) stays, and a key you changed yourself stays as you set it; a settings file berkeDark made goes only when nothing else is left in it
  - a file changed since the install is saved beside it first, with the date and time in its name
  - a file it cannot put back (its folder removed, read-only) is named, the others are still done, and running uninstall again finishes; the record is saved after every file, and a kept copy goes only once the record no longer names it
  - links stay links (symbolic and hard), line ends, modes and JSON indentation are kept, new files follow the umask; read-only, non-UTF-8 and mixed line-end files are left alone with a message
- foot: berkeDark's include carries only its colours, its cursor and the line that starts your own shell (foot.ini's `shell=`, also in a file it includes, else `$SHELL`) inside the colour interpreter; no font, padding or scrollback, and a `foot.ini` it creates includes the system's first
- `cli/qwen/reapply-diff-color-patch.py` (`./berkedark qwen-diff-patch`): makes Qwen Code's ```diff blocks readable with a custom theme; it finds first the copy the `qwen` command runs, keeps the original, checks the result with `node --check`, and is run again after each Qwen Code update
- README: before-and-after pictures for Qwen Code and Grok

### Fixed
- Qwen Code: the theme names berkeDark's text colour as Foreground; with an empty one Qwen Code's OpenTUI interface drew text in its own grey (`#cdd6f4`)
- Qwen Code, through the colour interpreter: its own drawn cursor (a light grey cell) gives way to the terminal's orange cursor, or to an underline in the cursor's colour; a request for a block cursor is followed by the terminal's own cursor style
- Qwen Code, through the colour interpreter: its selection (white on `#264f78`, not themable) takes berkeDark's selection ground (`#252261`) with the text colour
- Grok, through the colour interpreter: your messages stand on berkeDark's band (`#260307`) inside its box, as under Claude Code; their words are turquoise (`#6fdcf0`), the sent mark is the white `▶`, numbers and dates keep their colours
- Grok: what you type in the input box is spring green (`#3fff7f`), and its secondary text (notices such as "Worked for", status and key hints, the input box's hint) takes the faint grey (`#6e7380`) instead of a faint cream
- Grok: code and diff blocks take the pool's classes (numbers magenta, keywords sky, the added row green), also across empty lines; the diff of an edit stands on the diff grounds; headings, inline code, paths, links, list marks, task boxes, quotes and tables take the pool's classes as in Claude Code; `TERM` stays as it is
- Terminal colour interpreter, outside Claude Code: text in exactly berkeDark's text colour counts as plain text (numbers, dates, styles)

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
