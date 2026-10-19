import AppKit
import ApplicationServices
import CoreGraphics
import Foundation

enum DriverError: Error, CustomStringConvertible {
    case failure(String)
    var description: String { switch self { case .failure(let message): return message } }
}

func attribute(_ element: AXUIElement, _ name: String) -> CFTypeRef? {
    var value: CFTypeRef?
    return AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success ? value : nil
}

func stringAttribute(_ element: AXUIElement, _ name: String) -> String {
    attribute(element, name) as? String ?? ""
}

func children(_ element: AXUIElement) -> [AXUIElement] {
    attribute(element, kAXChildrenAttribute) as? [AXUIElement] ?? []
}

func walk(_ element: AXUIElement) -> [AXUIElement] {
    [element] + children(element).flatMap(walk)
}

func waitFor<T>(_ description: String, timeout: TimeInterval = 45, probe: () -> T?) throws -> T {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if let value = probe() { return value }
        Thread.sleep(forTimeInterval: 0.05)
    }
    throw DriverError.failure("timed out waiting for \(description)")
}

func containsText(_ root: AXUIElement, _ expected: Set<String>) -> Bool {
    walk(root).contains { element in
        [kAXTitleAttribute, kAXValueAttribute, kAXDescriptionAttribute]
            .contains { expected.contains(stringAttribute(element, $0)) }
    }
}

func findField(_ root: AXUIElement, label: String) -> AXUIElement? {
    walk(root).first { element in
        stringAttribute(element, kAXRoleAttribute) == (kAXTextFieldRole as String) &&
            [kAXTitleAttribute, kAXDescriptionAttribute]
                .contains { stringAttribute(element, $0) == label }
    }
}

func findButton(_ root: AXUIElement, titles: Set<String>) -> AXUIElement? {
    walk(root).first { element in
        stringAttribute(element, kAXRoleAttribute) == (kAXButtonRole as String) &&
            titles.contains(stringAttribute(element, kAXTitleAttribute))
    }
}

func press(_ element: AXUIElement, _ description: String) throws {
    let result = AXUIElementPerformAction(element, kAXPressAction as CFString)
    guard result == .success else {
        throw DriverError.failure("AXPress failed for \(description): \(result.rawValue)")
    }
}

func sendCommandComma() throws {
    guard let source = CGEventSource(stateID: .hidSystemState),
          let down = CGEvent(keyboardEventSource: source, virtualKey: 43, keyDown: true),
          let up = CGEvent(keyboardEventSource: source, virtualKey: 43, keyDown: false) else {
        throw DriverError.failure("could not create Command-comma events")
    }
    down.flags = .maskCommand
    up.flags = .maskCommand
    down.post(tap: .cghidEventTap)
    up.post(tap: .cghidEventTap)
}

func snapshot(_ element: AXUIElement, depth: Int = 0) -> [String: Any] {
    var result: [String: Any] = [
        "role": stringAttribute(element, kAXRoleAttribute),
        "identifier": stringAttribute(element, kAXIdentifierAttribute),
        "title": stringAttribute(element, kAXTitleAttribute),
        "label": stringAttribute(element, kAXDescriptionAttribute),
        "value": stringAttribute(element, kAXValueAttribute),
    ]
    if depth < 8 { result["children"] = children(element).map { snapshot($0, depth: depth + 1) } }
    return result
}

func writeJSON(_ value: Any, to url: URL) throws {
    try JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys])
        .write(to: url)
}

var diagnosticApplication: AXUIElement?
var diagnosticOutput: URL?

do {
    let arguments = CommandLine.arguments
    guard let pidIndex = arguments.firstIndex(of: "--pid"), pidIndex + 1 < arguments.count,
          let pid = Int32(arguments[pidIndex + 1]),
          let outputIndex = arguments.firstIndex(of: "--output"), outputIndex + 1 < arguments.count,
          let configIndex = arguments.firstIndex(of: "--config"), configIndex + 1 < arguments.count else {
        throw DriverError.failure("usage: macos-ax --pid PID --output DIRECTORY --config FILE")
    }
    let output = URL(fileURLWithPath: arguments[outputIndex + 1], isDirectory: true)
    let config = URL(fileURLWithPath: arguments[configIndex + 1])
    diagnosticOutput = output
    try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
    guard AXIsProcessTrusted() else { throw DriverError.failure("accessibility access is unavailable") }

    let application = AXUIElementCreateApplication(pid)
    diagnosticApplication = application
    let window: AXUIElement = try waitFor("PodLens window") {
        (attribute(application, kAXWindowsAttribute) as? [AXUIElement])?.first
    }
    _ = AXUIElementSetAttributeValue(application, kAXFrontmostAttribute as CFString, kCFBooleanTrue)
    _ = AXUIElementPerformAction(window, kAXRaiseAction as CFString)
    _ = try waitFor("native PodLens content") {
        containsText(window, ["Discover", "发现", "Choose an episode to begin", "选择一集开始"]) ? true : nil
    }

    try sendCommandComma()
    let field: AXUIElement = try waitFor("chat-model settings field") {
        findField(application, label: "chat-model")
    }
    guard AXUIElementSetAttributeValue(
        field, kAXValueAttribute as CFString, "acceptance-model" as CFString
    ) == .success else {
        throw DriverError.failure("could not set chat-model")
    }
    let save: AXUIElement = try waitFor("settings save button") {
        findButton(application, titles: ["Saved", "已保存"])
    }
    try press(save, "settings save")

    _ = try waitFor("settings persisted by the embedded backend") { () -> Bool? in
        guard let data = try? Data(contentsOf: config),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              object["chat-model"] as? String == "acceptance-model" else { return nil }
        return true
    }

    try writeJSON(snapshot(application), to: output.appendingPathComponent("accessibility.json"))
    try writeJSON([
        "platform": "macos",
        "actions": ["shortcut:Command-comma", "set:chat-model", "press:Saved"],
        "assertions": [
            "packaged SwiftUI window activated",
            "settings field exposed a stable accessibility label",
            "save mutation crossed RVT1 into the embedded backend",
            "isolated config.json contains acceptance-model",
            "native application accepted termination",
        ],
    ], to: output.appendingPathComponent("interaction-trace.json"))

    guard let running = NSRunningApplication(processIdentifier: pid), running.terminate() else {
        throw DriverError.failure("native application rejected termination")
    }
} catch {
    if let application = diagnosticApplication, let output = diagnosticOutput {
        try? writeJSON(snapshot(application), to: output.appendingPathComponent("accessibility-failure.json"))
    }
    FileHandle.standardError.write(Data("PodLens macOS acceptance failed: \(error)\n".utf8))
    exit(1)
}
