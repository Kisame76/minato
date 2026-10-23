import Synchronization

/// ADR-0061: what the iPhone's CoreMotion gives, in Android's sensor units, signs and frames (decision 2), and which
/// CoreMotion feeds run for the sensors the guest enabled (decision 3). Pure, so that the Mac tests it; the app's
/// MotionHost reads CoreMotion and pushes the values into the runtime's hub.

/// A CoreMotion vector (CMAcceleration, CMRotationRate, CMMagneticField), in the device's portrait frame, which is
/// Android's too: x to the right, y to the top, z out of the screen.
public struct Vector3: Sendable, Equatable {
    public var x, y, z: Double

    public init(x: Double, y: Double, z: Double) {
        self.x = x
        self.y = y
        self.z = z
    }
}

/// A CoreMotion attitude (CMQuaternion): the device's rotation in its reference frame.
public struct Quaternion: Sendable, Equatable {
    public var x, y, z, w: Double

    public init(x: Double, y: Double, z: Double, w: Double) {
        self.x = x
        self.y = y
        self.z = z
        self.w = w
    }
}

/// Android's SENSOR_TYPE numbers the host can serve (ADR-0061 decision 1).
public enum SensorTypes {
    public static let accelerometer: Int32 = 1
    public static let magneticField: Int32 = 2
    public static let gyroscope: Int32 = 4
    public static let gravity: Int32 = 9
    public static let linearAcceleration: Int32 = 10
    public static let rotationVector: Int32 = 11
    public static let magneticFieldUncalibrated: Int32 = 14
    public static let gameRotationVector: Int32 = 15
    public static let gyroscopeUncalibrated: Int32 = 16

    /// The types this device's CoreMotion feeds, ascending: the accelerometer's own, the fused device motion's
    /// (the magnetic ones only with its magnetic-north frame), and the raw gyroscope's and magnetometer's with the
    /// device motion's calibrated values for their bias.
    public static func available(accelerometer: Bool, gyro: Bool, magnetometer: Bool, deviceMotion: Bool,
                                 magneticFrame: Bool) -> [Int32] {
        var types: [Int32] = []
        if accelerometer { types.append(Self.accelerometer) }
        if deviceMotion && magneticFrame { types.append(magneticField) }
        if deviceMotion { types += [gyroscope, gravity, linearAcceleration] }
        if deviceMotion && magneticFrame { types.append(rotationVector) }
        if deviceMotion && magneticFrame && magnetometer { types.append(magneticFieldUncalibrated) }
        if deviceMotion { types.append(gameRotationVector) }
        if deviceMotion && gyro { types.append(gyroscopeUncalibrated) }
        return types
    }
}

public enum SensorValues {
    /// Android's SensorManager.STANDARD_GRAVITY, m/s² in one g.
    public static let standardGravity = 9.80665

    /// An acceleration in g (accelerometer, gravity, user acceleration) in m/s², with Android's sign: iOS reports
    /// -1 g on z for a phone that lies face up, Android +9.81 (decision 2).
    public static func acceleration(_ g: Vector3) -> [Float] {
        [Float(-g.x * standardGravity), Float(-g.y * standardGravity), Float(-g.z * standardGravity)]
    }

    /// A rotation rate in rad/s, as it is.
    public static func rate(_ rate: Vector3) -> [Float] { [Float(rate.x), Float(rate.y), Float(rate.z)] }

    /// A magnetic field in µT, as it is.
    public static func field(_ field: Vector3) -> [Float] { [Float(field.x), Float(field.y), Float(field.z)] }

    /// An uncalibrated type's six values: the raw reading and its bias (BiasEstimate).
    public static func uncalibrated(raw: Vector3, bias: Vector3) -> [Float] {
        [Float(raw.x), Float(raw.y), Float(raw.z), Float(bias.x), Float(bias.y), Float(bias.z)]
    }

    /// TYPE_GAME_ROTATION_VECTOR: x, y, z, w of the attitude; its yaw has no fixed origin on either platform.
    public static func gameRotation(_ q: Quaternion) -> [Float] {
        let q = positive(q)
        return [Float(q.x), Float(q.y), Float(q.z), Float(q.w)]
    }

    /// TYPE_ROTATION_VECTOR from the attitude in iOS's magnetic-north frame: north-west-up turned to Android's
    /// east-north-up (a quarter turn about up, before the attitude), and -1 for the heading accuracy, which
    /// CoreMotion does not give in radians (Android's value when it is unknown).
    public static func rotation(_ attitude: Quaternion) -> [Float] {
        let half = 0.5.squareRoot()
        let q = positive(multiply(Quaternion(x: 0, y: 0, z: half, w: half), attitude))
        return [Float(q.x), Float(q.y), Float(q.z), Float(q.w), -1]
    }

