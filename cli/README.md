# berkeDark for AI command-line tools

The files here are written from the pool (`../pool`), like every other file of berkeDark that carries a
colour. Each tool takes the classes it can show; what it cannot show is listed below.

## Claude Code

See `../terminal`: a theme file and the colour interpreter, set up with one command.

## Qwen Code

    cp qwen/berkeDark.json ~/.qwen/berkeDark.json

and in `~/.qwen/settings.json` (the path must lie inside your home folder):

    "ui": { "theme": "/home/you/.qwen/berkeDark.json" }

Qwen Code's theme has about fifteen colours and ties some things together: a number in code takes the
colour of success, a string the colour of a warning, the key of a mapping the colour of inline code.
Headings and list marks have no colour of their own. Plain text is left in the terminal's own colour.

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

Tested with Qwen Code 0.27.1 and Codex 0.167.0 on Void Linux in foot. Grok 1.0.77 was not run for this.
