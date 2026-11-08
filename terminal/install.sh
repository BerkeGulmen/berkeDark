#!/bin/sh
# Installs berkeDark for the terminal and for Claude Code. Linux only; needs clang (or set CC).
#
# It writes ~/.local/bin/berkeDark and Claude Code's theme file, sets "theme" and "tui" in Claude
# Code's settings, and, when foot is installed, adds one include line to foot.ini.
# A file it replaces is kept once as <file>.bak-berkedark. Running it again changes nothing more.
#
#   sh install.sh             everything
#   sh install.sh --no-foot   leave foot alone
set -eu
cd "$(dirname "$0")"

use_foot=yes
case "${1:-}" in
    "") ;;
    --no-foot) use_foot=no ;;
    *) echo "usage: sh install.sh [--no-foot]" >&2; exit 2 ;;
esac

if [ "$(uname -s)" != Linux ]; then
    echo "berkeDark: this installer is for Linux; recognising Claude Code needs /proc." >&2
    exit 1
fi
if ! command -v "${CC:-clang}" >/dev/null 2>&1; then
    echo "berkeDark: ${CC:-clang} was not found. Install clang, or set CC to your C compiler." >&2
    exit 1
fi

keep_copy() {
    if [ -e "$1" ] && [ ! -e "$1.bak-berkedark" ]; then
        cp -p "$1" "$1.bak-berkedark"
    fi
}

sh ./build.sh install
echo "installed  $HOME/.local/bin/berkeDark"

claude_dir=${CLAUDE_CONFIG_DIR:-$HOME/.claude}
mkdir -p "$claude_dir/themes"
theme=$claude_dir/themes/berkedark.json
keep_copy "$theme"
cp config/claude-theme.json "$theme"
echo "installed  $theme"

settings=$claude_dir/settings.json
settings_note='add these two lines to it yourself: "theme": "custom:berkedark", "tui": "fullscreen"'
if [ ! -e "$settings" ]; then
    printf '{\n  "theme": "custom:berkedark",\n  "tui": "fullscreen"\n}\n' > "$settings"
    echo "written    $settings"
elif ! command -v python3 >/dev/null 2>&1; then
    echo "NOT CHANGED $settings (python3 was not found): $settings_note"
else
    keep_copy "$settings"
    if python3 - "$settings" <<'PYTHON'
import json, os, sys

path = sys.argv[1]
try:
    settings = json.load(open(path, encoding="utf-8"))
except ValueError:
    sys.exit(1)
if not isinstance(settings, dict):
    sys.exit(1)
settings.update(theme="custom:berkedark", tui="fullscreen")
with open(path + ".new", "w", encoding="utf-8") as out:
    json.dump(settings, out, indent=2, ensure_ascii=False)
    out.write("\n")
os.chmod(path + ".new", os.stat(path).st_mode & 0o7777)
os.replace(path + ".new", path)
PYTHON
    then
        echo "updated    $settings"
    else
        echo "NOT CHANGED $settings (it is not valid JSON): $settings_note"
    fi
fi

if [ "$use_foot" = yes ] && command -v foot >/dev/null 2>&1; then
    foot_dir=${XDG_CONFIG_HOME:-$HOME/.config}/foot
    foot_file=$foot_dir/foot.ini
    line="include=$foot_dir/berkeDark.ini"
    mkdir -p "$foot_dir"
    # foot sets SHELL to its shell= command, so the login shell is read from the account.
    user_shell=$(getent passwd "$(id -un)" 2>/dev/null | cut -d: -f7)
    user_shell=${user_shell:-${SHELL:-/bin/sh}}
    sed "s|@SHELL@|$user_shell|g" config/foot.ini > "$foot_dir/berkeDark.ini"
    if ! grep -qxF "$line" "$foot_file" 2>/dev/null; then
        before=$(mktemp)
        had_file=no
        if [ -e "$foot_file" ]; then
            cp -p "$foot_file" "$before"
            had_file=yes
        fi
        keep_copy "$foot_file"
        printf '\n[main]\n%s\n' "$line" >> "$foot_file"
        if foot -c "$foot_file" --check-config >/dev/null 2>&1; then
            echo "updated    $foot_file"
        else
            if [ "$had_file" = yes ]; then cp -p "$before" "$foot_file"; else rm -f "$foot_file"; fi
            echo "NOT CHANGED $foot_file (this foot does not accept berkeDark's settings)"
        fi
        rm -f "$before"
    fi
else
    echo "foot left alone; start Claude Code with: berkeDark claude"
fi

echo
echo "Done. Open a new terminal window and start Claude Code there; windows already open keep what they had."
