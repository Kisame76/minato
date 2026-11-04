# Added to Minato's Ren'Py test game by tools/renpy-splash-build.sh: a splashscreen that calls a screen at once, so
# Interface start, and with it SDL's window, comes as early as a game's own splashscreen makes it.
screen minato_splash():
    text "Minato splash"
    timer 0.5 action Return()

label splashscreen:
    call screen minato_splash
    return
