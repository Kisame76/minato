import APKRuntime
import APKSession
import CoreMotion
import Foundation
import Synchronization
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "sensors")

/// ADR-0061 step 3: the runtime's sensor hub fed from CoreMotion. One CMMotionManager for the process, worked on the
/// host's own serial queue, where its handlers run too; the hub's start and stop only record (SensorFeeds) and kick
/// that queue once (later kicks while one waits are the same), which then starts, re-rates or stops the feeds
/// MotionPlan names. Each sample goes to the hub in
/// Android's units and frames (SensorValues), with CoreMotion's timestamp, seconds since boot without sleep, which the
/// hub shifts to the guest's CLOCK_BOOTTIME.
final class MotionHost: Sendable {
    static let shared = MotionHost()

    private let feeds = SensorFeeds()
    private let serial = DispatchQueue(label: "io.github.kisame76.minato.sensors", qos: .userInteractive)
    /// CoreMotion's handlers, on `serial`.
    private let queue = OperationQueue()
    /// An apply waits on `serial`.
    private let applyPending = Atomic(false)
    /// Worked on `serial` only, but for the availability that register() reads, which does not change.
    nonisolated(unsafe) private let manager = CMMotionManager()
    nonisolated(unsafe) private var running = MotionPlan()
    nonisolated(unsafe) private var game = GameRotation()
    nonisolated(unsafe) private var gyroBias = BiasEstimate()
    nonisolated(unsafe) private var fieldBias = BiasEstimate()

    private init() {
        queue.underlyingQueue = serial
        queue.maxConcurrentOperationCount = 1
    }

    /// Tells the hub which sensors this device's CoreMotion feeds and that this host serves them; once, before the
    /// guest runs. In the Simulator CoreMotion has none, and the hub offers none, as without a host.
    func register() {
        let available = SensorTypes.available(
            accelerometer: manager.isAccelerometerAvailable, gyro: manager.isGyroAvailable,
            magnetometer: manager.isMagnetometerAvailable, deviceMotion: manager.isDeviceMotionAvailable,
            magneticFrame: CMMotionManager.availableAttitudeReferenceFrames().contains(.xMagneticNorthZVertical))
        var host = apkrt_sensor_host()
        host.context = Unmanaged.passUnretained(self).toOpaque()
        host.start = { context, type, period in
            guard let context else { return -EINVAL }
            Unmanaged<MotionHost>.fromOpaque(context).takeUnretainedValue().start(type, period: period)
            return 0
        }
        host.stop = { context, type in
            guard let context else { return }
            Unmanaged<MotionHost>.fromOpaque(context).takeUnretainedValue().stop(type)
        }
        available.withUnsafeBufferPointer { apkrt_sensor_set_host(&host, $0.baseAddress, $0.count) }
        log.info("sensors: \(available.map(String.init).joined(separator: " "), privacy: .public)")
    }

    /// The session went to the background or came back (decision 3).
    func setBackground(_ on: Bool) {
        feeds.setBackground(on)
        follow()
    }

    private func start(_ type: Int32, period: Int64) {
        feeds.start(type, period: period)
        follow()
    }

    private func stop(_ type: Int32) {
        feeds.stop(type)
        follow()
    }

    /// Brings CoreMotion to the feeds' plan, on the host's queue; one apply waits at most, as it reads the plan when it
    /// runs.
    private func follow() {
        guard !applyPending.exchange(true, ordering: .acquiringAndReleasing) else { return }
        serial.async { [self] in
            applyPending.store(false, ordering: .releasing)
            apply(feeds.plan)
        }
    }

