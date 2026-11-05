// The written part of BluetoothSocket (services batch E): a socket to a remote device, which cannot connect while the
// adapter is off (Minato's is, and does not offer the iPhone's Bluetooth to guests). An app that connects gets the
// IOException that Android gives for a stack that is down.
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

class BluetoothSocket {
    private final BluetoothDevice mDevice;

    /** @hide */
    BluetoothSocket(BluetoothDevice device) {
        mDevice = device;
    }

    public void connect() throws IOException {
        BluetoothDevice.checkConnect("connect");
        throw new IOException("Bluetooth is off");
    }

    public void close() throws IOException {
    }

    public boolean isConnected() {
        return false;
    }

    public BluetoothDevice getRemoteDevice() {
        return mDevice;
    }

    public InputStream getInputStream() throws IOException {
        throw new IOException("Bluetooth is off");
    }

    public OutputStream getOutputStream() throws IOException {
        throw new IOException("Bluetooth is off");
    }

    public int getConnectionType() {
        return TYPE_RFCOMM;
    }

    public int getMaxReceivePacketSize() {
        return 0;
    }

    public int getMaxTransmitPacketSize() {
        return 0;
    }
}
