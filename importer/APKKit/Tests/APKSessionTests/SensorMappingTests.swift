import Foundation
import Synchronization
import Testing

@testable import APKSession

/// ADR-0061 decision 2: CoreMotion's values in Android's units, signs and frames, and decision 3: which CoreMotion
/// feeds run for the sensors the guest has enabled.
@Suite("SensorMapping")
struct SensorMappingTests {
    static let g = 9.80665
    static let half = 0.5.squareRoot()

    func close(_ a: [Float], _ b: [Double], within: Double = 1e-5) -> Bool {
        a.count == b.count && zip(a, b).allSatisfy { abs(Double($0) - $1) <= within }
    }

    /// iOS reports -1 g on z for a phone that lies face up; Android reports +9.81 m/s² up.
    @Test func accelerationIsInMetresPerSecondSquaredWithAndroidsSign() {
        #expect(close(SensorValues.acceleration(Vector3(x: 0, y: 0, z: -1)), [0, 0, Self.g]))
        #expect(close(SensorValues.acceleration(Vector3(x: 0.5, y: -1, z: 0)), [-0.5 * Self.g, Self.g, 0]))
    }

    @Test func rotationRateAndFieldPassAsTheyAre() {
        #expect(close(SensorValues.rate(Vector3(x: 0.1, y: -0.2, z: 3)), [0.1, -0.2, 3]))
        #expect(close(SensorValues.field(Vector3(x: 20, y: -5, z: -40)), [20, -5, -40]))
    }

