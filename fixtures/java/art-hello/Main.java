// art-hello: the first Java program on Minato's ART (ADR-0033 test 1). dalvikvm runs it as dalvikvm64 -Xint does
// on Android 15, and both print the same lines.
public class Main {
    public static void main(String[] args) {
        System.out.println("Hello from ART");
        System.out.println("vm: " + System.getProperty("java.vm.name") + " " + System.getProperty("java.vm.version"));
    }
}
