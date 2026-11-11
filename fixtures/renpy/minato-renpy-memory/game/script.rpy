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

# MINATO_PY_TRACE: time-stamped lines for the Android pause/resume path, off unless the env is set, so the normal
# memory run is unchanged. It wraps the engine points that decide whether the game suspends on a background and
# resumes its GL on a return: check_suspend (does it run, with which event), mobile_save and free_memory (entry /
# exit / exception), and the event poll and wait (which APP_* events are dequeued). Prints line up with the EGL
# trace by time.time().
init python:
    import os as _pt_os
    if _pt_os.environ.get("MINATO_PY_TRACE"):
        import time as _pt_time
        def _pt(msg):
            try:
                print("PYTRACE %.3f %s" % (_pt_time.time(), msg))
            except Exception:
                pass
        _pt("trace armed")
        try:
            import renpy.display.core as _pt_core
            _pt_cs = _pt_core.Interface.check_suspend
            def _pt_check_suspend(self, ev, *a, **k):
                _pt("check_suspend entry ev.type=%r" % (getattr(ev, "type", "?"),))
                r = _pt_cs(self, ev, *a, **k)
                _pt("check_suspend return=%r" % (r,))
                return r
            _pt_core.Interface.check_suspend = _pt_check_suspend
            _pt_ms = _pt_core.Interface.mobile_save
            def _pt_mobile_save(self, *a, **k):
                _pt("mobile_save entry")
                try:
                    r = _pt_ms(self, *a, **k)
                    _pt("mobile_save exit")
                    return r
                except BaseException as e:
                    _pt("mobile_save EXC %r" % (e,))
                    raise
            _pt_core.Interface.mobile_save = _pt_mobile_save
        except Exception as e:
            _pt("wrap core failed %r" % (e,))
        try:
            import renpy.exports as _pt_exp
            _pt_fm = _pt_exp.free_memory
            def _pt_free_memory(*a, **k):
                _pt("free_memory entry")
                try:
                    r = _pt_fm(*a, **k)
                    _pt("free_memory exit")
                    return r
                except BaseException as e:
                    _pt("free_memory EXC %r" % (e,))
                    raise
            _pt_exp.free_memory = _pt_free_memory
        except Exception as e:
            _pt("wrap free_memory failed %r" % (e,))
        try:
            import pygame_sdl2 as _pt_pg
            import pygame_sdl2.event as _pt_ev
            _pt_lo, _pt_hi = _pt_pg.APP_WILLENTERBACKGROUND, _pt_pg.APP_DIDENTERFOREGROUND
            def _pt_name(tp):
                for n in ("APP_WILLENTERBACKGROUND", "APP_DIDENTERBACKGROUND",
                          "APP_WILLENTERFOREGROUND", "APP_DIDENTERFOREGROUND"):
                    if getattr(_pt_pg, n, None) == tp:
                        return n
                return str(tp)
            def _pt_wrap_event(name, orig):
                def wrapped(*a, **k):
                    e = orig(*a, **k)
                    try:
                        if e is not None and _pt_lo <= e.type <= _pt_hi:
                            _pt("event.%s -> %s" % (name, _pt_name(e.type)))
                    except Exception:
                        pass
                    return e
                return wrapped
            _pt_ev.poll = _pt_wrap_event("poll", _pt_ev.poll)
            _pt_ev.wait = _pt_wrap_event("wait", _pt_ev.wait)
        except Exception as e:
            _pt("wrap events failed %r" % (e,))

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
