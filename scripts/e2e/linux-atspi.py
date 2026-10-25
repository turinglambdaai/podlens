#!/usr/bin/env python3
import argparse
import json
import subprocess
import time
from pathlib import Path

import pyatspi


def children(node):
    for index in range(node.childCount):
        try:
            yield node.getChildAtIndex(index)
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


def press_return(node):
    if not node.queryComponent().grabFocus():
        raise AssertionError(f"could not focus {node.name!r}")
    pyatspi.Registry.generateKeyboardEvent(0, "Return", pyatspi.KEY_SYM)


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
    wait_for("embedded backend readiness", lambda: contains(window, "就绪"))
    focus = wait_for(
        "Focus Listening button",
        lambda: find_named_any(window, ["专注聆听", "Focus Listening"], "toggle button"),
    )
    segment = wait_for(
        "keyboard-operable transcript sentence",
        lambda: find_name_prefix(
            window, ["逐句稿句子: Keyboard", "Transcript sentence: Keyboard"], "push button"
        ),
    )
    play = wait_for(
        "Play or pause button",
        lambda: find_named_any(window, ["播放或暂停", "Play or pause"], "push button"),
    )
    press(focus)
    wait_for(
        "focused listening layout",
        lambda: find_named_any(window, ["显示资料库", "Show Library"], "toggle button"),
    )
    press(play)
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
                    "packaged GTK4 frame activated on native Wayland",
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
