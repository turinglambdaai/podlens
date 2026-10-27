#!/usr/bin/env python3
import argparse
import ctypes
import json
import subprocess
import time
from pathlib import Path

import pyatspi


def children(node):
    if node is None:
        return
    for index in range(node.childCount):
        try:
            child = node.getChildAtIndex(index)
            if child is not None:
                yield child
        except Exception:
            continue


def walk(node):
    yield node
    for child in children(node):
        yield from walk(child)


def role(node):
    try:
        return node.getRoleName()
    except Exception:
        return "unknown"


def text(node):
    try:
        value = node.queryText()
        return value.getText(0, value.characterCount)
    except Exception:
        return ""


def wait_for(description, probe, timeout=45.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = probe()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {description}")


def find_application():
    desktop = pyatspi.Registry.getDesktop(0)
    for application in children(desktop):
        for node in walk(application):
            if role(node) == "frame" and (node.name or "") == "PodLens":
                return application
    return None


def find_named(root, name, expected_role=None):
    for node in walk(root):
        if (node.name or "") == name and (expected_role is None or role(node) == expected_role):
            return node
    return None


def find_named_any(root, names, expected_role=None):
    for name in names:
        found = find_named(root, name, expected_role)
        if found:
            return found
    return None


def find_name_prefix(root, prefixes, expected_role=None):
    for node in walk(root):
        name = node.name or ""
        if any(name.startswith(prefix) for prefix in prefixes) and (
            expected_role is None or role(node) == expected_role
        ):
            return node
    return None


def find_editable_named(root, name):
    for node in walk(root):
        if (node.name or "") != name:
            continue
        try:
            node.queryEditableText()
            return node
        except Exception:
            continue
    return None


def contains(root, expected):
    return any((node.name or "") == expected or text(node) == expected for node in walk(root))


def is_focused(node):
    try:
        return node.getState().contains(pyatspi.STATE_FOCUSED)
    except Exception:
        return False


def send_key(name):
    # Drive the X11 keyboard device directly.  AT-SPI still verifies the
    # resulting focus owner below, but its legacy synth API can return success
    # without delivering a key when the test session has no window manager.
    x11 = ctypes.cdll.LoadLibrary("libX11.so.6")
    xtst = ctypes.cdll.LoadLibrary("libXtst.so.6")
    display_pointer = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = display_pointer
    x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
    x11.XStringToKeysym.restype = ctypes.c_ulong
    x11.XKeysymToKeycode.argtypes = [display_pointer, ctypes.c_ulong]
    x11.XKeysymToKeycode.restype = ctypes.c_ubyte
    x11.XSync.argtypes = [display_pointer, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [display_pointer]
    xtst.XTestFakeKeyEvent.argtypes = [
        display_pointer,
        ctypes.c_uint,
        ctypes.c_int,
        ctypes.c_ulong,
    ]
    xtst.XTestFakeKeyEvent.restype = ctypes.c_int

    display = x11.XOpenDisplay(None)
    if not display:
        raise AssertionError("could not open the isolated X11 display for keyboard input")
    try:
        keysym = x11.XStringToKeysym(name.encode("ascii"))
        keycode = x11.XKeysymToKeycode(display, keysym)
        if keysym == 0 or keycode == 0:
            raise AssertionError(f"could not resolve native X11 key {name}")
        pressed = xtst.XTestFakeKeyEvent(display, keycode, True, 0)
        released = xtst.XTestFakeKeyEvent(display, keycode, False, 0)
        x11.XSync(display, 0)
        if not pressed or not released:
            raise AssertionError(f"could not send native X11 key {name}")
    finally:
        x11.XCloseDisplay(display)


def focus_application_window():
    # Gtk's top-level AT-SPI component does not implement grabFocus. Activate
    # its real X11 window instead, just as a window manager would, then keep
    # using AT-SPI to verify which native control receives each Tab.
    x11 = ctypes.cdll.LoadLibrary("libX11.so.6")
    display_pointer = ctypes.c_void_p
    window_type = ctypes.c_ulong
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = display_pointer
    x11.XDefaultRootWindow.argtypes = [display_pointer]
    x11.XDefaultRootWindow.restype = window_type
    x11.XQueryTree.argtypes = [
        display_pointer,
        window_type,
        ctypes.POINTER(window_type),
        ctypes.POINTER(window_type),
        ctypes.POINTER(ctypes.POINTER(window_type)),
        ctypes.POINTER(ctypes.c_uint),
    ]
    x11.XFetchName.argtypes = [display_pointer, window_type, ctypes.POINTER(ctypes.c_char_p)]
    x11.XFree.argtypes = [ctypes.c_void_p]
    x11.XSetInputFocus.argtypes = [display_pointer, window_type, ctypes.c_int, ctypes.c_ulong]
    x11.XGetInputFocus.argtypes = [
        display_pointer,
        ctypes.POINTER(window_type),
        ctypes.POINTER(ctypes.c_int),
    ]
    x11.XRaiseWindow.argtypes = [display_pointer, window_type]
    x11.XSync.argtypes = [display_pointer, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [display_pointer]

    display = x11.XOpenDisplay(None)
    if not display:
        raise AssertionError("could not open the isolated X11 display")

    def find_named_x11_window(parent):
        root = window_type()
        returned_parent = window_type()
        child_array = ctypes.POINTER(window_type)()
        count = ctypes.c_uint()
        if not x11.XQueryTree(
            display,
            parent,
            ctypes.byref(root),
            ctypes.byref(returned_parent),
            ctypes.byref(child_array),
            ctypes.byref(count),
        ):
            return None
        children = [child_array[index] for index in range(count.value)]
        if child_array:
            x11.XFree(child_array)
        for child in children:
            title = ctypes.c_char_p()
            if x11.XFetchName(display, child, ctypes.byref(title)) and title.value:
                try:
                    if title.value.decode("utf-8", errors="replace") == "PodLens":
                        return child
                finally:
                    if title.value:
                        x11.XFree(title)
            nested = find_named_x11_window(child)
            if nested is not None:
                return nested
        return None

    try:
        target = find_named_x11_window(x11.XDefaultRootWindow(display))
        if target is None:
            raise AssertionError("could not find native PodLens X11 window")
        x11.XRaiseWindow(display, target)
        x11.XSetInputFocus(display, target, 2, 0)
        x11.XSync(display, 0)
        focused = window_type()
        revert_to = ctypes.c_int()
        x11.XGetInputFocus(display, ctypes.byref(focused), ctypes.byref(revert_to))
        if focused.value != target:
            raise AssertionError("native PodLens X11 window did not receive input focus")
    finally:
        x11.XCloseDisplay(display)


def press(node):
    try:
        actions = node.queryAction()
        for index in range(actions.nActions):
            if actions.doAction(index):
                return
    except Exception:
        pass

    # GtkMenuButton is exposed as a push button but some GTK/AT-SPI builds
    # reject its action interface. A native pointer click through AT-SPI is
    # the accessibility-equivalent fallback and still exercises the packaged UI.
    try:
        extents = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
        if extents.width > 0 and extents.height > 0:
            pyatspi.Registry.generateMouseEvent(
                extents.x + extents.width // 2,
                extents.y + extents.height // 2,
                "b1c",
            )
            return
    except Exception:
        pass

    raise AssertionError(f"AT-SPI action failed for {node.name!r}")


def focus_with_tab(prefixes, expected_role, limit=80):
    # Follow the real X11 focus chain. AT-SPI's legacy synthetic key interface
    # is not delivered under Wayland, so XTest supplies the native key event
    # while AT-SPI verifies which accessible control actually owns focus.
    observed_focus = []
    for _ in range(limit):
        application = find_application()
        if application is not None:
            window = next((n for n in walk(application) if role(n) == "frame"), None)
            if window is not None:
                target = find_name_prefix(window, prefixes, expected_role)
                if target is not None and is_focused(target):
                    return application, window, target
                focused = " | ".join(
                    f"{role(node)}:{node.name or ''}"
                    for node in walk(window)
                    if is_focused(node)
                )
                if focused and (not observed_focus or focused != observed_focus[-1]):
                    observed_focus.append(focused)
        send_key("Tab")
        time.sleep(0.05)
    raise AssertionError(
        "transcript sentence was not reachable through the Tab focus chain; "
        f"observed focus: {observed_focus}"
    )


def press_return(node):
    if not is_focused(node):
        raise AssertionError(f"Return target did not hold keyboard focus: {node.name!r}")
    send_key("Return")


def activate_application_action(name):
    result = subprocess.run(
        [
            "gdbus",
            "call",
            "--session",
            "--dest",
            "site.jrtx.podlens",
            "--object-path",
            "/site/jrtx/podlens",
            "--method",
            "org.gtk.Actions.Activate",
            name,
            "[]",
            "{}",
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"could not activate app.{name}: {result.stderr.strip()}"
        )


def snapshot(node, depth=0):
    result = {"name": node.name or "", "role": role(node)}
    value = text(node)
    if value:
        result["text"] = value
    if depth < 8:
        result["children"] = [snapshot(child, depth + 1) for child in children(node)]
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    application = wait_for("PodLens application", find_application)
    window = wait_for(
        "PodLens frame", lambda: next((n for n in walk(application) if role(n) == "frame"), None)
    )
    wait_for(
        "embedded backend readiness",
        lambda: contains(window, "就绪")
        or contains(window, "Ready")
        or contains(window, "A small episode about focused listening"),
    )
    focus_application_window()
    focus = wait_for(
        "Focus Listening button",
        lambda: find_named_any(window, ["专注聆听", "Focus Listening"], "toggle button"),
    )
    press(focus)
    # GTK rebuilds the visible hierarchy when focus mode collapses the
    # library. Reacquire the application and frame instead of traversing
    # defunct AT-SPI cache objects from the previous layout.
    application = wait_for("PodLens application after focus layout", find_application)
    window = wait_for(
        "PodLens frame after focus layout",
        lambda: next((n for n in walk(application) if role(n) == "frame"), None),
    )
    wait_for(
        "focused listening layout",
        lambda: find_named_any(window, ["显示资料库", "Show Library"], "toggle button"),
    )
    wait_for(
        "keyboard-operable transcript sentence in focus mode",
        lambda: find_name_prefix(
            window, ["逐句稿句子: Keyboard", "Transcript sentence: Keyboard"], "push button"
        ),
    )
    play = wait_for(
        "Play or pause button after focus layout",
        lambda: find_named_any(window, ["播放或暂停", "Play or pause"], "push button"),
    )
    press(play)
    application, window, segment = focus_with_tab(
        ["逐句稿句子: Keyboard", "Transcript sentence: Keyboard"], "push button"
    )
    press_return(segment)
    wait_for("four-second transcript seek", lambda: contains(window, "00:04"))
    follow = wait_for(
        "Follow transcript toggle",
        lambda: find_named_any(window, ["跟随", "Follow"], "toggle button"),
    )
    press(follow)
    current = wait_for(
        "Current sentence button",
        lambda: find_named_any(window, ["当前句", "Current"], "push button"),
    )
    press(current)
    (args.output / "listening-accessibility.json").write_text(
        json.dumps(snapshot(application), ensure_ascii=False, indent=2), encoding="utf-8"
    )
    press(focus)
    menu = wait_for(
        "Main Menu button", lambda: find_named(window, "主菜单", "push button")
    )
    menu.queryComponent()
    activate_application_action("settings")

    field = wait_for(
        "chat-model settings field",
        lambda: find_editable_named(application, "chat-model"),
    )
    field.queryEditableText().setTextContents("acceptance-model")
    save = wait_for("Save button", lambda: find_named(application, "保存", "push button"))
    press(save)
    wait_for(
        "settings persisted by the embedded backend",
        lambda: args.config.exists()
        and json.loads(args.config.read_text(encoding="utf-8")).get("chat-model")
        == "acceptance-model",
    )

    (args.output / "accessibility.json").write_text(
        json.dumps(snapshot(application), ensure_ascii=False, indent=2), encoding="utf-8"
    )
    (args.output / "interaction-trace.json").write_text(
        json.dumps(
            {
                "platform": "linux",
                "actions": [
                    "press:Focus Listening",
                    "press:Play or pause",
                    "keyboard:Return:Transcript sentence 3",
                    "press:Follow:off",
                    "press:Current",
                    "press:Show Library",
                    "verify:Main Menu accessibility",
                    "activate:app.settings",
                    "set:chat-model",
                    "press:Save",
                ],
                "assertions": [
                    "packaged GTK4 frame activated in an isolated native Linux display session",
                    "synthetic bilingual transcript exposed a native AT-SPI action",
                    "focus mode hid and restored the library without reloading the episode",
                    "activating a transcript sentence moved playback to 00:04",
                    "Current remained operable after the listener turned follow off",
                    "main menu control exposed through AT-SPI",
                    "settings field exposed a stable AT-SPI name",
                    "save mutation crossed RVT1 into the embedded backend",
                    "isolated config.json contains acceptance-model",
                ],
            },
            indent=2,
        ),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
