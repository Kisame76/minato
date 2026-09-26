"""Where each argument of a function lies in Android's ABI (AAPCS64) and in Apple's arm64 ABI, and what a
call from guest code into host code therefore needs (ADR-0030, decisions 2 and 6).

Both ABIs pass the first eight integer and pointer arguments in x0-x7 and the first eight floating-point
ones in v0-v7, and leave the upper 32 bits of a 32-bit argument unspecified. They differ in three places:
AAPCS64 leaves the upper bits of an argument under 32 bits unspecified where Apple's callee expects them
extended by the type (extend); AAPCS64 gives each stack argument an 8-byte slot where Apple packs them at
their natural size and alignment (repack); and plain char is unsigned in AAPCS64 and signed on Apple
platforms. Results under 32 bits need nothing: the guest caller extends them itself."""


class Refused(Exception):
    """What never crosses between the two ABIs."""


class CType:
    """A value's type as the ABIs see it. kind is int, float, pointer, callback (a function pointer, named
    in callback), struct (a structure or union by value), char (plain char), long double, va_list,
    variadic, void or unknown; base is the type's name as the registry writes it."""

    __slots__ = ("kind", "size", "signed", "text", "callback", "base")

    def __init__(self, kind, size=0, signed=None, text="", callback=None, base=None):
        self.kind, self.size, self.signed, self.text, self.callback = kind, size, signed, text, callback
        self.base = base

    def __repr__(self):
        return f"CType({self.kind!r}, {self.size}, {self.signed}, {self.text!r})"


REFUSED = {
    "struct": "a structure by value",
    "char": "a char by value, whose signedness differs",
    "long double": "a long double, 128 bits in AAPCS64 and 64 on Apple platforms",
    "va_list": "a va_list",
    "variadic": "a variadic function",
    "unknown": "a type the generator does not know",
}
EXTEND = {(1, False): "uxtb", (2, False): "uxth", (1, True): "sxtb", (2, True): "sxth"}


def refusal(value, what):
    if value.kind in REFUSED:
        return f"{REFUSED[value.kind]} ({what} {value.text!r})"
    return None


def places(params):
    """Per argument its place: ("x", n) or ("v", n) for a register, or ("stack", aapcs, apple) with its offset
    in each ABI's stack area."""
    general = floating = 0
    apple_offset = 0
    stack = 0
    result = []
    for param in params:
        if param.kind == "float":
            if floating < 8:
                result.append(("v", floating))
                floating += 1
                continue
        elif general < 8:
            result.append(("x", general))
            general += 1
            continue
        apple_offset = (apple_offset + param.size - 1) // param.size * param.size
        result.append(("stack", 8 * stack, apple_offset))
        apple_offset += param.size
        stack += 1
    return result


def classify(result, params):
    """The call's class and what it changes: {"abi": "same" | "extend" | "repack", "extend": [[register,
    instruction], ...], "stack": [[aapcs offset, apple offset, size], ...]}. Raises Refused for what never
    crosses."""
    for index, value in [(None, result)] + list(enumerate(params)):
        why = refusal(value, "result" if index is None else f"argument {index + 1}")
        if why:
            raise Refused(why)
    extend, stack, repack = [], [], False
    for param, place in zip(params, places(params)):
        small = param.kind == "int" and param.size < 4
        if place[0] == "stack":
            stack.append([place[1], place[2], param.size])
            repack = repack or small or place[1] != place[2]
        elif small:
            extend.append([f"w{place[1]}", EXTEND[(param.size, bool(param.signed))]])
    if repack:
        return {"abi": "repack", "extend": extend, "stack": stack}
    return {"abi": "extend" if extend else "same", "extend": extend, "stack": stack}


def code(value):
    """A value in the report's short form: i4s is a signed 32-bit integer, f8 a double, p a pointer, cb a
    callback, v void; other kinds keep their names."""
    if value.kind == "int":
        return f"i{value.size}{'s' if value.signed else 'u'}"
    if value.kind == "float":
        return f"f{value.size}"
    return {"pointer": "p", "callback": "cb", "void": "v"}.get(value.kind, value.kind)


def signature(result, params):
    return f"{code(result)}({', '.join(code(param) for param in params)})"


def check_callback(name, result, params):
    """A callback's arguments are 32-bit or wider, or pointers, and its result 32-bit, a pointer or void
    (ADR-0030, decision 5): then it needs no conversion either way."""
    for index, value in [(None, result)] + list(enumerate(params)):
        what = "result" if index is None else f"argument {index + 1}"
        why = refusal(value, what)
        if why:
            raise Refused(f"callback {name}: {why}")
        if value.kind == "int" and value.size < 4:
            raise Refused(f"callback {name}: a {what} under 32 bits ({value.text!r})")
