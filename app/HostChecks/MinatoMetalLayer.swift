import QuartzCore

/// tools/session-window-check.sh's stand-in for graphics/host's MinatoMetalLayer (graphics/host/layer.m), whose pacing
/// and recording the check does not reach: SessionWindow.swift only makes it and hands it on.
class MinatoMetalLayer: CAMetalLayer {}
