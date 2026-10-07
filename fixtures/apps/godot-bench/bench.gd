# Test 2 with Godot (ADR-0042 decision 1): the scene of ADR-0037 in Godot 4.7.2, drawn for bench.conf's
# seconds. N lit, textured spheres, each drawn on its own; N particles in one instanced draw, blended over
# them; a full-screen background behind the 3D world; and a post pass that reads the finished frame.
# Everything moves by the frame index only, so every build draws the same frames.
#
# In: bench.conf in user://, the benchmark mode's key=value lines as ADR-0037's scene reads them (run, rate,
# load, seconds); without one, the emulator's defaults (60 Hz, light, 10 s). The user arguments after "--"
# may name another bench.conf (--bench-config=<path>, for the Mac's check), the driver the run must draw
# with (--bench-driver=<name>: another one counts as a failure, so a Vulkan build that fell back to GLES
# cannot pass as Vulkan), another frame for the hash (--bench-hash-frame=<n>) and a PNG of that frame
# beside bench.conf (--bench-snapshot).
# Out, beside bench.conf: scene.csv with a row per frame (index, tick_ns, cpu_ns), then the file done.
# tick_ns counts from the engine's start, so only its intervals mean something; cpu_ns is the main loop's
# CPU time that Godot measured for the frame before (process callbacks and the renderer's setup).
# The log: lines that begin "godot-bench:" are the same on every device (tools/android-reference.sh compares
# them), lines that begin "godot-bench-device:" are not and are only shown, and the last one,
# "run <run>: <frames> frames, <failures> failures", is checked by its form. The first touch on the screen is
# logged once, as a share of the viewport in whole percent, so that the line is the same on every screen (Godot's
# input, ADR-0042 b)).
# The process environment carries the frame the main loop draws (GODOT_BENCH_FRAME, from 0) and, after the last,
# GODOT_BENCH_DONE: the twin's frame hook reads them around each frame, so that its recording holds exactly the
# frames of scene.csv (ADR-0042 c).
extends Node3D

const LOADS := {"light": 512, "heavy": 2048}  # N per load [guess]; ADR-0042 d) calibrates them
const DEFAULTS := {"run": "default", "rate": 60, "load": "light", "seconds": 10}
const NAME_CHARACTERS := "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"

const SPHERE_SHADER := """
shader_type spatial;
uniform sampler2D albedo_texture : source_color, filter_linear_mipmap;
uniform float frame;
void fragment() {
	vec3 texel = texture(albedo_texture, UV).rgb;
	ALBEDO = texel * (0.75 + 0.25 * sin(frame * 0.05 + UV.y * 6.2831853));
	ROUGHNESS = 0.6;
}
"""

const PARTICLE_SHADER := """
shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled;
uniform float frame;
void vertex() {
	float t = frame * 0.02 + float(INSTANCE_ID) * 0.618;
	VERTEX += vec3(sin(t), cos(t * 1.3), sin(t * 0.7)) * 0.3;
}
void fragment() {
	vec2 d = UV - vec2(0.5);
	float glow = clamp(1.0 - dot(d, d) * 4.0, 0.0, 1.0);
	ALBEDO = vec3(1.0, 0.75, 0.35) * glow;
	ALPHA = glow;
}
"""

const BACKGROUND_SHADER := """
shader_type canvas_item;
uniform float frame;
void fragment() {
	float band = 0.5 + 0.5 * sin(UV.y * 12.0 + frame * 0.03);
	COLOR = vec4(mix(vec3(0.05, 0.07, 0.12), vec3(0.14, 0.10, 0.22), band), 1.0);
}
"""

const POST_SHADER := """
shader_type canvas_item;
uniform sampler2D screen : hint_screen_texture, filter_linear;
uniform float frame;
void fragment() {
	vec3 color = texture(screen, SCREEN_UV).rgb;
	vec2 d = SCREEN_UV - vec2(0.5);
	float vignette = 1.0 - dot(d, d) * 1.2;
	COLOR = vec4(color * vignette * vec3(1.0, 0.97, 0.92 + 0.04 * sin(frame * 0.01)), 1.0);
}
"""

