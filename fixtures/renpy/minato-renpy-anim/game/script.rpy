# Minato's Ren'Py anim game, a variant of the memory game (fixtures/renpy/minato-renpy-memory) with a frame-by-frame
# animation: a title background that is an ATL image sequence of 600 distinct 1920 x 1080 frames at 0.04 s each,
# looping (anim.rpy and the PNGs, written at the build by frames.py), shown for MINATO_SCENES seconds (180 by
# default). Same package and log lines as renpy-memory-test, plus the gate's lines below, which
# tools/java-start-check.sh's SDL returns round (renpy-returns-test) waits for. tools/renpy-anim.sh builds it.
define config.name = "Minato Ren'Py Memory"
define build.name = "minatorenpymemory"
define config.save_directory = "minatorenpymemory"
define config.image_cache_size_mb = 300

init python:
    def _send_low_memory(index):
        # jnius' Java objects have no pickle reduction, so binding one to a store name (store.activity) breaks
        # check_suspend's mobile_save on a background. Keep the import, the activity and any error as this
        # function's locals, out of the store, so the suspend save works as it does in a real game.
        try:
            import jnius
            jnius.autoclass("org.renpy.android.PythonSDLActivity").mActivity.onLowMemory()
            print("minato-renpy-memory: low memory sent at %d s" % index)
        except Exception as exc:
            print("minato-renpy-memory: low memory not sent: %r" % (exc,))

# The gate for the returns round (java-start-check's run_sdl_returns): after a display reset Ren'Py drops its
# textures, and the next interaction's final predict loads every image of the scene again, synchronously and without
# a look at the events (600 frames from 30 full-screen PNGs, tens of seconds under load). A HOME in that window can
# make Ren'Py reset its display while SDL's backup has left no context current, which crashes the process on Android.
# So the game says when it is safe to leave: "display reset <n>" at each reset, and "preload done <n>" at the first
# periodic callback of the first interaction that began after the last reset or after the start's scene
# (_gate_arm), i.e. after that interaction's final predict. "gate armed" at init tells the round the game has it.
init python:
    def _gate_install():
        import renpy.display.core as core
        box = {"pending": False, "armed": False, "resets": 0, "done": 0}
        reset = core.Interface.before_resize

        def before_resize(self):
            box["resets"] += 1
            box["pending"], box["armed"] = True, False
            print("minato-renpy-memory: display reset %d" % box["resets"])
            return reset(self)

        def interact_begins():
            if box["pending"]:
                box["pending"], box["armed"] = False, True

        def periodic():
            if box["armed"]:
                box["armed"] = False
                box["done"] += 1
                print("minato-renpy-memory: preload done %d" % box["done"])

        def arm():
            box["pending"], box["armed"] = True, False

        core.Interface.before_resize = before_resize
        config.interact_callbacks.append(interact_begins)
        config.periodic_callbacks.append(periodic)
        print("minato-renpy-memory: gate armed")
        return arm

    _gate_arm = _gate_install()

label start:
    $ import os
    $ seconds = int(os.environ.get("MINATO_SCENES", "180"))
    $ print("minato-renpy-memory: start")
    scene titleanim
    $ _gate_arm()
    $ index = 0
    while index < seconds:
        $ renpy.pause(1.0, hard=True)
        $ index += 1
        if index in (20, 60):
            $ _send_low_memory(index)
        if index % 10 == 0:
            $ print("minato-renpy-memory: scene %d" % index)
    $ print("minato-renpy-memory: done")
    $ renpy.quit()
