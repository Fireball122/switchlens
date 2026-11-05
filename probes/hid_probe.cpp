// hid_probe v3: controller test with a clean screen and a final report.
//   Phase A  0-12 s  baseline            (press buttons)
//   Phase B 12-27 s  swap mapping        (press buttons)
//   Phase C 27-42 s  virtual Pro Pad     (it presses A by itself; you keep pressing too)
// Report is shown at the end; press + to exit (auto-exit after 2 minutes; HOME also works).
// Log: sdmc:/switch/ar_glasses/hidprobe3_<n>.txt
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

// Screen helpers: fixed-position redraw, no scrolling.
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
static PadState g_padNo1, g_padHand, g_padAny;

struct Stats {
    u32 samples = 0;
    u32 realHand = 0;   // samples where the Handheld npad reported a button
    u32 realNo1 = 0;    // samples where Player 1 reported a button (not used in phase C)
    u32 realHdls = 0;   // samples where a rail Joy-Con entry in the HDLS dump had a button
    u32 virtA = 0;      // phase C: Player 1 showed A
    u32 virtExpect = 0; // phase C: samples where we were holding A
    u32 no1Style = 0;   // last style set seen on Player 1
    u32 handStyle = 0;
    u64 lastBtn = 0;
};
static Stats g_s[3];

static const char *phaseName[3] = {"A  BASELINE", "B  SWAP MAPPING", "C  VIRTUAL CONTROLLER"};
static const char *phaseHint[3] = {
    "Mash buttons on the Joy-Cons (A B X Y L R ZL ZR, move sticks).",
    "Keep mashing. The old swap mapping is applied now.",
    "A fake Pro Controller presses A by itself. KEEP MASHING the Joy-Cons too.",
};

static u64 sampleHdls(void) {
    if (!g_hdlsOk) return 0;
    memset(&g_list, 0, sizeof(g_list));
    if (R_FAILED(hiddbgDumpHdlsStates(g_sess, &g_list))) return 0;
    u64 b = 0;
    for (int i = 0; i < g_list.total_entries && i < 16; i++)
        if (g_list.entries[i].device.npadInterfaceType == HidNpadInterfaceType_Rail) b |= g_list.entries[i].state.buttons;
    return b;
}

static void swapMapping(bool on) {
    if (on) {
        hidsysEnableHandheldHids();
        hidSetNpadHandheldActivationMode(HidNpadHandheldActivationMode_Dual);
        hidSwapNpadAssignment(HidNpadIdType_Handheld, HidNpadIdType_No1);
        hidSetNpadJoyAssignmentModeDual(HidNpadIdType_No1);
    } else {
        hidSwapNpadAssignment(HidNpadIdType_No1, HidNpadIdType_Handheld);
    }
}

static Result virtualPad(bool pressA) {
    Result r = 0;
    if (!g_hdlsOk) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (!g_vAttached) {
        HiddbgHdlsDeviceInfo info;
        memset(&info, 0, sizeof(info));
        info.deviceType = HidDeviceType_FullKey3;
        info.npadInterfaceType = HidNpadInterfaceType_Bluetooth;
        info.singleColorBody = 0xFF303030;
        info.singleColorButtons = 0xFFFFFFFF;
        info.colorLeftGrip = 0xFF303030;
        info.colorRightGrip = 0xFF303030;
        r = hiddbgAttachHdlsVirtualDevice(&g_vh, &info);
        g_vAttached = R_SUCCEEDED(r);
        logf("attach virtual rc=0x%x", r);
        if (!g_vAttached) return r;
    }
    HiddbgHdlsState st;
    memset(&st, 0, sizeof(st));
    st.battery_level = 4;
    st.buttons = pressA ? HidNpadButton_A : 0;
    return hiddbgSetHdlsState(g_vh, &st);
}

