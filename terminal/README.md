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
- Diffs take berkeDark's syntax colours: keywords `#57bfff`, numbers `#ff00ff`, strings
  `#ffff00`, function and class names `#3f7fff`, punctuation in plain code `#b2d9ff`.
- In plain text, numbers standing alone are `#e87fe0`, dates and times `#a3a5f5`. Names such as
  `v10` or `a_9.tex`, list numbers, tool summaries and grey status rows keep their colour.

Numbers, dates and times get these colours in plain text of every full-screen program (`less`,
`htop` and so on). The marks and the diff colours apply only when the program is named `claude`.
The colours are in `src/colours.h`.

Tested on Void Linux with foot 1.28 and Claude Code 2.1.319. Not tested: other distributions,
other terminals and other ways of installing Claude Code. macOS and Windows are not supported.

berkeDark recognises these things from what Claude Code draws, so a later version may lose
some of the colours or put one in the wrong place until berkeDark is updated. Words and
numbers are always the ones Claude Code wrote; only `❯` on a sent row is replaced, by `▶`.
A reply line that begins with a tool's word (Read, Found and the like) keeps its numbers plain.

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
- `docs/look.md` notes on the look (Turkish)

Copyright Berke GÜLMEN. The licence is berkeDark's: see `LICENSE` in the repository's root.
