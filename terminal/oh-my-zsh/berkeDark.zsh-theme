# berkeDark for oh-my-zsh.
# Before the command: who @ where in which folder, on which branch, at what time.
# The line starts with # so that it does nothing if someone copies it and runs it.
# Install: copy to ~/.oh-my-zsh/custom/themes/ and set ZSH_THEME="berkeDark".

ZSH_THEME_GIT_PROMPT_PREFIX=" %F{#8f95a0}on%f %F{#6fdcf0}"
ZSH_THEME_GIT_PROMPT_SUFFIX="%f"
ZSH_THEME_GIT_PROMPT_DIRTY=" %F{#ff5c7c}x"
ZSH_THEME_GIT_PROMPT_CLEAN=" %F{#a9ec76}o"

_berke_dark_environment() {
  [[ -n $VIRTUAL_ENV ]] && print -n " %F{#8f95a0}with%f %F{#a3a5f5}${VIRTUAL_ENV:t}%f"
}

PROMPT='
%F{#8f95a0}#%f %(#,%F{#ff5c7c},%F{#57bfff})%n%f %F{#8f95a0}@%f %F{#a9ec76}%m%f %F{#8f95a0}in%f %F{#ffd25c}%~%f$(git_prompt_info)$(_berke_dark_environment) %F{#8f95a0}[%D{%Y-%m-%d %H:%M:%S}]%f%(?.. %F{#ff5c7c}exit %?%f)
%F{#ff9e60}%(#.#.$)%f '
