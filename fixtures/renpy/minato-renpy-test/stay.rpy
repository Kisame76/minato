# Added to the test game by tools/renpy-splash-build.sh stay: after the game's lines (its quit is taken out) a screen
# that moves for good, so that the app keeps running and drawing for java-start-check's resume round.
transform minato_spin:
    rotate 0
    linear 2.0 rotate 360
    repeat

screen minato_idle():
    add Solid("#fff", xysize=(300, 300)) align (0.5, 0.5) at minato_spin

label after_game:
    call screen minato_idle
    return