var conf := DEFAULTS.duplicate()
var conf_path := "user://bench.conf"
var expected_driver := ""
var hash_frame := 1000
var snapshot := false
var failures := 0
var frame := 0
var started_us := 0
var next_second := 1
var frames_this_second := 0
var finished := false
var touched := false
var rows := PackedStringArray(["index,tick_ns,cpu_ns"])
var materials: Array[ShaderMaterial] = []
var spin := Node3D.new()


func _ready() -> void:
	_read_arguments()
	_read_conf()
	var meshes: int = LOADS[conf["load"]]
	Engine.max_fps = conf["rate"]
	_build_scene(meshes)
	var method := RenderingServer.get_current_rendering_method()
	var driver := RenderingServer.get_current_rendering_driver_name()
	print("godot-bench: start %s rate %d load %s seconds %d meshes %d" % [conf["run"], conf["rate"], conf["load"], conf["seconds"], meshes])
	print("godot-bench: rendering %s %s" % [method, driver])
	print("godot-bench-device: %s, %s, %s" % [OS.get_name(), RenderingServer.get_video_adapter_name(), RenderingServer.get_video_adapter_api_version()])
	if expected_driver != "" and driver != expected_driver:
		_fail("drawing with %s, not %s" % [driver, expected_driver])


func _process(_delta: float) -> void:
	if finished:
		return
	var now_us := Time.get_ticks_usec()
	if frame == 0:
		started_us = now_us
	var cpu_seconds := Performance.get_monitor(Performance.TIME_PROCESS) + Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS)
	var cpu_ns := roundi(cpu_seconds * 1e9 + RenderingServer.get_frame_setup_time_cpu() * 1e6)
	rows.append("%d,%d,%d" % [frame, now_us * 1000, cpu_ns])
	OS.set_environment("GODOT_BENCH_FRAME", str(frame))
	_animate(frame)
	if frame == hash_frame:
		RenderingServer.frame_post_draw.connect(_hash_frame, CONNECT_ONE_SHOT)
	frame += 1
	frames_this_second += 1
	var elapsed := float(now_us - started_us) / 1e6
	if elapsed >= next_second:
		print("godot-bench-device: second %d: %d frames" % [next_second, frames_this_second])
		frames_this_second = 0
		next_second += 1
	if elapsed >= conf["seconds"]:
		_finish()


# The first touch only: how many of a run's taps land does not matter.
func _input(event: InputEvent) -> void:
	if touched or not (event is InputEventScreenTouch and event.pressed):
		return
	touched = true
	var size := get_viewport().get_visible_rect().size
	print("godot-bench: first touch at %d %% x %d %%" % [roundi(event.position.x / size.x * 100.0), roundi(event.position.y / size.y * 100.0)])


func _read_arguments() -> void:
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--bench-config="):
			conf_path = argument.trim_prefix("--bench-config=")
		elif argument.begins_with("--bench-driver="):
			expected_driver = argument.trim_prefix("--bench-driver=")
		elif argument.begins_with("--bench-hash-frame=") and argument.trim_prefix("--bench-hash-frame=").is_valid_int():
			hash_frame = argument.trim_prefix("--bench-hash-frame=").to_int()
		elif argument == "--bench-snapshot":
			snapshot = true


# bench.conf over the defaults, line by line as graphics/bench/scene.c reads it: a line it cannot take is a failure.
func _read_conf() -> void:
	if not FileAccess.file_exists(conf_path):
		print("godot-bench: no bench.conf: the emulator's defaults")
		return
	for line in FileAccess.get_file_as_string(conf_path).split("\n", false):
		var entry := line.strip_edges()
		if entry.is_empty():
			continue
		var parts := entry.split("=", true, 1)
		var key := parts[0]
		var value := parts[1] if parts.size() == 2 else ""
		if key == "run" and not value.is_empty() and value.length() < 64 and _is_name(value):
			conf["run"] = value
		elif key == "rate" and value.is_valid_int() and value.to_int() >= 1 and value.to_int() <= 240:
			conf["rate"] = value.to_int()
		elif key == "load" and LOADS.has(value):
			conf["load"] = value
		elif key == "seconds" and value.is_valid_int() and value.to_int() >= 1 and value.to_int() <= 3600:
			conf["seconds"] = value.to_int()
		else:
			_fail("bench.conf: cannot take %s" % entry)


