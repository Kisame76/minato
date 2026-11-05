import APKRuntime
import APKSession
import CoreLocation
import Foundation
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "location")

/// ADR-0063: the runtime's location hub fed from CoreLocation, and iOS's when-in-use prompt for the permission host.
/// One CLLocationManager for the process, worked on the main queue, where its delegate runs too; the hub's calls only
/// record what is wanted and hop there. Continuous updates follow the guest's wanted accuracy (kCLLocationAccuracyBest,
/// or kCLLocationAccuracyHundredMeters for a coarse one, which the runtime coarsens) and distance filter; a
/// getCurrentLocation is CoreLocation's requestLocation, or the next continuous fix while updates run, and a failure
/// answers it with no location. Every fix goes to the hub in
/// `struct apkrt_location_fix`'s terms (LocationFixValues); the hub gates and coarsens it by the app's grant. Without
/// a background mode iOS pauses the updates in the background and resumes them on the return (decision 4).
final class LocationHost: NSObject, CLLocationManagerDelegate, @unchecked Sendable {
    static let shared = LocationHost()

    /// Main queue only, as everything below.
    private var manager: CLLocationManager?
    private var wanted: (accuracy: Int32, distance: Int32)?
    private var singles: Set<UInt64> = []
    private var asking: [(PermissionDesk.IOSState) -> Void] = []
    private var updating = false

    /// The manager, made on the main queue at its first use.
    @MainActor private var locations: CLLocationManager {
        if let manager { return manager }
        let made = CLLocationManager()
        made.delegate = self
        manager = made
        return made
    }

    /// iOS's answer for location, which is Minato's for every app: reduced accuracy when precise location is off.
    @MainActor var state: PermissionDesk.IOSState {
        let manager = locations
        switch manager.authorizationStatus {
        case .notDetermined: return .notDetermined
        case .authorizedWhenInUse, .authorizedAlways:
            return manager.accuracyAuthorization == .reducedAccuracy ? .grantedReduced : .granted
        default: return .denied
        }
    }

    /// iOS's when-in-use prompt, once Minato's alert said yes (ADR-0063 decision 2); `done` gets iOS's answer.
    @MainActor func requestAuthorization(_ done: @escaping (PermissionDesk.IOSState) -> Void) {
        guard state == .notDetermined else { return done(state) }
        asking.append(done)
        if asking.count == 1 { locations.requestWhenInUseAuthorization() }
    }

    /// Tells the hub this host serves it; once, before the guest runs.
    func register() {
        var host = apkrt_location_host()
        host.context = Unmanaged.passUnretained(self).toOpaque()
        host.start = { context, accuracy, distance in
            guard let context else { return -EINVAL }
            let host = Unmanaged<LocationHost>.fromOpaque(context).takeUnretainedValue()
            DispatchQueue.main.async { MainActor.assumeIsolated { host.wanted = (accuracy, distance); host.apply() } }
            return 0
        }
        host.stop = { context in
            guard let context else { return }
            let host = Unmanaged<LocationHost>.fromOpaque(context).takeUnretainedValue()
            DispatchQueue.main.async { MainActor.assumeIsolated { host.wanted = nil; host.apply() } }
        }
        host.request_single = { context, token in
            guard let context else { return -EINVAL }
            let host = Unmanaged<LocationHost>.fromOpaque(context).takeUnretainedValue()
            DispatchQueue.main.async { MainActor.assumeIsolated { host.singles.insert(token); host.apply() } }
            return 0
        }
        host.cancel_single = { context, token in
            guard let context else { return }
            let host = Unmanaged<LocationHost>.fromOpaque(context).takeUnretainedValue()
            DispatchQueue.main.async { MainActor.assumeIsolated { _ = host.singles.remove(token) } }
        }
        apkrt_location_set_host(&host)
    }

    /// Brings CoreLocation to what the guest wants.
    @MainActor private func apply() {
        let manager = locations
        if let wanted {
            // A coarse request is coarsened by the runtime, as Android's service does; iOS's reduced accuracy would
            // move the fix a second time, a few km more than Android 15's coarse fixes (location-manager-test-coarse).
            manager.desiredAccuracy = wanted.accuracy == Int32(APKRT_LOCATION_ACCURACY_COARSE)
                ? kCLLocationAccuracyHundredMeters : kCLLocationAccuracyBest
            manager.distanceFilter = wanted.distance > 0 ? CLLocationDistance(wanted.distance) : kCLDistanceFilterNone
            if !updating {
                updating = true
                manager.startUpdatingLocation()
            }
        } else {
            if updating {
                updating = false
                manager.stopUpdatingLocation()
            }
            if !singles.isEmpty {
                manager.desiredAccuracy = kCLLocationAccuracyBest
                manager.requestLocation()
            }
        }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations updates: [CLLocation]) {
        MainActor.assumeIsolated {
            let now = Date()
            let boottime = Int64(clock_gettime_nsec_np(CLOCK_MONOTONIC))
            var last: apkrt_location_fix?
            for update in updates {
                guard let values = LocationFixValues(Self.reading(update), now: now, boottime: boottime)
                else { continue }
                var fix = Self.fix(values)
                if updating { apkrt_location_push(&fix) }
                last = fix
            }
            guard var last, !singles.isEmpty else { return }
            for token in singles { apkrt_location_push_single(token, &last) }
            singles.removeAll()
        }
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: any Error) {
        MainActor.assumeIsolated {
            log.info("location failed: \(error.localizedDescription, privacy: .public)")
            // A continuous run goes on (CoreLocation retries); a single ends with no location.
            guard !updating || (error as? CLError)?.code == .denied else {
                return
            }
            for token in singles { apkrt_location_push_single(token, nil) }
            singles.removeAll()
        }
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        MainActor.assumeIsolated {
            let state = self.state
            guard state != .notDetermined, !asking.isEmpty else { return }
            let waiting = asking
            asking.removeAll()
            for done in waiting { done(state) }
        }
    }

    private static func reading(_ location: CLLocation) -> LocationReading {
        LocationReading(latitude: location.coordinate.latitude, longitude: location.coordinate.longitude,
                        altitude: location.ellipsoidalAltitude, horizontalAccuracy: location.horizontalAccuracy,
                        verticalAccuracy: location.verticalAccuracy, speed: location.speed,
                        speedAccuracy: location.speedAccuracy, course: location.course,
                        courseAccuracy: location.courseAccuracy, timestamp: location.timestamp)
    }

    private static func fix(_ values: LocationFixValues) -> apkrt_location_fix {
        var fix = apkrt_location_fix()
        fix.latitude = values.latitude
        fix.longitude = values.longitude
        fix.altitude = values.altitude
        fix.horizontal_accuracy_m = values.horizontalAccuracy
        fix.vertical_accuracy_m = values.verticalAccuracy
        fix.speed_mps = values.speed
        fix.speed_accuracy_mps = values.speedAccuracy
        fix.bearing_deg = values.bearing
        fix.bearing_accuracy_deg = values.bearingAccuracy
        fix.time_ms = values.timeMs
        fix.elapsed_realtime_ns = values.elapsedRealtimeNs
        fix.provider = LocationFixValues.providerFused
        fix.valid = values.valid
        return fix
    }
}