    @Test func anUncalibratedValueCarriesItsBias() {
        var bias = BiasEstimate()
        let raw = Vector3(x: 1, y: 2, z: 3)
        #expect(close(SensorValues.uncalibrated(raw: raw, bias: bias.update(raw: raw, calibrated: nil)),
                      [1, 2, 3, 0, 0, 0]))
        let first = bias.update(raw: raw, calibrated: Vector3(x: 0.5, y: 2, z: 4))
        #expect(close(SensorValues.uncalibrated(raw: raw, bias: first), [1, 2, 3, 0.5, 0, -1]))  // raw - calibrated
    }

    @Test func theGameRotationVectorIsTheAttitude() {
        #expect(close(SensorValues.gameRotation(Quaternion(x: 0.1, y: 0.2, z: 0.3, w: 0.9)), [0.1, 0.2, 0.3, 0.9]))
    }

    /// iOS's magnetic frame is north-west-up, Android's east-north-up: a phone face up with its top to the north is
    /// Android's identity, and at iOS's identity its right edge points north.
    @Test func theRotationVectorTurnsNorthWestUpToEastNorthUp() {
        let topNorth = Quaternion(x: 0, y: 0, z: -Self.half, w: Self.half)
        #expect(close(SensorValues.rotation(topNorth), [0, 0, 0, 1, -1]))
        #expect(close(SensorValues.rotation(Quaternion(x: 0, y: 0, z: 0, w: 1)), [0, 0, Self.half, Self.half, -1]))
        // Upright, top to the sky, screen to the south (the camera to the north): Android's x east, y up, z south.
        let upright = Quaternion(x: Self.half, y: 0, z: 0, w: Self.half)  // about the device's x, from face up
        let turned = SensorValues.rotation(SensorValues.multiply(Quaternion(x: 0, y: 0, z: -Self.half, w: Self.half),
                                                                 upright))
        #expect(close(turned, [Self.half, 0, 0, Self.half, -1]))
    }

    /// Android keeps w ≥ 0 (getRotationMatrixFromVector rebuilds w as the positive root from three values).
    @Test func aRotationVectorKeepsItsWPositive() {
        let yaw170 = Quaternion(x: 0, y: 0, z: sin(170 * .pi / 360), w: cos(170 * .pi / 360))
        let turned = SensorValues.rotation(yaw170)
        #expect(turned[3] >= 0)
        #expect(close(Array(turned.prefix(4)), [0, 0, -sin(100 * .pi / 360), cos(100 * .pi / 360)]))  // 260° = -100°
        let game = SensorValues.gameRotation(Quaternion(x: 0.1, y: 0.2, z: 0.3, w: -0.9))
        #expect(close(game, [-0.1, -0.2, -0.3, 0.9]))
    }

    /// One device motion sample gives each fused type its own value, the magnetic ones only with the north frame.
    @Test func aDeviceMotionSampleFeedsEachType() {
        let sample = DeviceMotionSample(rate: Vector3(x: 1, y: 2, z: 3), gravity: Vector3(x: 0, y: 0, z: -1),
                                        user: Vector3(x: 0.5, y: 0, z: 0),
                                        attitude: Quaternion(x: 0, y: 0, z: 0, w: 1), field: nil, accuracy: -1)
        let game = Quaternion(x: 0, y: 0, z: 0.6, w: 0.8)
        let out = Dictionary(uniqueKeysWithValues: SensorValues.samples(of: sample, game: game).map { ($0.type, $0) })
        #expect(out.keys.sorted() == [4, 9, 10, 15])
        #expect(close(out[4]!.values, [1, 2, 3]))
        #expect(close(out[9]!.values, [0, 0, Self.g]))
        #expect(close(out[10]!.values, [-0.5 * Self.g, 0, 0]))
        #expect(close(out[15]!.values, [0, 0, 0.6, 0.8]))
        #expect(out.values.allSatisfy { $0.accuracy == 3 })
        var north = sample
        north.field = Vector3(x: 20, y: 0, z: -40)
        north.accuracy = 1
        let all = Dictionary(uniqueKeysWithValues: SensorValues.samples(of: north, game: game).map { ($0.type, $0) })
        #expect(all.keys.sorted() == [2, 4, 9, 10, 11, 15])
        #expect(close(all[2]!.values, [20, 0, -40]))
        #expect(all[2]!.accuracy == 2)
        #expect(close(all[11]!.values, [0, 0, Self.half, Self.half, -1]))
    }

    /// The game rotation vector goes on from its last value when the device motion's frame changes, as Android's
    /// drifts slowly but never jumps.
    @Test func theGameRotationVectorKeepsItsYawAcrossAFrameChange() {
        var game = GameRotation()
        let tilt = Quaternion(x: 0.2, y: 0, z: 0, w: 0.98)
        let first = game.next(tilt, frame: .arbitrary)
        #expect(close(SensorValues.gameRotation(first), SensorValues.gameRotation(tilt).map(Double.init)))
        // The same pose in the north frame comes with another yaw: the output stays.
        let yawed = SensorValues.multiply(Quaternion(x: 0, y: 0, z: sin(0.5), w: cos(0.5)), tilt)
        let after = game.next(yawed, frame: .magneticNorth)
        #expect(close(SensorValues.gameRotation(after), SensorValues.gameRotation(first).map(Double.init),
                      within: 1e-4))
        // and follows the motion from there.
        let turned = SensorValues.multiply(Quaternion(x: 0, y: 0, z: sin(0.1), w: cos(0.1)), yawed)
        let moved = game.next(turned, frame: .magneticNorth)
        let expected = SensorValues.multiply(Quaternion(x: 0, y: 0, z: sin(0.1), w: cos(0.1)), first)
        #expect(close(SensorValues.gameRotation(moved), SensorValues.gameRotation(expected).map(Double.init),
                      within: 1e-4))
    }

    /// The uncalibrated types' bias is an average of raw minus calibrated, not one sample's difference.
    @Test func theBiasIsSmoothed() {
        var bias = BiasEstimate()
        #expect(bias.update(raw: Vector3(x: 1, y: 1, z: 1), calibrated: nil) == Vector3(x: 0, y: 0, z: 0))
        let first = bias.update(raw: Vector3(x: 1.1, y: 1, z: 1), calibrated: Vector3(x: 1, y: 1, z: 1))
        #expect(abs(first.x - 0.1) < 1e-9 && first.y == 0 && first.z == 0)  // the first difference as it is
        var settled = BiasEstimate()
        var last = Vector3(x: 0, y: 0, z: 0)
        for _ in 0..<500 {
            last = settled.update(raw: Vector3(x: 1.1, y: 2, z: 3), calibrated: Vector3(x: 1, y: 2, z: 3))
        }
        #expect(abs(last.x - 0.1) < 1e-3 && abs(last.y) < 1e-9)
        let spike = settled.update(raw: Vector3(x: 11.1, y: 2, z: 3), calibrated: Vector3(x: 1, y: 2, z: 3))
        #expect(spike.x < 0.5)  // one sample's difference of 10.1 moves it little
    }

    @Test func calibrationAccuracyBecomesAndroidsStatus() {
        #expect(SensorValues.accuracy(-1) == 0)
        #expect(SensorValues.accuracy(0) == 1)
        #expect(SensorValues.accuracy(1) == 2)
        #expect(SensorValues.accuracy(2) == 3)
        #expect(SensorValues.accuracy(7) == 3)
    }

    @Test func whatThisDeviceOffers() {
        #expect(SensorTypes.available(accelerometer: true, gyro: true, magnetometer: true, deviceMotion: true,
                                      magneticFrame: true) == [1, 2, 4, 9, 10, 11, 14, 15, 16])
        #expect(SensorTypes.available(accelerometer: true, gyro: false, magnetometer: false, deviceMotion: false,
                                      magneticFrame: false) == [1])
        #expect(SensorTypes.available(accelerometer: true, gyro: true, magnetometer: true, deviceMotion: true,
                                      magneticFrame: false) == [1, 4, 9, 10, 15, 16])
        #expect(SensorTypes.available(accelerometer: true, gyro: false, magnetometer: false, deviceMotion: true,
                                      magneticFrame: true) == [1, 2, 4, 9, 10, 11, 15])
    }

    @Test func eachSensorRunsItsFeedAtItsPeriod() {
        let ms = Int64(1_000_000)
        #expect(MotionPlan(wanted: [:]) == MotionPlan())
        var accelerometer = MotionPlan()
        accelerometer.accelerometer = 0.02
        #expect(MotionPlan(wanted: [1: 20 * ms]) == accelerometer)
        var motion = MotionPlan()
        motion.deviceMotion = 0.01
        #expect(MotionPlan(wanted: [4: 10 * ms, 9: 50 * ms, 10: 20 * ms, 15: 1000 * ms]) == motion)  // the fastest
        var north = MotionPlan()
        north.deviceMotion = 0.05
        north.frame = .magneticNorth
        #expect(MotionPlan(wanted: [11: 50 * ms]) == north)
        #expect(MotionPlan(wanted: [2: 50 * ms]) == north)
        var raw = MotionPlan()
        raw.gyro = 0.01
        raw.magnetometer = 0.02
        raw.deviceMotion = 0.01  // the calibrated values for the biases
        raw.frame = .magneticNorth
        #expect(MotionPlan(wanted: [16: 10 * ms, 14: 20 * ms]) == raw)
        var field = MotionPlan()
        field.magnetometer = 0.02
        field.deviceMotion = 0.02
        field.frame = .magneticNorth
        #expect(MotionPlan(wanted: [14: 20 * ms]) == field)
    }

    @Test func nothingRunsInTheBackground() {
        #expect(MotionPlan(wanted: [1: 10_000_000, 11: 10_000_000], inBackground: true) == MotionPlan())
    }

    @Test func aTypeTheHostDoesNotServeRunsNothing() {
        #expect(MotionPlan(wanted: [3: 10_000_000, 99: 10_000_000]) == MotionPlan())
    }
}

