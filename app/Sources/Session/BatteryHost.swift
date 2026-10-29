import APKRuntime
import APKSession
import UIKit

/// The host's battery for the guest (the battery, gap 1 of the compatibility plan): while a session is open, UIDevice
/// reports the battery, and each level and state it has reaches the runtime (`apkrt_battery_set`), which keeps it for
/// the guest's first look (APKRT_NR_BATTERY) and announces each change (APKRT_EVENT_BATTERY); the framework makes
/// Android's sticky ACTION_BATTERY_CHANGED of it. On the main thread, as UIDevice's notifications come.
@MainActor
final class BatteryHost {
    private var observers: [any NSObjectProtocol] = []

    /// Turns UIDevice's battery monitoring on and sets the battery as it is now, before the guest starts.
    func start() {
        guard observers.isEmpty else { return }
        UIDevice.current.isBatteryMonitoringEnabled = true
        let center = NotificationCenter.default
        for name in [UIDevice.batteryLevelDidChangeNotification, UIDevice.batteryStateDidChangeNotification] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.report() }
            })
        }
        report()
    }

    /// The session closed: no more battery news, and UIDevice's monitoring off again.
    func stop() {
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        observers = []
        UIDevice.current.isBatteryMonitoringEnabled = false
    }

    private func report() {
        let device = UIDevice.current
        let reading = BatteryReading(level: device.batteryLevel, state: device.batteryState.rawValue)
        apkrt_battery_set(reading.percent, reading.state)
    }
}
