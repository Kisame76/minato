# Minato's Ren'Py memory game (tools/renpy-memory.sh): a visual novel's pattern, many distinct full-screen backgrounds
# one after the other with a dissolve and a sound each, so that what grows without bound in Minato's process while it
# plays shows in its footprint (images.py writes game/bg*.png, ../minato-renpy-test/tone.py game/tone.wav at the
# build, and track0.wav ... track59.wav, short distinct tones, by ../minato-renpy-test/tone.py's twin tracks.py). Every
# scene also changes the music every fourth scene and says a line of text no scene said before. It logs a line per ten
# scenes and "done" after the last; the number of scenes is MINATO_SCENES in the environment or 240.
define config.name = "Minato Ren'Py Memory"
define build.name = "minatorenpymemory"
define config.save_directory = "minatorenpymemory"
define config.image_cache_size_mb = 300

label start:
    $ import os
    $ scenes = int(os.environ.get("MINATO_SCENES", "240"))
    $ print("minato-renpy-memory: start")
    $ index = 0
    while index < scenes:
        scene expression ("bg%d.png" % (index % 300))
        with dissolve
        play sound "tone.wav"
        if index % 4 == 0:
            play music ("track%d.wav" % ((index // 4) % 60)) fadein 0.2 fadeout 0.2
        $ line = "".join(chr(0x4e00 + (index * 37 + k * 11) % 2000) for k in range(40)) + " %d" % index
        $ narrator(line, interact=False)
        $ renpy.pause(0.5, hard=True)
        $ index += 1
        if index % 10 == 0:
            $ print("minato-renpy-memory: scene %d" % index)
    $ print("minato-renpy-memory: done")
    $ renpy.quit()
