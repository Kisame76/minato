# Minato's Ren'Py test game (tools/renpy-check.sh): one line of text and a 2 s tone of 440 Hz (../tone.py
# writes game/tone.wav at the build). After a second of a hard pause, which draws frames, it logs whether the tone
# plays and where it is: a tone played at the wrong rate is not about 1 s in.
define config.name = "Minato Ren'Py Test"
define build.name = "minatorenpytest"
define config.save_directory = "minatorenpytest"

label start:
    play sound "tone.wav"
    show text "Minato Ren'Py test: text and a 440 Hz tone."
    $ print("minato-renpy-test: start")
    $ renpy.pause(1.0, hard=True)
    $ position = renpy.music.get_pos(channel="sound") or 0.0
    $ print("minato-renpy-test: tone playing " + str(renpy.sound.is_playing()))
    $ print("minato-renpy-test: tone position " + ("about 1 s" if 0.7 < position < 1.4 else "%.2f s" % position))
    $ print("minato-renpy-test: done")
    $ renpy.quit()