func _is_name(text: String) -> bool:
	for character in text:
		if not NAME_CHARACTERS.contains(character):
			return false
	return true


@warning_ignore("integer_division")
func _build_scene(meshes: int) -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_CANVAS
	environment.background_canvas_max_layer = -1
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.25, 0.25, 0.3)
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)

	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 14)
	add_child(camera)
	camera.make_current()
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-40, 30, 0)
	light.shadow_enabled = false
	add_child(light)

	# The spheres: a cube of N, each a node of its own and so a draw of its own, about 768 triangles each.
	var side := ceili(pow(meshes, 1.0 / 3.0))
	var spacing := 9.0 / side
	var sphere := SphereMesh.new()
	sphere.radius = spacing * 0.3
	sphere.height = spacing * 0.6
	sphere.radial_segments = 24
	sphere.rings = 16
	var sphere_material := _material(SPHERE_SHADER)
	sphere_material.set_shader_parameter("albedo_texture", _checker_texture())
	add_child(spin)
	var center := Vector3(side - 1, side - 1, side - 1) * 0.5
	for index in meshes:
		var instance := MeshInstance3D.new()
		instance.mesh = sphere
		instance.material_override = sphere_material
		instance.position = (Vector3(index % side, (index / side) % side, index / (side * side)) - center) * spacing
		spin.add_child(instance)

	# The particles: N quads in one instanced draw, on a spiral in front of the spheres.
	var quad := QuadMesh.new()
	quad.size = Vector2(0.08, 0.08)
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.mesh = quad
	multimesh.instance_count = meshes
	for index in meshes:
		var angle := index * 2.39996323
		var radius := 6.0 * sqrt(float(index) / meshes)
		multimesh.set_instance_transform(index, Transform3D(Basis(), Vector3(cos(angle) * radius, sin(angle) * radius, 4.0 + (index % 7) * 0.1)))
	var particles := MultiMeshInstance3D.new()
	particles.multimesh = multimesh
	particles.material_override = _material(PARTICLE_SHADER)
	add_child(particles)

	# The background, drawn behind the 3D world as its canvas, and the post pass over the finished frame.
	_full_screen(-1, BACKGROUND_SHADER)
	_full_screen(1, POST_SHADER)


func _material(code: String) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = code
	var material := ShaderMaterial.new()
	material.shader = shader
	materials.append(material)
	return material


func _full_screen(layer: int, code: String) -> void:
	var canvas := CanvasLayer.new()
	canvas.layer = layer
	var rect := ColorRect.new()
	rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	rect.material = _material(code)
	canvas.add_child(rect)
	add_child(canvas)


func _checker_texture() -> ImageTexture:
	var image := Image.create_empty(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var light := ((x >> 3) + (y >> 3)) % 2 == 0
			image.set_pixel(x, y, Color(0.9, 0.85, 0.7) if light else Color(0.25, 0.4, 0.6))
	image.generate_mipmaps()
	return ImageTexture.create_from_image(image)


func _animate(index: int) -> void:
	for material in materials:
		material.set_shader_parameter("frame", float(index))
	spin.rotation = Vector3(index * 0.003, index * 0.005, 0.0)


func _hash_frame() -> void:
	var image := get_viewport().get_texture().get_image()
	if image == null or image.is_empty():
		print("godot-bench-device: frame %d: no image" % hash_frame)
		return
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(image.get_data())
	print("godot-bench-device: frame %d sha256 %s, %dx%d" % [hash_frame, context.finish().hex_encode(), image.get_width(), image.get_height()])
	if snapshot:
		image.save_png(conf_path.get_base_dir().path_join("snapshot.png"))


func _fail(what: String) -> void:
	failures += 1
	print("godot-bench: failure: %s" % what)


func _finish() -> void:
	finished = true
	var folder := conf_path.get_base_dir()
	_write(folder.path_join("scene.csv"), "\n".join(rows) + "\n")
	print("run %s: %d frames, %d failures" % [conf["run"], frame, failures])
	_write(folder.path_join("done"), "%d frames\n" % frame)
	OS.set_environment("GODOT_BENCH_DONE", "1")
	get_tree().quit()


func _write(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_fail("cannot write %s" % path)
		return
	file.store_string(text)
	file.close()