    private func apply(_ plan: MotionPlan) {
        if plan.accelerometer != running.accelerometer {
            if let interval = plan.accelerometer {
                manager.accelerometerUpdateInterval = interval
                if !manager.isAccelerometerActive {
                    manager.startAccelerometerUpdates(to: queue) { [self] data, _ in
                        guard let data else { return }
                        let g = data.acceleration
                        push(SensorTypes.accelerometer,
                             SensorValues.acceleration(Vector3(x: g.x, y: g.y, z: g.z)), data.timestamp)
                    }
                }
            } else {
                manager.stopAccelerometerUpdates()
            }
        }
        if plan.gyro != running.gyro {
            if let interval = plan.gyro {
                manager.gyroUpdateInterval = interval
                if !manager.isGyroActive {
                    manager.startGyroUpdates(to: queue) { [self] data, _ in
                        guard let data else { return }
                        let raw = data.rotationRate
                        let reading = Vector3(x: raw.x, y: raw.y, z: raw.z)
                        let bias = gyroBias.update(raw: reading, calibrated: feeds.calibrated.rate)
                        push(SensorTypes.gyroscopeUncalibrated, SensorValues.uncalibrated(raw: reading, bias: bias),
                             data.timestamp)
                    }
                }
            } else {
                manager.stopGyroUpdates()
            }
        }
        if plan.magnetometer != running.magnetometer {
            if let interval = plan.magnetometer {
                manager.magnetometerUpdateInterval = interval
                if !manager.isMagnetometerActive {
                    manager.startMagnetometerUpdates(to: queue) { [self] data, _ in
                        guard let data else { return }
                        let raw = data.magneticField
                        let calibrated = feeds.calibrated
                        let reading = Vector3(x: raw.x, y: raw.y, z: raw.z)
                        let bias = fieldBias.update(raw: reading, calibrated: calibrated.field)
                        push(SensorTypes.magneticFieldUncalibrated, SensorValues.uncalibrated(raw: reading, bias: bias),
                             data.timestamp, accuracy: SensorValues.accuracy(calibrated.accuracy))
                    }
                }
            } else {
                manager.stopMagnetometerUpdates()
            }
        }
        if plan.deviceMotion != running.deviceMotion || plan.frame != running.frame {
            if let interval = plan.deviceMotion {
                manager.deviceMotionUpdateInterval = interval
                // Another frame needs a new start; a new interval takes effect as it is.
                if manager.isDeviceMotionActive, plan.frame != running.frame { manager.stopDeviceMotionUpdates() }
                if !manager.isDeviceMotionActive { startDeviceMotion(plan.frame) }
            } else {
                manager.stopDeviceMotionUpdates()
            }
        }
        running = plan
    }

    private func startDeviceMotion(_ frame: MotionPlan.Frame) {
        let north = frame == .magneticNorth
        manager.startDeviceMotionUpdates(using: north ? .xMagneticNorthZVertical : .xArbitraryZVertical,
                                         to: queue) { [self] motion, _ in
            guard let motion else { return }
            let q = motion.attitude.quaternion
            // The calibrated field is only there with a magnetic reference frame.
            let sample = DeviceMotionSample(
                rate: Vector3(x: motion.rotationRate.x, y: motion.rotationRate.y, z: motion.rotationRate.z),
                gravity: Vector3(x: motion.gravity.x, y: motion.gravity.y, z: motion.gravity.z),
                user: Vector3(x: motion.userAcceleration.x, y: motion.userAcceleration.y, z: motion.userAcceleration.z),
                attitude: Quaternion(x: q.x, y: q.y, z: q.z, w: q.w),
                field: north ? Vector3(x: motion.magneticField.field.x, y: motion.magneticField.field.y,
                                       z: motion.magneticField.field.z) : nil,
                accuracy: north ? Int(motion.magneticField.accuracy.rawValue) : -1)
            feeds.remember(rate: sample.rate, field: sample.field, accuracy: sample.accuracy)
            let attitude = game.next(sample.attitude, frame: frame)
            for out in SensorValues.samples(of: sample, game: attitude) {
                push(out.type, out.values, motion.timestamp, accuracy: out.accuracy)
            }
        }
    }

    /// One sample to the hub, while its type is started and the session in the foreground.
    private func push(_ type: Int32, _ values: [Float], _ time: TimeInterval, accuracy: Int32 = 3) {
        feeds.deliver(type) {
            values.withUnsafeBufferPointer { apkrt_sensor_push(type, $0.baseAddress, $0.count, time, accuracy) }
        }
    }
}
