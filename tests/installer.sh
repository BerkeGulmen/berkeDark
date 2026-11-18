#!/bin/sh
# Tests berkeDark's installer end to end in scratch home folders under a temporary folder; touches nothing else.
#   sh tests/installer.sh
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
S=$(mktemp -d "${TMPDIR:-/tmp}/berkeDark-installer-test.XXXXXX")
trap 'rm -rf "$S"' EXIT
cat > "$S/snap.py" <<'PYTHON'
import hashlib,os,sys,json
root=sys.argv[1]; out={}
for d,ds,fs in os.walk(root):
    for x in ds:
        p=os.path.join(d,x); out[os.path.relpath(p,root)+'/']=oct(os.stat(p).st_mode)
    for f in fs:
        p=os.path.join(d,f); out[os.path.relpath(p,root)]=(hashlib.sha256(open(p,'rb').read()).hexdigest()[:16],oct(os.stat(p).st_mode))
json.dump(out,open(sys.argv[2],'w'),indent=1,sort_keys=True)
PYTHON
# run HOME ARGS: the installer with nothing of the caller's environment but PATH.
# X may hold more VAR=value pairs for one call.
R() { H=$1; shift; env -i HOME="$H" PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 XDG_CONFIG_HOME="$H/.config" \
      XDG_STATE_HOME="$H/.local/state" ${X:-} python3 -I "$REPO/berkedark" "$@"; }
