// gyro_probe: can the rail Joy-Con motion data be read while docked, and can it be forwarded
// through a virtual Pro Controller?
//   Phase A  0-10 s  keep the console STILL (Joy-Cons attached, do not touch)
//   Phase B 10-30 s  SHAKE / ROTATE the console (tilt it, wave it around)
//   Phase C 30-50 s  a virtual Pro Controller forwards the rail motion; keep shaking
// Sources measured (accel range = max-min summed over x,y,z, in g):
//   hdlsA : HDLS dump, rail Joy-Con entry, acceleration
//   hdlsG : HDLS dump, rail Joy-Con entry, angle
//   hand  : libnx hid SixAxis on the Handheld npad
//   no1   : libnx hid SixAxis on Player 1 (in phase C this is the virtual pad)
// Report is shown at the end; press + to exit. Log: sdmc:/switch/ar_glasses/gyroprobe_<n>.txt
#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static FILE *g_log = nullptr;
static void logf(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}
static void at(int row) { printf("\x1b[%d;1H", row); }
static void ln(const char *fmt, ...) {
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    printf("%s\x1b[K\n", b);
}

alignas(0x1000) static u8 g_hdlsBuf[0x1000];
static HiddbgHdlsSessionId g_sess;
static bool g_hdlsOk = false;
static HiddbgHdlsHandle g_vh;
static bool g_vAttached = false;
static HiddbgHdlsStateList g_list;

struct Rng {
    float mn[3], mx[3];
    u32 n;
    void reset() {
        n = 0;
        for (int i = 0; i < 3; i++) { mn[i] = 1e9f; mx[i] = -1e9f; }
    }
    void add(const HidVector &v) {
        float a[3] = {v.x, v.y, v.z};
        for (int i = 0; i < 3; i++) {
            if (a[i] < mn[i]) mn[i] = a[i];
            if (a[i] > mx[i]) mx[i] = a[i];
        }
        n++;
    }
    float range() const {
        if (!n) return 0;
        float s = 0;
        for (int i = 0; i < 3; i++) s += mx[i] - mn[i];
        return s;
    }
};
enum { S_HDLS_A, S_HDLS_G, S_HAND, S_NO1, S_COUNT };
static const char *srcName[S_COUNT] = {"HDLS rail accel", "HDLS rail angle", "hid Handheld accel", "hid Player1 accel"};
static Rng g_r[3][S_COUNT];

