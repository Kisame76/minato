// The hand file of SampleAppManager, which managergen --implement completes from SampleBaseManager (ADR-0039 b3.5a):
// a hidden ctor, an abstract method and an override of a concrete one.
import com.android.minato.sample.pm.SampleInfo;

class SampleAppManager {
    private final String mName;

    /** @hide */
    public SampleAppManager(String name) {
        mName = name;
    }

    public SampleInfo getInfo(String name, int flags) throws NotFoundException {
        if (!name.equals(mName)) {
            throw new NotFoundException(name);
        }
        SampleInfo info = new SampleInfo();
        info.name = name;
        return info;
    }

    public SampleInfo getInfo(String name, long flags) throws NotFoundException {
        return getInfo(name, (int) flags);
    }
}
