# berkeDark for oh-my-zsh.
# Before the command: who @ where in which folder, on which branch, at what time.
# The line starts with # so that it does nothing if someone copies it and runs it.
# Install: copy to ~/.oh-my-zsh/custom/themes/ and set ZSH_THEME="berkeDark".

ZSH_THEME_GIT_PROMPT_PREFIX=" %F{@{shell.prompt.word}}on%f %F{@{shell.prompt.branch}}"
ZSH_THEME_GIT_PROMPT_SUFFIX="%f"
ZSH_THEME_GIT_PROMPT_DIRTY=" %F{@{shell.prompt.branch.dirty}}x"
ZSH_THEME_GIT_PROMPT_CLEAN=" %F{@{shell.prompt.branch.clean}}o"

_berke_dark_environment() {
  [[ -n $VIRTUAL_ENV ]] && print -n " %F{@{shell.prompt.word}}with%f %F{@{shell.prompt.environment}}${VIRTUAL_ENV:t}%f"
}

PROMPT='
%F{@{shell.prompt.word}}#%f %(#,%F{@{shell.prompt.user.root}},%F{@{shell.prompt.user}})%n%f %F{@{shell.prompt.word}}@%f %F{@{shell.prompt.host}}%m%f %F{@{shell.prompt.word}}in%f %F{@{shell.prompt.path}}%~%f$(git_prompt_info)$(_berke_dark_environment) %F{@{shell.prompt.time}}[%D{%Y-%m-%d %H:%M:%S}]%f%(?.. %F{@{shell.prompt.status}}exit %?%f)
%F{@{shell.prompt.character}}%(#.#.$)%f '
