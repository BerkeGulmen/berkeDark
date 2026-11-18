# berkeDark for AI command-line tools

The files here are written from the pool (`../pool`), like every other file of berkeDark that carries a
colour. Each tool takes the classes it can show; what it cannot show is listed below.

`../berkedark install qwen codex grok` places these files and sets the settings below for you, keeping what
it replaces (see the README). The steps here are the same by hand.

## Claude Code

See `../terminal`: a theme file and the colour interpreter, set up with one command.

## Qwen Code

    cp qwen/berkeDark.json ~/.qwen/berkeDark.json

and in `~/.qwen/settings.json` (the path must lie inside your home folder):

    "ui": { "theme": "/home/you/.qwen/berkeDark.json" }

Qwen Code's theme has about fifteen colours and ties some things together: a number in code takes the
colour of success, a string the colour of a warning, the key of a mapping the colour of inline code.
Headings and list marks have no colour of their own. Plain text is berkeDark's cream (`#ffe8c2`): with an
empty value Qwen Code's OpenTUI interface (the default where Qwen Code runs on Bun) drew it in its own grey.

Two faults of Qwen Code itself spoil the look; berkeDark carries a remedy for each.

**The cursor.** Qwen Code draws its own cursor as a light grey cell (`#d4d4d4`) with the character in the
text colour on it, which cannot be read in cream; its OpenTUI interface asks the terminal for a steady block
instead. Neither can be set in Qwen Code. Run Qwen Code inside berkeDark's colour interpreter
(`../terminal`, `berkeDark qwen`, or every foot window as set up there) and the terminal's own cursor comes
back, in berkeDark's foot a hollow orange box that blinks with the character readable inside. Where the
terminal's cursor is not on Qwen Code's cell, the cell is underlined in orange instead.

**```diff blocks.** For a custom theme Qwen Code draws the added and removed rows of a ```diff block as
full-width grounds in the success and error colours, with the text on them (1.18:1 on green, 2.48:1 on rose;
readable text needs 4.5:1). Its built-in themes use the same colours as text colours. The theme cannot fix
this, because the same two colours are those of success and error messages. `qwen/reapply-diff-color-patch.py`
changes those two lines in Qwen Code's installed files to the built-in form:

    python3 qwen/reapply-diff-color-patch.py --dry-run    # see what would change; nothing is changed
    python3 qwen/reapply-diff-color-patch.py              # change it, then restart Qwen Code

It looks in the global npm folder, `~/.local/lib/node_modules` and the copies Qwen Code's self-update keeps
in `~/.qwen/updates`; `--root DIR` names one copy (the folder with Qwen Code's `package.json`). It touches only
the one file that holds the two lines, keeps it as it was beside it (`<file>.berkeDark-backup`), checks the
result with `node --check` before putting it in place, and changes nothing when the lines are not found in the
expected form. A Qwen Code update brings back the original lines: **run it again after every update**. To
undo it:

    python3 qwen/reapply-diff-color-patch.py --restore

Qwen Code's file diffs (when it edits a file) are not affected: by Qwen Code's code they stand on the theme's dark DiffAdded and DiffRemoved grounds.

## Codex

    mkdir -p ~/.codex/themes && cp codex/berkedark.tmTheme ~/.codex/themes/

and in `~/.codex/config.toml`:

    [tui]
    theme = "berkedark"

Codex draws its replies and code with this syntax theme: heading levels, bold, italic, links, quotes,
list marks, inline code, and the words of code. A table's header takes the colour of a type.

## Grok

Grok takes no theme file. With

    [ui]
    theme = "terminal"

in `~/.grok/config.toml` it draws with the terminal's sixteen colours, which are berkeDark's when the
terminal uses the palette in `../terminal/config/foot.ini`.
Grok 1.0.89 also needs

    [features]
    terminal_theme = true

while the theme is being rolled out; without it `theme = "terminal"` falls back to Grok's own theme.

Measured on Grok 1.0.89 (2026-11-18): headings, bold, slanted and struck text, inline code, links, quotes,
lists, task boxes and tables take the palette's sixteen colours, and Grok leaves the terminal's cursor alone.
Two things Grok cannot be told, so berkeDark's colour interpreter (`../terminal`) does them when Grok runs
inside it (it knows Grok by its process name, as it knows Claude Code); nothing needs to change in how Grok
is started, `TERM` included:

- **Your messages on berkeDark's band.** Grok draws its band only in terminals it asks for their colours, and
  it does not ask foot, so there your messages were plain bold. The interpreter lays berkeDark's wine
  (`#260307`) under each of your messages, with the empty row above and below, across the band's width;
  numbers and times on it keep their colours. Where Grok does draw its own band, it becomes the same wine.
- **Code and diffs in berkeDark's classes.** Grok gives its code palette colours in roles of its own (numbers
  red, keywords magenta, the added row of a diff cyan). In a block of code, recognised by the grey `◇` row with
  its language under it and followed upward across empty lines while the rows are code, the interpreter gives
  them berkeDark's: keywords sky, names of functions blue, parameters orange, types spring green, strings
  yellow, keys of JSON amber, numbers magenta, constants and signs ice, comments grey and slanted; in a `diff`
  block the `+` spring green and the added text green, the `-` rose.
- **The diff of an edit** (shown under the tool's `◆` row, all red or all green) stands on berkeDark's diff
  grounds, the line numbers grey.
- **Text as in Claude Code:** a heading of level 1 yellow (bold, slanted, underlined), the other heading levels
  (Grok draws levels 2 to 4 alike) flame, inline code lavender and paths in it gold, a link's words sky and
  underlined, a link's address sky and a bare address turquoise, list marks red, task boxes `○` orange and `✓`
  green, quotes light pink and slanted, tables in the table colours.

Limits: in compact mode (`[ui] compact_mode`) Grok leaves out the `❯` and draws your message exactly like a
heading line, so it cannot be told apart and gets no band there. A file Grok reads is shown only as
"Read <file>"; there is no code to colour. Measured with Grok 1.0.89; a later Grok that draws these
differently keeps its own colours until berkeDark follows it.

Tested with Qwen Code 0.27.1 and 0.28.0 (its Ink interface under Node and its OpenTUI interface under Bun), Codex 0.167.0 and Grok 1.0.89 (answered by a local stand-in endpoint, no account) on Void Linux in foot.
