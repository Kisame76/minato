// The written part of the placeholder SharedMemory (framework breadth, batch H1): the TV ad buffer duplicates the memory's
// descriptor (hidden API); the placeholder has no memory, so the call is unsupported like the rest of its methods.
class SharedMemory {
    /** @hide */
    public ParcelFileDescriptor getFdDup() throws java.io.IOException {
        throw new UnsupportedOperationException("SharedMemory has no descriptor here");
    }
}