# setup HOME: settings files that hold other keys and comments.
setup() {
    H=$1; rm -rf "$H"; mkdir -p "$H/.claude" "$H/.qwen" "$H/.codex" "$H/.grok" "$H/.config/foot" "$H/.oh-my-zsh/custom/themes" "$H/.local/bin"
    printf '{\n  "permissions": {"allow": ["Bash(ls)"]},\n  "theme": "dark"\n}\n' > "$H/.claude/settings.json"
    printf '{"$version": 4, "tools": {"approvalMode": "default"}, "permissions": {"allow": ["y"]}, "ui": {"hideTips": true}}' > "$H/.qwen/settings.json"
    printf '# my codex settings\nmodel = "some-model"\n\n[tui]\n# keep this comment\nnotifications = true\n\n[sandbox]\nmode = "workspace-write"\n' > "$H/.codex/config.toml"
    printf '# grok\n[ui]\ncompact_mode = true\n' > "$H/.grok/config.toml"
    printf 'font=monospace:size=11\n' > "$H/.config/foot/foot.ini"
    printf 'export ZSH="$HOME/.oh-my-zsh"\nZSH_THEME="robbyrussell"\nplugins=(git)\nsource $ZSH/oh-my-zsh.sh\n' > "$H/.zshrc"
    printf '#!/bin/sh\necho old interpreter\n' > "$H/.local/bin/berkeDark"; chmod 755 "$H/.local/bin/berkeDark"
}
pass=0; fail=0
ok() { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "ok   $2"; else fail=$((fail+1)); echo "FAIL $2"; fi; }
# 0. a full home: install, again, a settings change, uninstall: the tree byte for byte as before
H=$S/h0; setup $H; python3 $S/snap.py $H $S/b0.json
R $H install >/dev/null 2>&1; ok $? "full home: install"
python3 $S/snap.py $H $S/i1.json; R $H install >/dev/null 2>&1; python3 $S/snap.py $H $S/i2.json
cmp -s $S/i1.json $S/i2.json; ok $? "full home: a second install changes nothing"
R $H settings >/dev/null; sed -i 's/^# cream = .*/cream = "#fff0d6"/' $H/.config/berkeDark/berkeDark.toml
R $H install >/dev/null 2>&1; grep -q '^foreground=fff0d6' $H/.config/foot/berkeDark.ini && grep -q fff0d6 $H/.qwen/berkeDark.json; ok $? "full home: a colour from the settings file reaches the files"
R $H uninstall >/dev/null; rm -rf $H/.config/berkeDark; python3 $S/snap.py $H $S/a0.json
cmp -s $S/b0.json $S/a0.json; ok $? "full home: uninstall puts every file back byte for byte"
ok() { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "ok   $2"; else fail=$((fail+1)); echo "FAIL $2"; fi; }
# 1. an empty home: everything created, then removed, the tree as before
H=$S/h1; rm -rf $H; mkdir -p $H; python3 $S/snap.py $H $S/b1.json
R $H install >/dev/null 2>&1; ok $? "empty home: install"
[ -f $H/.qwen/settings.json ] && [ -f $H/.codex/config.toml ] && [ -f $H/.grok/config.toml ]; ok $? "empty home: settings files made"
R $H uninstall >/dev/null; python3 $S/snap.py $H $S/a1.json; cmp -s $S/b1.json $S/a1.json; ok $? "empty home: uninstall leaves the tree as it was"
# 2. Qwen settings with comments are not rewritten; the file stays as it was
H=$S/h2; rm -rf $H; mkdir -p $H/.qwen; printf '{\n  // mine\n  "ui": {}\n}\n' > $H/.qwen/settings.json; cp $H/.qwen/settings.json $S/q.orig
R $H install qwen > $S/o2.txt 2>&1; [ $? = 1 ]; ok $? "comments in Qwen settings: refused with a failing status"
cmp -s $H/.qwen/settings.json $S/q.orig; ok $? "comments in Qwen settings: file untouched"
grep -q "NOT CHANGED" $S/o2.txt; ok $? "comments in Qwen settings: said so"
R $H uninstall >/dev/null; [ ! -e $H/.qwen/berkeDark.json ]; ok $? "comments in Qwen settings: the theme file it placed goes again"
# 3. a dotted TOML key is not rewritten
H=$S/h3; rm -rf $H; mkdir -p $H/.codex; printf 'tui.theme = "other"\n' > $H/.codex/config.toml; cp $H/.codex/config.toml $S/c.orig
R $H install codex > $S/o3.txt 2>&1; cmp -s $H/.codex/config.toml $S/c.orig; ok $? "dotted TOML key: file untouched"
grep -q "NOT CHANGED" $S/o3.txt; ok $? "dotted TOML key: said so"
R $H uninstall >/dev/null
# 4. an existing theme key is replaced, other lines kept
H=$S/h4; rm -rf $H; mkdir -p $H/.codex; printf '[tui]\ntheme = "dracula" # old\nalt = 1\n' > $H/.codex/config.toml; cp $H/.codex/config.toml $S/c4.orig
R $H install codex >/dev/null; grep -q '^theme = "berkedark"$' $H/.codex/config.toml && grep -q '^alt = 1$' $H/.codex/config.toml; ok $? "existing TOML key replaced, others kept"
R $H uninstall codex >/dev/null; cmp -s $H/.codex/config.toml $S/c4.orig; ok $? "existing TOML key: uninstall restores the bytes"
# 5. a settings file changed after install: saved aside, then only berkeDark's keys go back
H=$S/h5; rm -rf $H; mkdir -p $H/.grok; printf '[ui]\n' > $H/.grok/config.toml
R $H install grok >/dev/null; printf 'extra = 1\n' >> $H/.grok/config.toml
R $H uninstall grok >/dev/null; grep -q extra $H/.grok/config.toml.berkeDark-before-uninstall-*; ok $? "user's later change saved aside before uninstall"
[ "$(cat $H/.grok/config.toml)" = "$(printf '[ui]\n\n[features]\nextra = 1')" ]; ok $? "user's later change kept, berkeDark's keys gone"
# 6. one target at a time
H=$S/h6; rm -rf $H; mkdir -p $H; R $H install qwen claude >/dev/null
R $H uninstall qwen >/dev/null; [ ! -e $H/.qwen/berkeDark.json ] && [ -e $H/.claude/themes/berkedark.json ]; ok $? "uninstall one target leaves the other"
R $H uninstall >/dev/null; [ -z "$(find $H -mindepth 1 -not -type d)" ]; ok $? "then everything goes"
# 7. a bad settings file stops before anything is written
H=$S/h7; rm -rf $H; mkdir -p $H/.config/berkeDark; printf '[roles]\n"no.such" = { fg = "pink" }\n' > $H/.config/berkeDark/berkeDark.toml
R $H install claude > $S/o7.txt 2>&1; [ $? != 0 ] && [ ! -e $H/.claude ]; ok $? "unknown class in settings: stops, nothing written"
printf '[palette]\ncream = "pink"\n' > $H/.config/berkeDark/berkeDark.toml
R $H install claude > $S/o7.txt 2>&1; [ $? != 0 ] && [ ! -e $H/.claude ]; ok $? "bad colour in settings: stops, nothing written"

