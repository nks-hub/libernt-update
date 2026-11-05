/* VM-only fixture: the guarded offline probe installs this driver to test recovery before SMSS. */
__declspec(dllimport) __declspec(noreturn) void KeBugCheckEx(unsigned long code,
    unsigned long long p1, unsigned long long p2, unsigned long long p3, unsigned long long p4);

long DriverEntry(void* driver, void* registryPath) {
    (void)driver;
    (void)registryPath;
    KeBugCheckEx(0xe2, 0x4c555550, 0, 0, 0);
}
