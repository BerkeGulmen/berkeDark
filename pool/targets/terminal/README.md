# berkeDark for the terminal

berkeDark is a colour theme family: VS Code, the browser, and now the terminal.

A terminal theme alone can only set the window's colours. A program says "italic" or
"strikethrough" with a style mark that carries no colour, and neither the terminal nor
the program offers a setting for it. `berkeDark` is a small colour interpreter that closes
that gap: it runs a program in a pseudo-terminal of its own, reads what the program prints
and adds the colour right after such a mark. Everything else passes through byte for byte.

Design rule: different things look different, in colours that stay in harmony.

Plain C with the C library only. Build with clang:

    sh build.sh            # build/berkeDark
    sh build.sh install    # also ~/.local/bin/berkeDark

Run:

    berkeDark              # your shell inside the interpreter
    berkeDark vim notes    # any program
    berkeDark --filter     # standard input to standard output

Colours live in `src/colours.h`: italic, bold italic, strikethrough, and top headings
(bold, italic and underlined at once). Text a program coloured itself keeps its colour, and
so does dim text.

To start it in every new foot window, add this to `foot.ini`; without the program the shell
simply starts plainly:

    shell=/bin/sh -c '[ -x "$HOME/.local/bin/berkeDark" ] && exec "$HOME/.local/bin/berkeDark" /bin/zsh; exec /bin/zsh'

## Claude Code

berkeDark gives Claude Code's full-screen interface the same look. This is an independent
project, not affiliated with Anthropic. One command sets it up:

    sh install.sh

It builds the interpreter, installs Claude Code's berkeDark theme, sets `"theme":
"custom:berkedark"` and `"tui": "fullscreen"` in Claude Code's settings and, if you use foot,
adds one include line to `foot.ini`. Fullscreen changes how Claude Code uses the terminal; the
berkeDark look needs it. A file it replaces is kept once as `<file>.bak-berkedark`. Open a new
window afterwards: a window already open keeps what it had. Without foot, or with
`--no-foot`, start it as `berkeDark claude`.

What you get:

- The rows you sent stand in a box, with a white `▶` at the start. The input row keeps `❯`,
  in white, and grey while Claude works.
- Diffs take berkeDark's syntax colours: keywords `@{code.keyword}`, numbers `@{code.number}`, strings
  `@{code.string}`, function and class names `@{code.function.definition}`, punctuation in plain code `@{code.punctuation}`.
- In plain text, numbers standing alone are `@{prose.number}`, dates and times `@{prose.date}`. Names such as
  `v10` or `a_9.tex`, tool summaries and grey status rows keep their colour.
- The text you type into the input box is `@{chat.user.input.text}`, so it stands apart from the reply text. The hint
  of an empty box stays dim, and shell mode (`!`) keeps Claude Code's own look.
- In a reply's text, things Claude Code draws in one colour get their own:
  - a row that is bold from end to end, a heading line, is `@{prose.heading.line}`; headings of level 2 to 4 arrive as such a row
  - a label, bold words that open a row or a list item and end with a colon, is `@{prose.label}`
  - the hyphen or the number before a list item is `@{prose.list.bullet}`
  - a task's box becomes `○` in `@{prose.task.open}` or `✓` in `@{prose.task.done}`, right before the task's text
  - a web address standing alone is `@{prose.link.bare}`; one in brackets after words, a link's address, is `@{prose.link.url}`
  - a table's lines are `@{prose.table.border}`, the names of its columns `@{prose.table.header}` and bold
  - a rule, written as three hyphens, is drawn as a line across the text in `@{prose.rule}`
  - the text beside a quote bar is `@{prose.quote.text}`
- Code in a reply takes berkeDark's colours:
  - inline code, and a block with no language, is `@{prose.code.inline}`; a word of inline code that is the path of a file is `@{prose.path}`
  - in a block with a language: keywords `@{code.keyword}`, `true`, `null` and their kin `@{code.constant.language}`, the name of a class
    `@{code.class}`, the name of a function `@{code.function.definition}`, types and shell commands `@{code.type}`, a built-in function that is
    called `@{code.function.call}`, keys of JSON and YAML `@{code.property.key}`, strings `@{code.string}`, numbers `@{code.number}`, comments `@{code.comment}`
    and slanted, decorators and a preprocessor's own words `@{code.decorator}`, the signs of plain code `@{code.punctuation}`
- A diff's rows stand on the theme's grounds: added `@{bg:diff.added}`, removed `@{bg:diff.removed}`, their changed words `@{bg:diff.added.word}`
  and `@{bg:diff.removed.word}`. Claude Code names these in its theme but draws all of them grey with the dark-ansi base.
- In a diff, an apostrophe inside a comment of several rows no longer turns the rest of the row into a "string".
- A command line shows its parts: in shell mode (`!`), in the grey line of a running tool and in a `Bash(...)`
  heading. The name of each command is `@{command.name}`, an option `@{command.option}` and slanted, quoted text `@{command.string}`, a
  variable `@{command.variable}`, a path `@{command.path}`, a number `@{command.number}`, the signs that join commands `@{command.operator}`.
- The dim words offered in the input box are `@{chat.user.input.suggestion}`; a path in a tool's heading is `@{prose.path}`.

Numbers, dates and times get these colours in plain text of every full-screen program (`less`,
`htop` and so on). The marks, the diff colours and the reply text apply only when the program is
named `claude`. The colours are in `src/colours.h`.

Tested on Void Linux with foot 1.28 and Claude Code 2.1.322. Not tested: other distributions,
other terminals and other ways of installing Claude Code. macOS and Windows are not supported.

berkeDark recognises these things from what Claude Code draws, so a later version may lose
some of the colours or put one in the wrong place until berkeDark is updated. Words and
numbers are always the ones Claude Code wrote; only `❯` on a sent row is replaced, by `▶`,
a task's box by its mark, and a rule's three hyphens by a line.
A reply line that begins with a tool's word (Read, Found and the like) keeps its numbers plain.

Limits of the reply text: Claude Code draws no frame around a block of code, so the rows of a
block that its highlighter leaves without colour read as reply text, and a hyphen at the start of
such a row or an address in it gets the same colour; a box drawn with line characters gets the
colour of a table's lines. An address
in brackets cannot be told from a link's address. A long address broken at the end of a row keeps
its second part plain. The words of a link carry no mark and stay as they are. The reply's first
row is known by the colour of its mark, the theme's text colour: with another theme, change
`REPLY_MARK_FOREGROUND` in `src/colours.h`.

Limits of the code colours: Claude Code's highlighter tells few things apart, so parameters,
variables, methods and operators keep the text colour, and a docstring is a string. A built-in name
that is not called counts as a type. The path of a file is found by its shape: a name with one
folder and no known kind of file is not taken for one. foot draws bold text of a regular palette
colour in the bright one, so bold inline code and a bold path are written one step off their colour.

To remove it: delete `~/.local/bin/berkeDark`, `~/.claude/themes/berkedark.json` and
`~/.config/foot/berkeDark.ini`, delete the last two lines of `foot.ini` (`[main]` and the
`include` line), and take `theme` and `tui` out of Claude Code's settings or restore the
`.bak-berkedark` copies.

- `src/` the interpreter
- `tests/run_tests.py` stream and pseudo-terminal tests, `tests/sample.sh` a look at every style,
  `tests/palette.py` every berkeDark colour with its hex value
- `oh-my-zsh/` the berkeDark prompt theme
- `install.sh` the one-command setup
- `config/` colour files for foot and Claude Code

Copyright Berke GÜLMEN. The licence is berkeDark's: see `LICENSE` in the repository's root.