/// The host's record of the enabled sensors: start and stop only record, and no push follows a stop (apkrt.h).
@Suite("SensorFeeds")
struct SensorFeedsTests {
    @Test func aPushGoesOutOnlyWhileItsTypeIsStartedAndInTheForeground() {
        let feeds = SensorFeeds()
        var pushed = 0
        feeds.deliver(1) { pushed += 1 }
        #expect(pushed == 0)
        feeds.start(1, period: 20_000_000)
        feeds.deliver(1) { pushed += 1 }
        feeds.deliver(4) { pushed += 1 }
        #expect(pushed == 1)
        feeds.setBackground(true)
        feeds.deliver(1) { pushed += 1 }
        #expect(pushed == 1)
        #expect(feeds.plan == MotionPlan())
        feeds.setBackground(false)
        feeds.deliver(1) { pushed += 1 }
        #expect(pushed == 2)
        feeds.stop(1)
        feeds.stop(1)  // harmless
        feeds.deliver(1) { pushed += 1 }
        #expect(pushed == 2)
    }

    @Test func aSecondStartChangesTheRate() {
        let feeds = SensorFeeds()
        feeds.start(1, period: 20_000_000)
        feeds.start(1, period: 10_000_000)
        #expect(feeds.plan.accelerometer == 0.01)
        feeds.stop(1)
        #expect(feeds.plan == MotionPlan())
    }

    @Test func noPushFollowsAStopThatReturned() {
        let feeds = SensorFeeds()
        feeds.start(1, period: 10_000_000)
        let counter = Counter()
        let stopped = Counter()
        let finished = DispatchSemaphore(value: 0)
        Thread.detachNewThread {
            while stopped.value == 0 {
                feeds.deliver(1) {
                    counter.add()
                    sched_yield()
                }
            }
            for _ in 0..<1000 { feeds.deliver(1) { counter.add() } }
            finished.signal()
        }
        while counter.value < 100 { usleep(100) }
        feeds.stop(1)
        let atStop = counter.value
        stopped.add()
        finished.wait()
        #expect(atStop >= 100)
        #expect(counter.value == atStop)
    }

    /// A stop while a push is in flight returns only after it: the push runs under the record's lock.
    @Test func aStopWaitsForThePushInFlight() {
        let feeds = SensorFeeds()
        feeds.start(1, period: 10_000_000)
        let inside = DispatchSemaphore(value: 0)
        let finished = DispatchSemaphore(value: 0)
        let done = Counter()
        Thread.detachNewThread {
            feeds.deliver(1) {
                inside.signal()
                usleep(200_000)
                done.add()
            }
            finished.signal()
        }
        inside.wait()
        feeds.stop(1)
        #expect(done.value == 1)
        finished.wait()
    }

    @Test func theCalibratedValuesAreKeptForTheBiases() {
        let feeds = SensorFeeds()
        #expect(feeds.calibrated.rate == nil && feeds.calibrated.field == nil && feeds.calibrated.accuracy == -1)
        feeds.remember(rate: Vector3(x: 1, y: 2, z: 3), field: Vector3(x: 4, y: 5, z: 6), accuracy: 2)
        #expect(feeds.calibrated.rate == Vector3(x: 1, y: 2, z: 3))
        #expect(feeds.calibrated.field == Vector3(x: 4, y: 5, z: 6))
        #expect(feeds.calibrated.accuracy == 2)
    }

    final class Counter: Sendable {
        private let count = Mutex(0)
        func add() { count.withLock { $0 += 1 } }
        var value: Int { count.withLock { $0 } }
    }
}