    /// The same rotation with w ≥ 0, as Android gives it: SensorManager rebuilds w as the positive root when an app
    /// passes the first three values only.
    static func positive(_ q: Quaternion) -> Quaternion {
        q.w < 0 ? Quaternion(x: -q.x, y: -q.y, z: -q.z, w: -q.w) : q
    }

    /// The fused types of one device motion sample: the gyroscope, gravity, linear acceleration and the game rotation
    /// vector (`game`, GameRotation's), and with the magnetic-north frame's field the magnetic field and the rotation
    /// vector, with the field's accuracy.
    public static func samples(of motion: DeviceMotionSample, game: Quaternion) -> [SensorSample] {
        var samples = [
            SensorSample(type: SensorTypes.gyroscope, values: rate(motion.rate)),
            SensorSample(type: SensorTypes.gravity, values: acceleration(motion.gravity)),
            SensorSample(type: SensorTypes.linearAcceleration, values: acceleration(motion.user)),
            SensorSample(type: SensorTypes.gameRotationVector, values: gameRotation(game)),
        ]
        if let field = motion.field {
            let status = accuracy(motion.accuracy)
            samples.append(SensorSample(type: SensorTypes.magneticField, values: Self.field(field), accuracy: status))
            samples.append(SensorSample(type: SensorTypes.rotationVector, values: rotation(motion.attitude),
                                        accuracy: status))
        }
        return samples
    }

    /// The Hamilton product a ⊗ b: the rotation b, then a.
    static func multiply(_ a: Quaternion, _ b: Quaternion) -> Quaternion {
        Quaternion(x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                   y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                   z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                   w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z)
    }

    /// CoreMotion's CMMagneticFieldCalibrationAccuracy (uncalibrated -1, low 0, medium 1, high 2) as Android's
    /// SENSOR_STATUS (unreliable 0, low 1, medium 2, high 3).
    public static func accuracy(_ calibration: Int) -> Int32 { Int32(min(max(calibration + 1, 0), 3)) }
}

/// One CMDeviceMotion sample's values; `field` and `accuracy` only with the magnetic-north frame.
public struct DeviceMotionSample: Sendable, Equatable {
    public var rate, gravity, user: Vector3
    public var attitude: Quaternion
    public var field: Vector3?
    public var accuracy: Int

    public init(rate: Vector3, gravity: Vector3, user: Vector3, attitude: Quaternion, field: Vector3?, accuracy: Int) {
        self.rate = rate
        self.gravity = gravity
        self.user = user
        self.attitude = attitude
        self.field = field
        self.accuracy = accuracy
    }
}

/// One sample for the hub: its type, its values and its SENSOR_STATUS.
public struct SensorSample: Sendable, Equatable {
    public var type: Int32
    public var values: [Float]
    public var accuracy: Int32 = 3
}

/// The game rotation vector's attitude, kept going when the device motion's frame changes (a magnetic type enabled or
/// dropped restarts it with another yaw origin): Android's drifts slowly but never jumps, so the new frame's attitude
/// is turned about up to go on from the last value.
public struct GameRotation: Sendable {
    private var offset = Quaternion(x: 0, y: 0, z: 0, w: 1)
    private var last: Quaternion?
    private var frame: MotionPlan.Frame?

    public init() {}

    public mutating func next(_ attitude: Quaternion, frame: MotionPlan.Frame) -> Quaternion {
        if let last, let earlier = self.frame, earlier != frame {
            // The yaw between the last value and the same pose in the new frame; their tilt is the same.
            let between = SensorValues.multiply(last, Quaternion(x: -attitude.x, y: -attitude.y, z: -attitude.z,
                                                                 w: attitude.w))
            let norm = (between.z * between.z + between.w * between.w).squareRoot()
            offset = norm > 1e-9 ? Quaternion(x: 0, y: 0, z: between.z / norm, w: between.w / norm)
                : Quaternion(x: 0, y: 0, z: 0, w: 1)
        }
        self.frame = frame
        let value = SensorValues.multiply(offset, attitude)
        last = value
        return value
    }
}

/// An uncalibrated type's bias: a slow average of raw minus calibrated, as Android's is a drift estimate, rather than
/// the difference of two samples taken at different times, which swings with every turn.
public struct BiasEstimate: Sendable {
    private var estimate: Vector3?
    /// The weight of each new difference.
    static let weight = 0.02

    public init() {}

    public mutating func update(raw: Vector3, calibrated: Vector3?) -> Vector3 {
        guard let calibrated else { return estimate ?? Vector3(x: 0, y: 0, z: 0) }
        let difference = Vector3(x: raw.x - calibrated.x, y: raw.y - calibrated.y, z: raw.z - calibrated.z)
        guard let old = estimate else {
            estimate = difference
            return difference
        }
        let new = Vector3(x: old.x + Self.weight * (difference.x - old.x),
                          y: old.y + Self.weight * (difference.y - old.y),
                          z: old.z + Self.weight * (difference.z - old.z))
        estimate = new
        return new
    }
}