# 8. (finding 1) uninstall after a target's folder was removed: the others are put back, a rerun is clean
H=$S/h8; rm -rf $H; mkdir -p $H/.codex $H/.grok $H/.qwen
printf '[tui]\nx = 1\n' > $H/.codex/config.toml; printf '[ui]\ny=2\n' > $H/.grok/config.toml; echo '{"a":1}' > $H/.qwen/settings.json
cp $H/.grok/config.toml $S/g8; cp $H/.qwen/settings.json $S/q8
R $H install codex grok qwen >/dev/null 2>&1; rm -rf $H/.codex
R $H uninstall > $S/o8.txt 2>&1; [ $? = 0 ] && cmp -s $H/.grok/config.toml $S/g8 && cmp -s $H/.qwen/settings.json $S/q8; ok $? "folder removed: uninstall goes on and puts the others back"
grep -q Traceback $S/o8.txt; [ $? != 0 ] && [ -n "$(ls $H/.local/state/berkeDark/backups)" ] && grep -q "kept" $S/o8.txt; ok $? "folder removed: no traceback, the old file is kept and named"
R $H uninstall > $S/o8b.txt 2>&1; [ $? = 0 ] && ! grep -q Traceback $S/o8b.txt; ok $? "folder removed: a second uninstall runs clean"
# 8b. a file that cannot be put back: reported, the rest done, the record keeps it with its copy, a rerun finishes
H=$S/h8b; rm -rf $H; mkdir -p $H/.grok $H/.qwen; printf '[ui]\n' > $H/.grok/config.toml; echo '{"a":1}' > $H/.qwen/settings.json
cp $H/.grok/config.toml $S/g8b; R $H install grok qwen >/dev/null 2>&1; chmod 444 $H/.grok/config.toml
R $H uninstall > $S/o8c.txt 2>&1; [ $? = 1 ] && grep -q "NOT RESTORED" $S/o8c.txt && [ "$(cat $H/.qwen/settings.json)" = '{"a":1}' ]; ok $? "a read-only file at uninstall: reported, the others done"
grep -q grok/config.toml $H/.local/state/berkeDark/manifest.json && [ -n "$(ls $H/.local/state/berkeDark/backups)" ]; ok $? "a read-only file at uninstall: still in the record, its copy kept"
chmod 644 $H/.grok/config.toml; R $H uninstall >/dev/null 2>&1; [ $? = 0 ] && cmp -s $H/.grok/config.toml $S/g8b && [ ! -e $H/.local/state ]; ok $? "a read-only file at uninstall: the rerun finishes and cleans up"
# 9. (finding 2) links: a symlinked .zshrc and Codex config stay links; a hard-linked settings file keeps its link
H=$S/h9; rm -rf $H; mkdir -p $H/.oh-my-zsh/custom $H/dots $H/.codex $H/.claude
printf 'ZSH_THEME="robbyrussell"\n' > $H/dots/zshrc; ln -s $H/dots/zshrc $H/.zshrc
printf '[tui]\nx = 1\n' > $H/dots/codex.toml; ln -s ../dots/codex.toml $H/.codex/config.toml
echo '{"theme": "dark"}' > $H/dots/claude.json; ln $H/dots/claude.json $H/.claude/settings.json
cp $H/dots/zshrc $S/z9; cp $H/dots/codex.toml $S/c9; cp $H/dots/claude.json $S/j9
R $H install ohmyzsh codex claude >/dev/null 2>&1
[ -L $H/.zshrc ] && [ -L $H/.codex/config.toml ] && grep -q berkeDark $H/dots/zshrc && grep -q berkedark $H/dots/codex.toml; ok $? "symlinks: still links, the files they point to changed"
[ "$(stat -c %h $H/dots/claude.json)" = 2 ] && grep -q custom:berkedark $H/dots/claude.json; ok $? "hard link: still two names for one changed file"
R $H uninstall >/dev/null 2>&1
[ -L $H/.zshrc ] && [ -L $H/.codex/config.toml ] && cmp -s $H/dots/zshrc $S/z9 && cmp -s $H/dots/codex.toml $S/c9; ok $? "symlinks: after uninstall still links, the files as they were"
[ "$(stat -c %h $H/dots/claude.json)" = 2 ] && cmp -s $H/dots/claude.json $S/j9; ok $? "hard link: after uninstall still linked, the file as it was"
H=$S/h9b; rm -rf $H; mkdir -p $H/.codex; ln -s $H/nowhere.toml $H/.codex/config.toml
R $H install codex > $S/o9.txt 2>&1; [ $? = 1 ] && grep -q "is a link to" $S/o9.txt && [ -L $H/.codex/config.toml ] && [ ! -e $H/nowhere.toml ]; ok $? "dangling symlink: refused, left as it was"
R $H uninstall >/dev/null 2>&1
# 10. (finding 3) a file berkeDark cannot read stops its target before anything is written; nothing orphaned
H=$S/h10; rm -rf $H; mkdir -p $H/.oh-my-zsh/custom/themes
echo '# my own older berkeDark theme' > $H/.oh-my-zsh/custom/themes/berkeDark.zsh-theme
printf '# \376irket\nZSH_THEME="robbyrussell"\n' > $H/.zshrc; python3 $S/snap.py $H $S/b10.json
R $H install ohmyzsh > $S/o10.txt 2>&1; [ $? = 1 ] && grep -q "not UTF-8" $S/o10.txt && grep -q "my own older" $H/.oh-my-zsh/custom/themes/berkeDark.zsh-theme; ok $? "not UTF-8 .zshrc: refused with a failing status, nothing written"
python3 $S/snap.py $H $S/a10.json; cmp -s $S/b10.json $S/a10.json; ok $? "not UTF-8 .zshrc: the tree as before, no record and no copies left"
# 10b. a write that fails part-way (the folder became read-only): the record still names the copy; uninstall cleans up
H=$S/h10b; rm -rf $H; mkdir -p $H/.grok; printf '[ui]\n' > $H/.grok/config.toml; cp $H/.grok/config.toml $S/g10
chmod 555 $H/.grok; R $H install grok > $S/o10b.txt 2>&1; st=$?; chmod 755 $H/.grok
[ $st = 1 ] && ! grep -q Traceback $S/o10b.txt && cmp -s $H/.grok/config.toml $S/g10; ok $? "a write that fails: reported, the file as it was"
b=$(ls $H/.local/state/berkeDark/backups); grep -q "$b" $H/.local/state/berkeDark/manifest.json; ok $? "a write that fails: the copy it made is in the record"
mkdir -p $H/.local/state/berkeDark/backups; echo stray > $H/.local/state/berkeDark/backups/not-listed
R $H uninstall >/dev/null 2>&1; cmp -s $H/.grok/config.toml $S/g10 && [ -f $H/.local/state/berkeDark/backups/not-listed ] && [ ! -e $H/.local/state/berkeDark/manifest.json ]; ok $? "uninstall: the listed copy goes, a copy it does not list stays"
# 11. (finding 4) uninstall gives back only berkeDark's keys; what the tools wrote since stays
H=$S/h11; rm -rf $H; mkdir -p $H/.codex $H/.claude; printf '[tui]\nx = 1\n' > $H/.codex/config.toml
printf '{\n  "theme": "dark",\n  "model": "a"\n}\n' > $H/.claude/settings.json
R $H install codex claude qwen >/dev/null 2>&1
printf '\n[projects."/home/u/p"]\ntrust_level = "trusted"\n' >> $H/.codex/config.toml
python3 - $H <<'PY'
import json, sys
p = sys.argv[1] + "/.claude/settings.json"; d = json.load(open(p)); d["model"] = "b"; json.dump(d, open(p, "w"), indent=2)
p = sys.argv[1] + "/.qwen/settings.json"; d = json.load(open(p)); d["security"] = {"auth": {"selectedType": "x"}}; json.dump(d, open(p, "w"), indent=2)
PY
R $H uninstall >/dev/null 2>&1
[ "$(cat $H/.codex/config.toml)" = "$(printf '[tui]\nx = 1\n\n[projects."/home/u/p"]\ntrust_level = "trusted"')" ]; ok $? "Codex: the project trust written after install stays, the theme key goes"
python3 -c "import json,sys; d=json.load(open(sys.argv[1])); sys.exit(d != {'theme':'dark','model':'b'})" $H/.claude/settings.json; ok $? "Claude: theme back to its old value, tui gone, the later model kept"
python3 -c "import json,sys; d=json.load(open(sys.argv[1])); sys.exit(d != {'security':{'auth':{'selectedType':'x'}}})" $H/.qwen/settings.json; ok $? "Qwen settings berkeDark made: its key gone, the tool's auth kept"
H=$S/h11b; rm -rf $H; mkdir -p $H; R $H install qwen codex >/dev/null 2>&1
python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d['ui']['theme']='Dracula'; json.dump(d,open(p,'w'))" $H/.qwen/settings.json
R $H uninstall > $S/o11.txt 2>&1; grep -q Dracula $H/.qwen/settings.json && grep -q "changed since" $S/o11.txt && [ ! -e $H/.codex ]; ok $? "a key the user changed after install is left; files berkeDark made and nothing else uses go"
# 12. (finding 5) two install and uninstall cycles keep two saved copies
H=$S/h12; rm -rf $H; mkdir -p $H/.grok; printf '[ui]\n' > $H/.grok/config.toml
R $H install grok >/dev/null; echo 'first = 1' >> $H/.grok/config.toml; R $H uninstall grok >/dev/null
R $H install grok >/dev/null; echo 'second = 2' >> $H/.grok/config.toml; R $H uninstall grok >/dev/null
[ "$(ls $H/.grok | grep -c before-uninstall)" = 2 ] && grep -l 'first' $H/.grok/*before-uninstall* | grep -q . && grep -l 'second' $H/.grok/*before-uninstall* | grep -q .; ok $? "two cycles: two saved copies, neither overwritten"
# 13. (finding 6) foot: only colours, cursor and the user's own shell; the user's font and shell stay theirs
H=$S/h13; rm -rf $H; mkdir -p $H/.config/foot; printf 'font=Hack:size=13\npad=2x2\nshell=/usr/bin/fish -l\n\n[scrollback]\nlines=500\n' > $H/.config/foot/foot.ini
cat > $S/effective.py <<'PY'
import os, re, sys
def load(path, out):
    section = "main"
    for raw in open(os.path.expanduser(path), encoding="utf-8"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        header = re.match(r"^\[(.+)\]$", line)
        if header:
            section = header.group(1)
            continue
        key, _, value = line.partition("=")
        if key.strip() == "include":
            load(value.strip(), out)
        else:
            out[(section, key.strip())] = value.strip()
    return out
print("\n".join(f"{s}.{k}={v}" for (s, k), v in sorted(load(sys.argv[1], {}).items())))
PY
python3 $S/effective.py $H/.config/foot/foot.ini | grep -v '^main.shell' > $S/e13a
R $H install terminal >/dev/null 2>&1; python3 $S/effective.py $H/.config/foot/foot.ini > $S/e13b
grep -v '^main.shell' $S/e13b | grep -v '^colors-dark\.\|^cursor\.\|^main.bold-text-in-bright' | cmp -s - $S/e13a; ok $? "foot: the user's font, pad and scrollback stay in effect"
! grep -q '^font=\|^pad=\|^\[scrollback\]\|^lines=' $H/.config/foot/berkeDark.ini; ok $? "foot: berkeDark's include sets no font, pad or scrollback"
grep -qxF "main.shell=/bin/sh -c '[ -x \"\$HOME/.local/bin/berkeDark\" ] && exec \"\$HOME/.local/bin/berkeDark\" /usr/bin/fish -l; exec /usr/bin/fish -l'" $S/e13b; ok $? "foot: the user's own shell= runs inside the interpreter, and alone without it"
foot -c $H/.config/foot/foot.ini --check-config >/dev/null 2>&1; ok $? "foot: --check-config passes"
H=$S/h13b; rm -rf $H; mkdir -p $H; X="SHELL=/bin/bash" R $H install terminal >/dev/null 2>&1
grep -q 'berkeDark" /bin/bash; exec /bin/bash' $H/.config/foot/berkeDark.ini; ok $? "foot: without a shell= of the user's, \$SHELL"
if [ -f /etc/xdg/foot/foot.ini ]; then head -1 $H/.config/foot/foot.ini | grep -qxF include=/etc/xdg/foot/foot.ini; ok $? "foot: a new foot.ini includes the system's first"; fi
R $H uninstall >/dev/null 2>&1; [ -z "$(find $H -mindepth 1 -not -type d)" ]; ok $? "foot: the new foot.ini goes with uninstall"
# 13b. a foot.ini that already sets berkeDark's font, pad, shell and palette (this project's own machine): the same in effect
H=$S/h13c; rm -rf $H; mkdir -p $H/.config/foot
{ printf 'font=JetBrainsMono Nerd Font:size=18\npad=8x6\nbold-text-in-bright=palette-based\n'
  printf '%s\n' "shell=/bin/sh -c '[ -x \"\$HOME/.local/bin/berkeDark\" ] && exec \"\$HOME/.local/bin/berkeDark\" /bin/zsh; exec /bin/zsh'"
  printf '\n[scrollback]\nlines=10000\n\n'; sed -n '/^\[colors-dark\]/,$p' "$REPO/terminal/config/foot.ini"; } > $H/.config/foot/berkeDark.ini
{ cat $H/.config/foot/berkeDark.ini; printf '\n[main]\ninclude=%s\n' "$H/.config/foot/berkeDark.ini"; } > $H/.config/foot/foot.ini
python3 $S/effective.py $H/.config/foot/foot.ini > $S/e13c; R $H install terminal >/dev/null 2>&1; python3 $S/effective.py $H/.config/foot/foot.ini > $S/e13d
cmp -s $S/e13c $S/e13d && foot -c $H/.config/foot/foot.ini --check-config >/dev/null 2>&1; ok $? "foot: a foot.ini with berkeDark's own settings gives the same result, and foot accepts it"
# 14. (findings 7-11) CRLF, JSON layout, modes, read-only files, the copy taken at the first real change
H=$S/h14; rm -rf $H; mkdir -p $H/.codex $H/.oh-my-zsh/custom $H/.qwen $H/.claude
printf '# c\r\n[tui]\r\nx = 1\r\n' > $H/.codex/config.toml; printf 'export A=1\r\nZSH_THEME="x"\r\n' > $H/.zshrc
printf '{"a": 1}\n' > $H/.qwen/settings.json; printf '{\n\t"theme": "dark"\n}\n' > $H/.claude/settings.json
R $H install codex ohmyzsh qwen claude >/dev/null 2>&1
[ "$(tr -cd '\r' < $H/.codex/config.toml | wc -c)" = 4 ] && [ "$(tr -cd '\r' < $H/.zshrc | wc -c)" = 2 ]; ok $? "CRLF files keep CRLF on every line"
[ "$(wc -l < $H/.qwen/settings.json)" = 1 ] && grep -q '^	"theme"' $H/.claude/settings.json; ok $? "JSON keeps its layout: one line stays one line, tabs stay tabs"
R $H uninstall >/dev/null 2>&1
H=$S/h14b; rm -rf $H; mkdir -p $H; (umask 077; R $H install qwen >/dev/null 2>&1)
[ "$(stat -c %a $H/.qwen/settings.json)" = 600 ] && [ "$(stat -c %a $H/.qwen)" = 700 ]; ok $? "new files and folders follow the umask"
H=$S/h14c; rm -rf $H; mkdir -p $H/.qwen; echo '{"a":1}' > $H/.qwen/settings.json; chmod 444 $H/.qwen/settings.json
R $H install qwen > $S/o14.txt 2>&1; [ $? = 1 ] && grep -q read-only $S/o14.txt && [ "$(cat $H/.qwen/settings.json)" = '{"a":1}' ] && [ "$(stat -c %a $H/.qwen/settings.json)" = 444 ]; ok $? "a read-only settings file: refused, untouched"
chmod 644 $H/.qwen/settings.json
H=$S/h14d; rm -rf $H; mkdir -p $H; R $H install qwen >/dev/null 2>&1; cp $H/.qwen/berkeDark.json $S/t14; R $H uninstall >/dev/null 2>&1
mkdir -p $H/.qwen; cp $S/t14 $H/.qwen/berkeDark.json; R $H install qwen >/dev/null 2>&1
R $H settings >/dev/null; sed -i 's/^# cream = .*/cream = "#fff0d6"/' $H/.config/berkeDark/berkeDark.toml; R $H install qwen >/dev/null 2>&1
grep -q fff0d6 $H/.qwen/berkeDark.json && R $H uninstall >/dev/null 2>&1 && cmp -s $H/.qwen/berkeDark.json $S/t14; ok $? "a file first found as wanted, changed later: kept then, put back by uninstall"
# 15. (findings 12, 13) no compiler: a failing status; a damaged record: a message, no traceback
H=$S/h15; rm -rf $H; mkdir -p $H; X="CC=no-such-compiler" R $H install terminal > $S/o15.txt 2>&1; [ $? = 1 ] && grep -q "NOT DONE.*no-such-compiler" $S/o15.txt; ok $? "no compiler: NOT DONE and a failing status"
R $H uninstall >/dev/null 2>&1; mkdir -p $H/.local/state/berkeDark; echo '{"files": ' > $H/.local/state/berkeDark/manifest.json
R $H uninstall > $S/o15b.txt 2>&1; [ $? != 0 ] && grep -q "cannot be read" $S/o15b.txt && ! grep -q Traceback $S/o15b.txt; ok $? "damaged record: says what to do, no traceback"
# 16. (finding 14) the Qwen patch: the copy the qwen command runs, a failed check leaves no copy, an unwritable copy is reported
H=$S/h16; P=$H/lib/node_modules/@qwen-code/qwen-code; rm -rf $H; mkdir -p $P/chunks $P/dist $H/bin
echo '{"name":"@qwen-code/qwen-code","version":"9.9.9"}' > $P/package.json; printf '#!/usr/bin/env node\n' > $P/dist/cli.js; chmod 755 $P/dist/cli.js; ln -s ../lib/node_modules/@qwen-code/qwen-code/dist/cli.js $H/bin/qwen
A='"hljs-addition":{backgroundColor:colors.AccentGreen,display:"inline-block",width:"100%"}'
D='"hljs-deletion":{backgroundColor:colors.AccentRed,display:"inline-block",width:"100%"}'
QP() { env -i HOME="$H" PATH="$H/bin:/usr/bin:/bin" python3 -I "$REPO/berkedark" qwen-diff-patch "$@"; }
printf 'const colors={};export const t={%s,%s};\n' "$A" "$D" > $P/chunks/a.js
QP --dry-run > $S/o16.txt 2>&1; grep -q "$P  (the one the qwen command runs)" $S/o16.txt; ok $? "Qwen patch: finds the copy the qwen command runs"
printf 'const colors={};export const t={%s,%s};}}}\n' "$A" "$D" > $P/chunks/a.js; cp $P/chunks/a.js $S/a16
QP --root $P >/dev/null 2>&1; [ $? = 1 ] && cmp -s $P/chunks/a.js $S/a16 && [ ! -e $P/chunks/a.js.berkeDark-backup ]; ok $? "Qwen patch: a failed node --check leaves the file and no copy"
printf 'const colors={};export const t={%s,%s};\n' "$A" "$D" > $P/chunks/a.js; chmod 555 $P/chunks
QP --root $P > $S/o16b.txt 2>&1; st=$?; chmod 755 $P/chunks
[ $st = 1 ] && grep -q "cannot change this copy" $S/o16b.txt && ! grep -q Traceback $S/o16b.txt; ok $? "Qwen patch: an unwritable copy is reported, no traceback"
echo "installer cases: $pass passed, $fail failed"
[ $fail = 0 ]
