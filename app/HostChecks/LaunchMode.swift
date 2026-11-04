/// tools/session-window-check.sh's stand-in for LaunchRequest.swift's LaunchMode, whose file needs APKKit: the check's
/// APKSession takes InterfaceOrientations.swift, which extends it.
public enum LaunchMode: Equatable, Sendable {
    case library
    case gameMode
    case compileMode
}
