// Sends your open Apple Reminders to the AMOLED calendar and completes the ones you tick off on it.
// Runs in the background (launchd agent, see install.sh). Pushes on every Reminders change and every
// 5 minutes; asks the board for ticked-off reminders every 30 seconds.
import EventKit
import Foundation

let board = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "amoled-calendar.local"
let secrets = (try? String(contentsOfFile: NSHomeDirectory() + "/esp/amoled_demo/main/secrets.h", encoding: .utf8)) ?? ""
guard let tokenRange = secrets.range(of: #"OTA_TOKEN "([^"]+)""#, options: .regularExpression) else {
    fputs("no OTA_TOKEN in secrets.h\n", stderr); exit(1)
}
let token = String(secrets[tokenRange].split(separator: "\"")[1])
let store = EKEventStore()

func log(_ s: String) { print("\(Date().formatted(date: .omitted, time: .standard)) \(s)"); fflush(stdout) }

func request(_ path: String, method: String, body: Data? = nil) async -> Data? {
    var req = URLRequest(url: URL(string: "http://\(board)\(path)")!, timeoutInterval: 10)
    req.httpMethod = method
    req.setValue(token, forHTTPHeaderField: "X-OTA-Token")
    if let body { req.httpBody = body; req.setValue("application/json", forHTTPHeaderField: "Content-Type") }
    guard let (data, resp) = try? await URLSession.shared.data(for: req), (resp as? HTTPURLResponse)?.statusCode == 200 else { return nil }
    return data
}

func hex(_ c: CGColor?) -> String {
    guard let c = c?.converted(to: CGColorSpace(name: CGColorSpace.sRGB)!, intent: .defaultIntent, options: nil),
          let k = c.components, k.count >= 3 else { return "#FF9500" }
    return String(format: "#%02X%02X%02X", Int(k[0] * 255), Int(k[1] * 255), Int(k[2] * 255))
}

// Open reminders that have a due date, from 60 days overdue up to a week ahead
func openReminders() async -> [EKReminder] {
    let start = Calendar.current.date(byAdding: .day, value: -60, to: Date())!
    let end = Calendar.current.date(byAdding: .day, value: 8, to: Calendar.current.startOfDay(for: Date()))!
    let pred = store.predicateForIncompleteReminders(withDueDateStarting: start, ending: end, calendars: nil)
    return await withCheckedContinuation { cont in store.fetchReminders(matching: pred) { cont.resume(returning: $0 ?? []) } }
}

func push() async {
    let items: [[String: Any]] = await openReminders().compactMap { r in
        guard let dc = r.dueDateComponents, let due = Calendar.current.date(from: dc) else { return nil }
        return ["id": r.calendarItemIdentifier, "title": r.title ?? "", "list": r.calendar.title,
                "due": Int(due.timeIntervalSince1970), "time": dc.hour != nil, "color": hex(r.calendar.cgColor)]
    }
    let body = try! JSONSerialization.data(withJSONObject: items)
    log(await request("/reminders", method: "POST", body: body) != nil ? "sent \(items.count) reminders" : "board not reachable")
}

func completeTicked() async {
    guard let data = await request("/reminders/done", method: "GET"),
          let ids = try? JSONSerialization.jsonObject(with: data) as? [String], !ids.isEmpty else { return }
    for id in ids {
        guard let r = store.calendarItem(withIdentifier: id) as? EKReminder, !r.isCompleted else { continue }
        r.isCompleted = true
        do { try store.save(r, commit: true); log("completed \"\(r.title ?? "")\" (ticked on the board)") }
        catch { log("couldn't complete \(id): \(error)") }
    }
    await push()                                     // the board drops it once it's no longer sent
}

guard (try? await store.requestFullAccessToReminders()) == true else {
    log("No access to Reminders. Allow it in System Settings > Privacy & Security > Reminders.")
    exit(1)
}
log("syncing reminders with \(board)")
await push()

var pending: Task<Void, Never>?
NotificationCenter.default.addObserver(forName: .EKEventStoreChanged, object: store, queue: .main) { _ in
    pending?.cancel()                                // a burst of changes: send once, 2 s after the last
    pending = Task { try? await Task.sleep(for: .seconds(2)); if !Task.isCancelled { await push() } }
}
Task { while true { try? await Task.sleep(for: .seconds(30)); await completeTicked() } }
Task { while true { try? await Task.sleep(for: .seconds(300)); await push() } }
while true { try? await Task.sleep(for: .seconds(3600)) }   // keep running; the tasks above do the work
