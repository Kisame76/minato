// The written part of TileService (services batch C): the quick settings tile of an app. The system's status bar
// binds a tile service and lends it its tile; Minato has none, so nothing binds one and a service that is not bound
// has no tile and no connection to the status bar, which is what AOSP's answers on an unbound instance are: no tile,
// and a NullPointerException for the lock state [verified: services-c-test]. The callbacks do nothing, as AOSP's.
class TileService {
    public TileService() {
    }

    public final Tile getQsTile() {
        return null;
    }

    public final boolean isLocked() {
        throw new NullPointerException("Attempt to invoke interface method 'boolean isLocked()' on a null object "
                + "reference: the tile service is not bound to the status bar");
    }

    public final boolean isSecure() {
        throw new NullPointerException("Attempt to invoke interface method 'boolean isSecure()' on a null object "
                + "reference: the tile service is not bound to the status bar");
    }

    public void onTileAdded() {
    }

    public void onTileRemoved() {
    }

    public void onStartListening() {
    }

    public void onStopListening() {
    }

    public void onClick() {
    }
}
