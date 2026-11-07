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

## The box

In Claude Code, the rows of what you typed carry a background of their own. On a full-screen
program berkeDark keeps a small map of the screen and draws a box around such rows after
each frame. A terminal has no cell borders, so the box is made of thin block characters: on
the blank rows above and below, and in the first and last cell of each row. The prompt mark
moves one cell to the right to make room. The program's own bytes stay as they are.

- `src/` the interpreter
- `tests/run_tests.py` stream and pseudo-terminal tests, `tests/sample.sh` a look at every style,
  `tests/palette.py` every berkeDark colour with its hex value
- `config/` colour files for foot and Claude Code

Copyright Berke GÜLMEN. The licence follows the rest of berkeDark and comes with the first
release.
