import APKKit
import Foundation

extension Container {
    /// [guess] Decision 7's caps for bundletool's table of contents.
    public static let maxTOCSize: UInt64 = 16 << 20

    /// The table of contents of bundletool's `.apks`: `toc.pb`, or `toc.json`, never both (`ResultUtils.java:
    /// 111-125`), each within its cap.
    public func bundletoolToc() throws -> BundletoolToc {
        let proto = archive.entry(named: "toc.pb"), json = archive.entry(named: "toc.json")
        if proto != nil, json != nil { throw ContainerError.invalidTOC("it holds both toc.pb and toc.json") }
        guard let entry = proto ?? json else { throw ContainerError.invalidTOC("it has no table of contents") }
        let cap = proto != nil ? Self.maxTOCSize : Self.maxJSONSize
        guard entry.uncompressedSize <= cap else {
            throw ContainerError.overCap("its \(entry.name) has \(entry.uncompressedSize) bytes, more than the \(cap) Minato reads")
        }
        guard !entry.isEncrypted else { throw ContainerError.encrypted(entry.name) }
        let data: Data
        do {
            data = try archive.data(for: entry, maxSize: cap)
        } catch let error as APKError {
            throw ContainerError.unreadable(error)
        }
        do {
            return proto != nil ? try BundletoolToc(proto: data) : try BundletoolToc(json: data)
        } catch ProtoError.truncated(let what) {
            throw ContainerError.invalidTOC("its toc.pb is cut short in \(what)")
        } catch ProtoError.invalid(let what) {
            throw ContainerError.invalidTOC(proto != nil ? "its toc.pb holds \(what)" : "its \(what)")
        }
    }

    /// bundletool's choice for `device`, made from the table of contents before anything is extracted (decision 2),
    /// and its APKs under names of ours, `1.apk` on, never an entry's path. The packs it names come through
    /// `bundletoolPackItems`.
    public func bundletoolItems(for device: BundletoolDevice) throws -> (items: [Item], choice: BundletoolChoice) {
        let choice = BundletoolSelector.choose(try bundletoolToc(), for: device)
        if let reason = choice.incompatibility { throw ContainerError.incompatible(reason) }
        guard choice.apks.count <= Self.maxAPKs else {
            throw ContainerError.overCap("it holds \(choice.apks.count) APKs for this iPhone, more than the \(Self.maxAPKs) Minato takes from a container")
        }
        let items = try choice.apks.enumerated().map { index, apk in
            guard let entry = archive.entry(named: apk.path) else {
                throw ContainerError.invalidTOC("its table of contents names \(apk.path), which it lacks")
            }
            return Item(entry: entry, fileName: "\(index + 1).apk")
        }
        return (items, choice)
    }

    /// One slice of a pack Play fetches after the install, fast-follow or on-demand (format 6): its pack, its path in
    /// the container and the item that takes it out.
    public struct PackItem: Sendable, Equatable {
        public var pack: String
        public var path: String
        public var item: Item
    }

    /// The slices of the fast-follow and on-demand packs `choice` names, under names of ours, `p1.apk` on, never an
    /// entry's path (ADR-0049 decision 2); the library names each after its split once it has read it. They count
    /// toward the cap with the choice's APKs.
    public func bundletoolPackItems(of choice: BundletoolChoice) throws -> [PackItem] {
        let slices = choice.packs.flatMap { pack in pack.paths.map { (pack: pack.name, path: $0) } }
        guard choice.apks.count + slices.count <= Self.maxAPKs else {
            throw ContainerError.overCap("it holds \(choice.apks.count + slices.count) APKs and pack slices for this "
                                         + "iPhone, more than the \(Self.maxAPKs) Minato takes from a container")
        }
        return try slices.enumerated().map { index, slice in
            guard let entry = archive.entry(named: slice.path) else {
                throw ContainerError.invalidTOC("its table of contents names \(slice.path), which it lacks")
            }
            return PackItem(pack: slice.pack, path: slice.path, item: Item(entry: entry, fileName: "p\(index + 1).apk"))
        }
    }
}