// Fetch the latest sixaxis state of an npad (first active style). Returns style used or 0.
static u32 readSix(HidNpadIdType id, u32 styleSet, HidSixAxisSensorState *out) {
    static const HidNpadStyleTag styles[] = {HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadJoyDual,
                                             HidNpadStyleTag_NpadJoyLeft, HidNpadStyleTag_NpadJoyRight,
                                             HidNpadStyleTag_NpadFullKey};
    for (auto st : styles) {
        if (!(styleSet & st)) continue;
        HidSixAxisSensorHandle h[2];
        int total = (st == HidNpadStyleTag_NpadJoyDual) ? 2 : 1;
        if (R_FAILED(hidGetSixAxisSensorHandles(h, total, id, st))) continue;
        hidStartSixAxisSensor(h[0]);
        if (hidGetSixAxisSensorStates(h[0], out, 1) < 1) continue;
        return (u32)st;
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    mkdir("sdmc:/switch/ar_glasses", 0777);
    int logN = 0;
    for (int n = 1; n < 20 && !g_log; n++) {
        char p[96];
        snprintf(p, sizeof(p), "sdmc:/switch/ar_glasses/gyroprobe_%d.txt", n);
        FILE *t = fopen(p, "rb");
        if (t) { fclose(t); continue; }
        g_log = fopen(p, "w");
        logN = n;
    }
    for (auto &p : g_r) for (auto &r : p) r.reset();

    PadState padNo1, padHand;
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitialize(&padNo1, HidNpadIdType_No1);
    padInitialize(&padHand, HidNpadIdType_Handheld);
    hidInitializeNpad();

    // Experiment: activate the Handheld npad in Dual mode (no swap) so the real Joy-Cons are exposed.
    Result rcSys = hidsysInitialize();
    HidNpadHandheldActivationMode oldMode = HidNpadHandheldActivationMode_Dual;
    Result rcGetMode = hidGetNpadHandheldActivationMode(&oldMode);
    Result rcEn = R_SUCCEEDED(rcSys) ? hidsysEnableHandheldHids() : rcSys;
    Result rcMode = hidSetNpadHandheldActivationMode(HidNpadHandheldActivationMode_Dual);
    logf("hidsys=0x%x oldMode=%d (rc=0x%x) enableHandheld=0x%x setDual=0x%x", rcSys, (int)oldMode, rcGetMode, rcEn, rcMode);
    Result rcDbg = hiddbgInitialize();
    Result rcBuf = 0xFFFF;
    if (R_SUCCEEDED(rcDbg)) {
        rcBuf = hiddbgAttachHdlsWorkBuffer(&g_sess, g_hdlsBuf, sizeof(g_hdlsBuf));
        g_hdlsOk = R_SUCCEEDED(rcBuf);
    }
    logf("hiddbg=0x%x workbuf=0x%x", rcDbg, rcBuf);

    const int T_B = 10, T_C = 30, T_END = 50;
    const u64 start = armTicksToNs(armGetSystemTick());
    u64 lastLog = 0;
    int railEntries = 0, chosenType = -1;
    u32 handStyleUsed = 0, no1StyleUsed = 0;
    Result lastSet = 0;
    float curA = 0, curG = 0;

    printf("\x1b[2J");
    while (true) {
        u64 now = armTicksToNs(armGetSystemTick());
        double tsec = (double)(now - start) / 1e9;
        if (tsec >= T_END) break;
        int p = tsec < T_B ? 0 : (tsec < T_C ? 1 : 2);

        // --- HDLS dump: real rail Joy-Con entries ---
        const HiddbgHdlsStateListEntry *pick = nullptr;
        railEntries = 0;
        if (g_hdlsOk) {
            memset(&g_list, 0, sizeof(g_list));
            if (R_SUCCEEDED(hiddbgDumpHdlsStates(g_sess, &g_list))) {
                for (int i = 0; i < g_list.total_entries && i < 16; i++) {
                    const auto &e = g_list.entries[i];
                    if (e.device.npadInterfaceType != HidNpadInterfaceType_Rail) continue;
                    railEntries++;
                    if (!pick || e.device.deviceType == HidDeviceType_JoyRight1) pick = &e;
                }
            }
        }
        if (pick) {
            chosenType = pick->device.deviceType;
            g_r[p][S_HDLS_A].add(pick->state.six_axis_sensor_acceleration);
            g_r[p][S_HDLS_G].add(pick->state.six_axis_sensor_angle);
            curA = pick->state.six_axis_sensor_acceleration.x;
            curG = pick->state.six_axis_sensor_angle.x;
        }

        // --- phase C: forward to a virtual pad ---
        if (p == 2 && g_hdlsOk) {
            if (!g_vAttached) {
                HiddbgHdlsDeviceInfo info;
                memset(&info, 0, sizeof(info));
                info.deviceType = HidDeviceType_FullKey3;
                info.npadInterfaceType = HidNpadInterfaceType_Bluetooth;
                info.singleColorBody = 0xFF303030;
                info.singleColorButtons = 0xFFFFFFFF;
                info.colorLeftGrip = 0xFF303030;
                info.colorRightGrip = 0xFF303030;
                Result r = hiddbgAttachHdlsVirtualDevice(&g_vh, &info);
                g_vAttached = R_SUCCEEDED(r);
                logf("attach virtual rc=0x%x", r);
            }
            if (g_vAttached) {
                HiddbgHdlsState st;
                memset(&st, 0, sizeof(st));
                st.battery_level = 4;
                if (pick) {
                    st.six_axis_sensor_acceleration = pick->state.six_axis_sensor_acceleration;
                    st.six_axis_sensor_angle = pick->state.six_axis_sensor_angle;
                }
                st.attribute = HiddbgHdlsAttribute_HasVirtualSixAxisSensorAcceleration |
                               HiddbgHdlsAttribute_HasVirtualSixAxisSensorAngle;
                lastSet = hiddbgSetHdlsState(g_vh, &st);
            }
        }

        // --- libnx hid sixaxis ---
        padUpdate(&padNo1);
        padUpdate(&padHand);
        HidSixAxisSensorState ss;
        u32 sty = readSix(HidNpadIdType_Handheld, padGetStyleSet(&padHand), &ss);
        if (sty) { handStyleUsed = sty; g_r[p][S_HAND].add(ss.acceleration); }
        sty = readSix(HidNpadIdType_No1, padGetStyleSet(&padNo1), &ss);
        if (sty) { no1StyleUsed = sty; g_r[p][S_NO1].add(ss.acceleration); }

        if (now - lastLog >= 1000000000ULL) {
            lastLog = now;
            logf("t=%4.1f ph=%c rail=%d type=%d hdlsAx=%.3f hdlsAngX=%.3f handSty=%x no1Sty=%x set=0x%x", tsec,
                 'A' + p, railEntries, chosenType, curA, curG, handStyleUsed, no1StyleUsed, lastSet);
        }

        const char *hint[3] = {"Keep the console STILL.", "SHAKE and ROTATE the console now!",
                               "Virtual pad forwarding. KEEP SHAKING."};
        at(1);
        ln("=== GYRO PROBE ===        time %2d / %d s", (int)tsec, T_END);
        ln("");
        ln(" >>> %s", hint[p]);
        ln("");
        ln(" Rail Joy-Con entries in HDLS dump: %d (type %d)", railEntries, chosenType);
        ln(" Live: accel.x=%.3f  angle.x=%.3f", curA, curG);
        ln(" hid styles: handheld=%x  player1=%x", handStyleUsed, no1StyleUsed);
        ln(" Motion range this phase (higher = moving data):");
        for (int s = 0; s < S_COUNT; s++)
            ln("   %-20s %7.3f   (%u samples)", srcName[s], g_r[p][s].range(), g_r[p][s].n);
        if (p == 2) ln(" virtual set-state rc=0x%x", lastSet);
        consoleUpdate(NULL);
        svcSleepThread(20000000LL);
    }

    hidSetNpadHandheldActivationMode(oldMode);
    if (R_SUCCEEDED(rcSys)) hidsysExit();
    if (g_vAttached) logf("detach virtual rc=0x%x", hiddbgDetachHdlsVirtualDevice(g_vh));
    if (g_hdlsOk) hiddbgReleaseHdlsWorkBuffer(g_sess);
    if (R_SUCCEEDED(rcDbg)) hiddbgExit();

    printf("\x1b[2J");
    at(1);
    ln("=== GYRO REPORT  (saved as gyroprobe_%d.txt) ===", logN);
    ln("");
    ln(" Motion range (A still | B shaking | C shaking+virtual):");
    for (int s = 0; s < S_COUNT; s++) {
        ln(" %-20s %6.3f | %6.3f | %6.3f   n=%u/%u/%u", srcName[s], g_r[0][s].range(), g_r[1][s].range(),
           g_r[2][s].range(), g_r[0][s].n, g_r[1][s].n, g_r[2][s].n);
        logf("RESULT %s still=%.3f shake=%.3f virt=%.3f", srcName[s], g_r[0][s].range(), g_r[1][s].range(),
             g_r[2][s].range());
    }
    ln("");
    ln(" Rail entries: %d   hid styles: handheld=%x player1=%x", railEntries, handStyleUsed, no1StyleUsed);
    ln("");
    ln(" Send me this screen (photo) or gyroprobe_%d.txt.", logN);
    ln(" Press + to exit.");
    consoleUpdate(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pe;
    padInitializeDefault(&pe);
    u64 t0 = armTicksToNs(armGetSystemTick());
    while (appletMainLoop()) {
        padUpdate(&pe);
        if (padGetButtonsDown(&pe) & HidNpadButton_Plus) break;
        if (armTicksToNs(armGetSystemTick()) - t0 > 120000000000ULL) break;
        svcSleepThread(50000000LL);
    }
    consoleExit(NULL);
    return 0;
}
