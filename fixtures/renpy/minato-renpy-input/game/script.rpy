# Minato's Ren'Py input game (tools/renpy-input-reference.sh): a name dialog, as a visual novel asks for one with
# renpy.input. The script that drives the AVD presses Back and taps outside the field while the dialog is up; the game
# logs "minato-renpy-input: " lines at each step, and the keyboard fixture (ime-probe-test) logs what the field tells
# the keyboard.
define config.name = "Minato Ren'Py Input"
define build.name = "minatorenpyinput"
define config.save_directory = "minatorenpyinput"

label start:
    scene black
    show text "Minato Ren'Py input test."
    $ print("minato-renpy-input: start")
    $ renpy.pause(2.0, hard=True)
    $ print("minato-renpy-input: asking")
    $ name = renpy.input("Your name", length=20)
    $ print("minato-renpy-input: input returned " + repr(name))
    $ renpy.pause(2.0, hard=True)
    $ print("minato-renpy-input: done")
    $ renpy.quit()
