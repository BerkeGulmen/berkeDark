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
R() { H=$1; shift; env -i HOME="$H" PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 XDG_CONFIG_HOME="$H/.config" \
      XDG_STATE_HOME="$H/.local/state" python3 -I "$REPO/berkedark" "$@"; }
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
# 5. a file changed by the user after install is saved before restoring
H=$S/h5; rm -rf $H; mkdir -p $H/.grok; printf '[ui]\n' > $H/.grok/config.toml
R $H install grok >/dev/null; printf 'extra = 1\n' >> $H/.grok/config.toml
R $H uninstall grok >/dev/null; [ -f $H/.grok/config.toml.berkeDark-before-uninstall ] && grep -q extra $H/.grok/config.toml.berkeDark-before-uninstall; ok $? "user's later change saved before restoring"
[ "$(cat $H/.grok/config.toml)" = "[ui]" ]; ok $? "user's later change: original restored"
# 6. one target at a time
H=$S/h6; rm -rf $H; mkdir -p $H; R $H install qwen claude >/dev/null
R $H uninstall qwen >/dev/null; [ ! -e $H/.qwen/berkeDark.json ] && [ -e $H/.claude/themes/berkedark.json ]; ok $? "uninstall one target leaves the other"
R $H uninstall >/dev/null; [ -z "$(find $H -mindepth 1 -not -type d)" ]; ok $? "then everything goes"
# 7. a bad settings file stops before anything is written
H=$S/h7; rm -rf $H; mkdir -p $H/.config/berkeDark; printf '[roles]\n"no.such" = { fg = "pink" }\n' > $H/.config/berkeDark/berkeDark.toml
R $H install claude > $S/o7.txt 2>&1; [ $? != 0 ] && [ ! -e $H/.claude ]; ok $? "unknown class in settings: stops, nothing written"
printf '[palette]\ncream = "pink"\n' > $H/.config/berkeDark/berkeDark.toml
R $H install claude > $S/o7.txt 2>&1; [ $? != 0 ] && [ ! -e $H/.claude ]; ok $? "bad colour in settings: stops, nothing written"
echo "installer cases: $pass passed, $fail failed"