static void yn(char *dst, size_t n, u32 c) {
    if (c) snprintf(dst, n, "YES  (%u samples)", c);
    else snprintf(dst, n, "NO");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    mkdir("sdmc:/switch/ar_glasses", 0777);
    int logN = 0;
    for (int n = 1; n < 20 && !g_log; n++) {
        char p[96];
        snprintf(p, sizeof(p), "sdmc:/switch/ar_glasses/hidprobe3_%d.txt", n);
        FILE *t = fopen(p, "rb");
        if (t) {
            fclose(t);
            continue;
        }
        g_log = fopen(p, "w");
        logN = n;
    }

    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitialize(&g_padNo1, HidNpadIdType_No1);
    padInitialize(&g_padHand, HidNpadIdType_Handheld);
    padInitializeAny(&g_padAny);
    hidInitializeNpad();

    Result rcSys = hidsysInitialize();
    bool hidsysOk = R_SUCCEEDED(rcSys);
    Result rcDbg = hiddbgInitialize();
    Result rcBuf = 0xFFFF;
    if (R_SUCCEEDED(rcDbg)) {
        rcBuf = hiddbgAttachHdlsWorkBuffer(&g_sess, g_hdlsBuf, sizeof(g_hdlsBuf));
        g_hdlsOk = R_SUCCEEDED(rcBuf);
    }
    logf("hidsys=0x%x hiddbg=0x%x workbuf=0x%x", rcSys, rcDbg, rcBuf);

    const int T_B = 12, T_C = 27, T_END = 42;
    const u64 start = armTicksToNs(armGetSystemTick());
    int phase = -1;
    bool bOn = false;
    u64 lastLog = 0;
    Result lastVr = 0;

    printf("\x1b[2J");
    while (true) {
        u64 now = armTicksToNs(armGetSystemTick());
        double tsec = (double)(now - start) / 1e9;
        if (tsec >= T_END) break;
        int p = tsec < T_B ? 0 : (tsec < T_C ? 1 : 2);

        if (p != phase) {
            if (p == 1 && hidsysOk) {
                swapMapping(true);
                bOn = true;
                logf("== phase B: swap mapping ON");
            }
            if (p == 2) {
                if (bOn) {
                    swapMapping(false);
                    bOn = false;
                }
                logf("== phase C: virtual controller");
            }
            if (p == 0) logf("== phase A: baseline");
            phase = p;
        }

        bool vHold = (p == 2) && (((int)tsec) & 1) == 0;
        if (p == 2) lastVr = virtualPad(vHold);

        padUpdate(&g_padNo1);
        padUpdate(&g_padHand);
        u64 bNo1 = padGetButtons(&g_padNo1);
        u64 bHand = padGetButtons(&g_padHand);
        u64 bHdls = sampleHdls();
        Stats &s = g_s[p];
        s.samples++;
        s.handStyle = padGetStyleSet(&g_padHand);
        s.no1Style = padGetStyleSet(&g_padNo1);
        if (bHand) s.realHand++;
        if (bHdls) s.realHdls++;
        if (p != 2) {
            if (bNo1) s.realNo1++;
        } else {
            if (vHold) {
                s.virtExpect++;
                if (bNo1 & HidNpadButton_A) s.virtA++;
            }
        }
        s.lastBtn = bHand | bHdls | (p != 2 ? bNo1 : 0);

        if (now - lastLog >= 1000000000ULL) {
            lastLog = now;
            logf("t=%4.1f ph=%c handSt=%x no1St=%x hand=%llx no1=%llx hdlsRail=%llx", tsec, 'A' + p, s.handStyle,
                 s.no1Style, (unsigned long long)bHand, (unsigned long long)bNo1, (unsigned long long)bHdls);
        }

        at(1);
        ln("=== CONTROLLER PROBE ===     time %2d / %d s", (int)tsec, T_END);
        ln("");
        ln(" PHASE %s", phaseName[p]);
        ln(" >>> %s", phaseHint[p]);
        ln("");
        ln(" Handheld pad style=%u   Player1 pad style=%u", s.handStyle, s.no1Style);
        ln(" Buttons now: handheld=%llx  player1=%llx  rail(dump)=%llx", (unsigned long long)bHand,
           (unsigned long long)bNo1, (unsigned long long)bHdls);
        ln("");
        ln(" Real Joy-Con presses detected so far:");
        ln("   handheld pad: %u    rail dump: %u    player1: %u", s.realHand, s.realHdls, s.realNo1);
        if (p == 2) {
            ln(" Virtual pad: A seen on Player 1 %u of %u samples (vr=0x%x)", s.virtA, s.virtExpect, lastVr);
        } else {
            ln("");
        }
        ln("");
        ln(" (nothing to do but press buttons; the report comes at the end)");
        consoleUpdate(NULL);
        svcSleepThread(50000000LL);
    }

    if (g_vAttached) {
        Result r = hiddbgDetachHdlsVirtualDevice(g_vh);
        logf("detach virtual rc=0x%x", r);
    }
    if (bOn) swapMapping(false);
    if (g_hdlsOk) hiddbgReleaseHdlsWorkBuffer(g_sess);
    if (R_SUCCEEDED(rcDbg)) hiddbgExit();
    if (hidsysOk) hidsysExit();

    // ---- report ----
    char a1[40], a2[40], a3[40], b1[40], b2[40], b3[40], c1[40], c2[40];
    u32 aAny = g_s[0].realHand + g_s[0].realHdls + g_s[0].realNo1;
    u32 bAny = g_s[1].realHand + g_s[1].realHdls + g_s[1].realNo1;
    yn(a1, sizeof(a1), g_s[0].realHand);
    yn(a2, sizeof(a2), g_s[0].realHdls);
    yn(a3, sizeof(a3), g_s[0].realNo1);
    yn(b1, sizeof(b1), g_s[1].realNo1);
    yn(b2, sizeof(b2), g_s[1].realHand);
    yn(b3, sizeof(b3), g_s[1].realHdls);
    yn(c1, sizeof(c1), g_s[2].realHand);
    yn(c2, sizeof(c2), g_s[2].realHdls);
    bool virtOk = g_s[2].virtA > 0;

    printf("\x1b[2J");
    at(1);
    ln("=== REPORT  (also saved as hidprobe3_%d.txt) ===", logN);
    ln("");
    ln(" A baseline: real presses seen on");
    ln("     handheld pad : %s", a1);
    ln("     rail dump    : %s", a2);
    ln("     player 1     : %s", a3);
    ln(" B swap mapping: real presses seen on");
    ln("     player 1     : %s", b1);
    ln("     handheld pad : %s", b2);
    ln("     rail dump    : %s", b3);
    ln("     Player1 style after swap = %u (0 = swap did not show here)", g_s[1].no1Style);
    ln(" C virtual Pro Controller:");
    ln("     fake A visible on player 1: %s (%u/%u)", virtOk ? "YES" : "NO", g_s[2].virtA, g_s[2].virtExpect);
    ln("     Player1 style = %u (1 = Pro Controller)", g_s[2].no1Style);
    ln("     real presses  handheld: %s   rail dump: %s", c1, c2);
    ln("");
    ln(" VERDICT");
    ln("   Joy-Con buttons readable here : %s", (aAny || bAny || g_s[2].realHand || g_s[2].realHdls) ? "YES" : "NO / you did not press");
    ln("   Virtual Pro Controller works  : %s", virtOk ? "YES" : "NO");
    ln("");
    ln(" Tell Claude: which AR mode this was (off/on) and this screen.");
    ln(" Press + to exit (auto-exit in 2 minutes).");
    consoleUpdate(NULL);

    logf("REPORT A: hand=%u rail=%u no1=%u | B: no1=%u hand=%u rail=%u no1style=%u | C: virtA=%u/%u no1style=%u hand=%u rail=%u",
         g_s[0].realHand, g_s[0].realHdls, g_s[0].realNo1, g_s[1].realNo1, g_s[1].realHand, g_s[1].realHdls,
         g_s[1].no1Style, g_s[2].virtA, g_s[2].virtExpect, g_s[2].no1Style, g_s[2].realHand, g_s[2].realHdls);
    if (g_log) fclose(g_log);

    padInitializeAny(&g_padAny);
    u64 t0 = armTicksToNs(armGetSystemTick());
    while (appletMainLoop()) {
        padUpdate(&g_padAny);
        if (padGetButtonsDown(&g_padAny) & HidNpadButton_Plus) break;
        if (armTicksToNs(armGetSystemTick()) - t0 > 120ULL * 1000000000ULL) break;
        svcSleepThread(50000000LL);
    }
    consoleExit(NULL);
    return 0;
}