/// The CoreMotion feeds for the sensors the guest enabled, each at the fastest period wanted of it (the hub sends
/// every sample to every queue, as Android does): the accelerometer's own feed; the fused device motion for the
/// gyroscope, gravity, linear acceleration, both rotation vectors and the calibrated field, and for the calibrated
/// values the uncalibrated types' bias needs; the raw gyroscope and magnetometer for those. The device motion uses the
/// magnetic-north frame while a magnetic type is wanted, else the arbitrary one, which needs no magnetometer. Nothing
/// runs in the background (decision 3).
public struct MotionPlan: Sendable, Equatable {
    public enum Frame: Sendable { case arbitrary, magneticNorth }

    /// Update intervals in seconds, nil for a feed that does not run.
    public var accelerometer: Double?
    public var gyro: Double?
    public var magnetometer: Double?
    public var deviceMotion: Double?
    public var frame = Frame.arbitrary

    public init() {}

    /// `wanted`: each enabled type's period in nanoseconds, as the hub's start gives it.
    public init(wanted: [Int32: Int64], inBackground: Bool = false) {
        guard !inBackground else { return }
        func fastest(_ current: Double?, _ period: Int64) -> Double {
            min(current ?? .infinity, Double(period) / 1e9)
        }
        for (type, period) in wanted {
            switch type {
            case SensorTypes.accelerometer:
                accelerometer = fastest(accelerometer, period)
            case SensorTypes.gyroscope, SensorTypes.gravity, SensorTypes.linearAcceleration,
                 SensorTypes.gameRotationVector:
                deviceMotion = fastest(deviceMotion, period)
            case SensorTypes.magneticField, SensorTypes.rotationVector:
                deviceMotion = fastest(deviceMotion, period)
                frame = .magneticNorth
            case SensorTypes.magneticFieldUncalibrated:
                magnetometer = fastest(magnetometer, period)
                deviceMotion = fastest(deviceMotion, period)
                frame = .magneticNorth
            case SensorTypes.gyroscopeUncalibrated:
                gyro = fastest(gyro, period)
                deviceMotion = fastest(deviceMotion, period)
            default:
                break
            }
        }
    }
}

/// The host's record of the sensors the hub started (apkrt_sensor_host): start and stop run on the thread that enabled
/// or disabled the sensor, a guest thread or a runtime sweep under its lock, so they only record; CoreMotion follows
/// on the host's own queue from `plan`. A push runs under the same lock and only while its type is started and the
/// session in the foreground, so that none follows a stop that returned (apkrt.h). The hub calls start and stop with
/// no lock of its own that a push takes, so a stop that waits for a push in flight cannot deadlock.
public final class SensorFeeds: Sendable {
    public struct Calibrated: Sendable {
        /// The device motion's last rotation rate and magnetic field, for the uncalibrated types' bias.
        public var rate: Vector3?
        public var field: Vector3?
        /// Its field's CMMagneticFieldCalibrationAccuracy, -1 (uncalibrated) before the first sample.
        public var accuracy = -1
    }

    private struct State {
        var wanted: [Int32: Int64] = [:]
        var inBackground = false
        var calibrated = Calibrated()
    }

    private let state = Mutex(State())

    public init() {}

    /// The hub begins or re-rates `type`.
    public func start(_ type: Int32, period: Int64) { state.withLock { $0.wanted[type] = period } }

    /// The hub stops `type`; harmless for one that was not started.
    public func stop(_ type: Int32) { state.withLock { _ = $0.wanted.removeValue(forKey: type) } }

    /// The session went to the background or came back: nothing runs meanwhile (decision 3), the wanted types stay.
    public func setBackground(_ on: Bool) { state.withLock { $0.inBackground = on } }

    /// The feeds CoreMotion should run now.
    public var plan: MotionPlan { state.withLock { MotionPlan(wanted: $0.wanted, inBackground: $0.inBackground) } }

    /// Runs `push` for a sample of `type` while the type is started and the session in the foreground.
    public func deliver(_ type: Int32, _ push: () -> Void) {
        state.withLock { state in
            if state.wanted[type] != nil, !state.inBackground { push() }
        }
    }

    public func remember(rate: Vector3?, field: Vector3?, accuracy: Int) {
        state.withLock { $0.calibrated = Calibrated(rate: rate, field: field, accuracy: accuracy) }
    }

    public var calibrated: Calibrated { state.withLock { $0.calibrated } }
}
