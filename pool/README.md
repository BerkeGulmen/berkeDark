# berkeDark pool

One source for what berkeDark tells apart and how each thing looks: in an editor, a terminal, a shell
prompt, and in a conversation with an AI tool. It is general on purpose. It holds classes that some
tool cannot show; a tool takes the classes it can show and falls back for the rest.

State: version 0.3.0. Judged by the author on 2026-11-10, adapted to Claude Code, and since then the single
source: every file of berkeDark that carries a colour is written from this pool.

## One source, many files

    python3 pool/tools/generate.py          writes the targets
    python3 pool/tools/generate.py check    says which targets differ from what the pool gives
    python3 pool/tools/generate.py where sky    lists the places that use a colour or a class

`targets/` holds one template for each target, at the target's own path: the VS Code theme, the browser
theme, and for the terminal the interpreter's colours, the foot palette, Claude Code's theme, the shell
prompt and the palette sheet. A template is the target file with a reference in place of each colour
(`@{prose.label}`, `@{bg:diff.added}`, `@{=sky}`, `@{kept:browser:frame}`, `@{mark:prose.task.open}`;
the forms are in `generate.py`). To change a look, change the class or the colour in `pool.json` and
generate; a target is never edited by hand, and the terminal's tests fail when one differs from the pool.

What is not generated: the words of the README files (they name colours in prose), and the styles
(bold, italic) inside the templates, which stand there as written.

## Files

- `pool.json`: the source. Edited by hand.
  - `palette`: the colours by name. A class names a colour, never a number.
  - `roles`: the classes. `fg` and `bg` name palette colours, `style` lists bold, italic, underline,
    strike, and `fallback` is the class a tool uses when it cannot show this one.
  - `state` of a class: `decided` (the author's decision, with its source), `in-use` (drawn today
    without a recorded decision of its own), `proposal` (added here, awaiting the author), `open`
    (two decisions meet in one class, or the value is in doubt).
  - `alt`: other values on record for the same class.
  - `claude-code`: how Claude Code could show the class.
  - `kept`: decisions of the published theme not yet sorted into classes (155 workbench colours,
    the browser theme, two of Claude Code's). A value is a palette name, `name/alpha`, `role:CLASS`,
    `bg:CLASS`, or a number where no name exists yet.
- `tools/pool.py`: `check` (faults, classes drawn alike, low contrast), `resolve` (writes
  `build/pool.resolved.json`, every class as plain values), `open` (what awaits the author), `list`.
- `tools/generate.py`: writes the targets from the pool, checks them, finds where a colour is used.
- `tools/preview.py`: `catalogue` draws every class in its own look; `sample FILE` draws a text
  marked with classes. Needs Pillow and the terminal's font.
- `samples/`: the marked texts. `prose-options.txt` puts the candidates of each open choice side by side.
- `build/`: generated pictures and the resolved file.

## Domains

`base` canvas and text levels, `ansi` the terminal's sixteen named colours, `prose` text that is read,
`command` the words of a command line, `shell` the prompt and file listings, `code` program text,
`diff` and `vcs` changes, `status` what a message means, `chat` a conversation with an assistant,
`ui` windows and lists.

## Order of work

1. The pool is judged by the author.
2. It is adapted to Claude Code first (theme file and the colour interpreter).
3. Then to the other AI tools, then the editor and the browser are generated from it.
