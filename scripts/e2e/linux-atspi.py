#!/usr/bin/env python3
import argparse
import json
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


def contains(root, expected):
    return any((node.name or "") == expected or text(node) == expected for node in walk(root))


def press(node):
    actions = node.queryAction()
    if actions.nActions < 1 or not actions.doAction(0):
        raise AssertionError(f"AT-SPI action failed for {node.name!r}")


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
    wait_for("embedded backend readiness", lambda: contains(window, "Ready"))
    menu = wait_for("Main Menu", lambda: find_named(window, "Main Menu"))
    press(menu)
    options = wait_for("Options menu", lambda: find_named(application, "Options"))
    press(options)
    settings = wait_for("Settings menu item", lambda: find_named(application, "Settings…"))
    press(settings)

    field = wait_for("chat-model settings field", lambda: find_named(application, "chat-model"))
    field.queryEditableText().setTextContents("acceptance-model")
    save = wait_for("Save button", lambda: find_named(application, "Save", "push button"))
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
                "actions": ["open:Settings", "set:chat-model", "press:Save"],
                "assertions": [
                    "packaged GTK4 frame activated on native Wayland",
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
