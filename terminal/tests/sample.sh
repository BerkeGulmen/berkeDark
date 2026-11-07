#!/bin/sh
# Prints every style berkeDark treats, so the look can be judged on screen.
printf 'Düz yazı, genel renk\n'
printf '\033[1mKalın yazı\033[22m\n'
printf '\033[3mEğik yazı, berkeDark altını\033[23m\n'
printf '\033[9mÜstü çizili yazı, bir önceki genel renk\033[29m\n'
printf '\033[1m\033[3mKalın ve eğik\033[23m\033[22m\n'
printf '\033[35mSatır içi kod\033[39m ve \033[3m\033[35mprogramın renklendirdiği eğik yazı\033[39m\033[23m\n'
printf '\033[1m\033[3m\033[4mEn büyük başlık\033[24m\033[23m\033[22m\n'
