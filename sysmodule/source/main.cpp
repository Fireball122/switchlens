// Switchlens sysmodule: resident AR-glasses manager (boot2 sysmodule, program id 420000000007E51C).
//
// Interface with the Switchlens overlay (files in sdmc:/switch/ar_glasses/):
//   active.flag     '1' = user wants AR on, '0' = off
//   controller.flag '0' = Joy-Con passthrough off, '1' (default) = Joy-Cons act as a wireless Pro Controller
//                   while AR is on, '2'/'3' = same plus experimental gyro forwarding (see docs)
//   state.bin       ArGlassesStatus telemetry, heartbeat increments while alive
// Log: log.txt (current boot), log_prev.txt (previous boot).
#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern "C" {
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
}

// libnx's default heap init claims nearly all free system-pool memory, which starves
// am at boot (fatal 2001-0132). A fixed heap is mandatory for a boot2 sysmodule.
#define HEAP_SIZE 0x40000
extern "C" void __libnx_initheap(void) {
    static u8 heap[HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof(heap);
}

static Result g_rcSetsys = 0;
static Result g_rcFs = 0;

extern "C" void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);

    g_rcSetsys = setsysInitialize();
    if (R_SUCCEEDED(g_rcSetsys)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        }
    }

    g_rcFs = fsInitialize();
    if (R_SUCCEEDED(g_rcFs)) fsdevMountSdmc();
}

extern "C" void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    if (R_SUCCEEDED(g_rcSetsys)) setsysExit();
    smExit();
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
#define STATE_DIR    "sdmc:/switch/ar_glasses"
#define LOG_FILE     STATE_DIR "/log.txt"
#define LOG_PREV     STATE_DIR "/log_prev.txt"
#define ACTIVE_FLAG  STATE_DIR "/active.flag"
#define JOY_FLAG     STATE_DIR "/controller.flag"
#define STATUS_FILE  STATE_DIR "/state.bin"

#define LOG_MAX_BYTES (512 * 1024)

enum : u32 { STATE_IDLE = 0, STATE_NEGOTIATING = 1, STATE_ACTIVE = 2, STATE_SLEEPING = 3 };

#pragma pack(push, 1)
struct ArGlassesStatus {
    u32 magic;      // 'ARGS'
    u32 version;    // 1
    u32 state;
    u16 status1;
    u16 dp_status;
    u16 alert_status;
    u16 reserved;
    u64 heartbeat;
};
#pragma pack(pop)

static u64 g_logBytes = 0;

static u64 nowMs(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

static void logLine(const char *fmt, ...) {
    if (g_logBytes > LOG_MAX_BYTES) return;
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    int n = fprintf(f, "[%8llu] ", (unsigned long long)nowMs());
    va_list ap;
    va_start(ap, fmt);
    n += vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    if (n > 0) g_logBytes += (u64)n + 1;
}

// Returns the first character of the file if it is a digit ('0'..'9'), else `def`.
static char readFlag(const char *path, char def) {
    FILE *f = fopen(path, "rb");
    if (!f) return def;
    int c = fgetc(f);
    fclose(f);
    return (c >= '0' && c <= '9') ? (char)c : def;
}

static u64 g_heartbeat = 0;

static volatile bool g_vpadAttached = false;  // virtual Pro Controller currently attached (set by the worker thread)
static volatile char g_joyFlag = '1';         // controller.flag: '0' off, '1' buttons only, '2' + gyro (angular velocity), '3' + gyro (angle)

static void writeStatus(u32 state, u16 st1, u16 dp) {
    ArGlassesStatus s = {};
    s.magic = 0x53475241;  // 'ARGS'
    s.version = 1;
    s.state = state;
    s.status1 = st1;
    s.dp_status = dp;
    s.reserved = g_vpadAttached ? 1 : 0;  // bit0: Joy-Con passthrough (virtual Pro Controller) active
    s.heartbeat = ++g_heartbeat;
    FILE *f = fopen(STATUS_FILE, "wb");
    if (!f) return;
    fwrite(&s, sizeof(s), 1, f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// BM92T36 (USB-PD controller)
// ---------------------------------------------------------------------------
#define REG_STATUS1      0x03
#define REG_COMMAND      0x05
#define REG_DP_STATUS    0x18
#define REG_INCOMING_VDM 0x50
#define REG_OUTGOING_VDM 0x60

#define CMD_DP_ENTER_MODE        0x3131
#define CMD_START_HPD            0x3434
#define CMD_DP_CFG_AND_START_HPD 0x3636
#define CMD_STOP_HPD_EXIT_DP     0x3B3B

#define STATUS1_INSERT   0x0080
#define DP_STATUS_DP_EN  0x8000
#define DP_STATUS_HPD    0x0080

static I2cSession g_i2c;
static bool g_i2cOpen = false;

static Result bmRead(u8 reg, void *buf, size_t size) {
    Result rc = i2csessionSendAuto(&g_i2c, &reg, 1, I2cTransactionOption_Start);
    if (R_FAILED(rc)) return rc;
    return i2csessionReceiveAuto(&g_i2c, buf, size, I2cTransactionOption_All);
}

static Result bmRead16(u8 reg, u16 *out) {
    u8 b[2] = {0, 0};
    Result rc = bmRead(reg, b, sizeof(b));
    *out = (u16)(b[0] | (b[1] << 8));
    return rc;
}

static Result bmWrite(const void *buf, size_t size) {
    return i2csessionSendAuto(&g_i2c, buf, size, I2cTransactionOption_All);
}

static Result bmCmd(u16 cmd) {
    u8 b[3] = {REG_COMMAND, (u8)(cmd & 0xFF), (u8)(cmd >> 8)};
    return bmWrite(b, sizeof(b));
}

static void sleepMs(u64 ms) {
    svcSleepThread((s64)(ms * 1000000ULL));
}

// ---------------------------------------------------------------------------
// OS side: omm operation-mode policy, TV underscan
// ---------------------------------------------------------------------------
// omm command 10 = SetOperationModePolicy: 0 auto, 1 force handheld, 2 force docked.
static Result ommSetPolicy(u32 policy) {
    Service omm;
    Result rc = smGetService(&omm, "omm");
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatchIn(&omm, 10, policy);
    serviceClose(&omm);
    return rc;
}

static bool g_underscanChecked = false;

static void fixUnderscan(void) {
    if (g_underscanChecked || R_FAILED(g_rcSetsys)) return;
    g_underscanChecked = true;
    SetSysTvSettings tv;
    Result rc = setsysGetTvSettings(&tv);
    if (R_FAILED(rc)) {
        logLine("setsysGetTvSettings rc=0x%x", rc);
        return;
    }
    if (tv.underscan == 0) {
        logLine("underscan already 0");
        return;
    }
    u32 old = tv.underscan;
    tv.underscan = 0;
    rc = setsysSetTvSettings(&tv);
    logLine("underscan %u -> 0 rc=0x%x", old, rc);
}

// ---------------------------------------------------------------------------
// Virtual Pro Controller (controller.flag = '1', the default).
// The real rail Joy-Cons are readable through hiddbgDumpHdlsStates even while docked (probe
// v3, AR on), and a virtual Bluetooth Pro Controller is accepted on Player 1. A worker thread
// merges the two Joy-Con entries into one Pro Controller state at ~125 Hz, so games see an
// ordinary wireless pad (the case the Handheld->Player 1 swap cannot satisfy: Mario Kart 8).
// All hiddbg calls live in this thread; the main loop only flips g_vpadWant, and the psc
// handler only flips g_vpadQuiet (the thread makes no call while the console sleeps).
// ---------------------------------------------------------------------------
static volatile bool g_vpadWant = false;
static volatile bool g_vpadQuiet = false;
static bool g_vpadThreadUp = false;
static Thread g_vpadThread;
alignas(0x1000) static u8 g_hdlsBuf[0x1000];
static HiddbgHdlsStateList g_hdlsList;

static void vpadThreadFn(void *) {
    bool dbgInit = false, haveBuf = false, attached = false;
    HiddbgHdlsSessionId sess = {};
    HiddbgHdlsHandle handle = {};
    u32 setFails = 0, attachFails = 0;
    u64 sent = 0;
    const u64 stripSlSr = ~((u64)HidNpadButton_LeftSL | HidNpadButton_LeftSR | HidNpadButton_RightSL | HidNpadButton_RightSR);
    // Gyro: hidsysEnableHandheldHids exposes the real rail Joy-Cons on the Handheld npad (JoyDual),
    // where hid six-axis data is readable (probe: shake range 20 g vs 0.1 g still). HDLS dump entries carry no motion.
    bool hidInit = false, hidsysInit = false, handheldWas = true, handheldForced = false, gyroTried = false;
    HidSixAxisSensorHandle sixH[2] = {};
    bool haveSix = false;
    u64 lastSixMs = 0;

    while (true) {
        if (g_vpadQuiet) {
            sleepMs(50);
            continue;
        }
        if (!g_vpadWant) {
            if (attached) {
                Result r = hiddbgDetachHdlsVirtualDevice(handle);
                logLine("vpad: detached rc=0x%x (sent %llu states)", r, (unsigned long long)sent);
                attached = false;
                g_vpadAttached = false;
            }
            if (haveBuf) {
                hiddbgReleaseHdlsWorkBuffer(sess);
                haveBuf = false;
            }
            if (handheldForced) {
                Result r = hidsysDisableHandheldHids();
                logLine("vpad: handheld hids restored (was off) rc=0x%x", r);
                handheldForced = false;
            }
            haveSix = false;
            gyroTried = false;
            sleepMs(100);
            continue;
        }
        if (!dbgInit) {
            Result r = hiddbgInitialize();
            dbgInit = R_SUCCEEDED(r);
            logLine("vpad: hiddbgInitialize rc=0x%x", r);
            if (!dbgInit) {
                sleepMs(3000);
                continue;
            }
        }
        if (!haveBuf) {
            Result r = hiddbgAttachHdlsWorkBuffer(&sess, g_hdlsBuf, sizeof(g_hdlsBuf));
            haveBuf = R_SUCCEEDED(r);
            if (!haveBuf) {
                if (attachFails++ < 5) logLine("vpad: AttachHdlsWorkBuffer rc=0x%x", r);
                sleepMs(2000);
                continue;
            }
        }
        if (g_joyFlag >= '2' && !gyroTried) {
            gyroTried = true;
            Result r1 = hidInit ? 0 : hidInitialize();
            hidInit = hidInit || R_SUCCEEDED(r1);
            Result r2 = hidsysInit ? 0 : hidsysInitialize();
            hidsysInit = hidsysInit || R_SUCCEEDED(r2);
            Result r3 = 0, r4 = 0;
            if (hidInit && hidsysInit) {
                padConfigureInput(8, HidNpadStyleSet_NpadStandard);
                hidInitializeNpad();
                hidsysIsHandheldHidsEnabled(&handheldWas);
                r3 = hidsysEnableHandheldHids();
                handheldForced = R_SUCCEEDED(r3) && !handheldWas;
            }
            logLine("vpad: gyro init hid=0x%x hidsys=0x%x enableHandheld=0x%x (was %d)", r1, r2, r3 | r4, handheldWas ? 1 : 0);
        }
        if (!attached) {
            HiddbgHdlsDeviceInfo info = {};
            info.deviceType = HidDeviceType_FullKey3;
            info.npadInterfaceType = HidNpadInterfaceType_Bluetooth;
            info.singleColorBody = 0xFF303030;
            info.singleColorButtons = 0xFFFFFFFF;
            info.colorLeftGrip = 0xFF303030;
            info.colorRightGrip = 0xFF303030;
            Result r = hiddbgAttachHdlsVirtualDevice(&handle, &info);
            attached = R_SUCCEEDED(r);
            if (attached) {
                g_vpadAttached = true;
                logLine("vpad: virtual Pro Controller attached");
                setFails = 0;
                attachFails = 0;
                sent = 0;
            } else {
                if (attachFails++ < 5) logLine("vpad: AttachHdlsVirtualDevice rc=0x%x", r);
                sleepMs(2000);
                continue;
            }
        }

        // Merge the rail Joy-Cons (interface Rail) into one Pro Controller state.
        HiddbgHdlsState st = {};
        st.battery_level = 4;
        memset(&g_hdlsList, 0, sizeof(g_hdlsList));
        if (R_SUCCEEDED(hiddbgDumpHdlsStates(sess, &g_hdlsList))) {
            for (int i = 0; i < g_hdlsList.total_entries && i < 16; i++) {
                const HiddbgHdlsStateListEntry *e = &g_hdlsList.entries[i];
                if (e->device.npadInterfaceType != HidNpadInterfaceType_Rail) continue;
                st.buttons |= e->state.buttons & stripSlSr;
                const HidAnalogStickState &l = e->state.analog_stick_l, &r = e->state.analog_stick_r;
                bool leftJc = e->device.deviceType == HidDeviceType_JoyLeft2 || e->device.deviceType == HidDeviceType_JoyLeft4;
                const HidAnalogStickState &pick = (l.x || l.y) ? l : r;
                if (leftJc) st.analog_stick_l = pick;
                else st.analog_stick_r = (r.x || r.y) ? r : l;
            }
        }
        if (g_joyFlag >= '2' && hidInit && hidsysInit) {
            u64 nm = nowMs();
            if (!haveSix || nm - lastSixMs > 500) {
                lastSixMs = nm;
                haveSix = R_SUCCEEDED(hidGetSixAxisSensorHandles(sixH, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadJoyDual));
                if (haveSix) {
                    hidStartSixAxisSensor(sixH[0]);
                    hidStartSixAxisSensor(sixH[1]);
                }
            }
            HidSixAxisSensorState ss;
            if (haveSix && hidGetSixAxisSensorStates(sixH[1], &ss, 1) >= 1) {
                st.six_axis_sensor_acceleration = ss.acceleration;
                st.six_axis_sensor_angle = (g_joyFlag == '3') ? ss.angle : ss.angular_velocity;
                st.attribute = HiddbgHdlsAttribute_HasVirtualSixAxisSensorAcceleration | HiddbgHdlsAttribute_HasVirtualSixAxisSensorAngle;
            }
        }
        Result rs = hiddbgSetHdlsState(handle, &st);
        if (R_FAILED(rs)) {
            if (++setFails == 20) {
                logLine("vpad: SetHdlsState failing (rc=0x%x), reattaching", rs);
                hiddbgDetachHdlsVirtualDevice(handle);
                attached = false;
                g_vpadAttached = false;
            }
        } else {
            setFails = 0;
            sent++;
        }
        sleepMs(8);
    }
}

static void controllerOn(void) {
    if (!g_vpadThreadUp) {
        Result r = threadCreate(&g_vpadThread, vpadThreadFn, NULL, NULL, 0x4000, 44, 3);
        if (R_SUCCEEDED(r)) r = threadStart(&g_vpadThread);
        logLine("vpad: thread create/start rc=0x%x", r);
        g_vpadThreadUp = R_SUCCEEDED(r);
    }
    g_vpadWant = g_vpadThreadUp;
}

static void controllerOff(void) {
    g_vpadWant = false;
}

// ---------------------------------------------------------------------------
// Engage / teardown
// ---------------------------------------------------------------------------
static bool g_engaged = false;
static bool g_ommForced = false;
// We put the chip in DP Alt Mode and have not exited it. Exiting (0x3B3B) and re-entering
// leaves HPD low (DP_STATUS 800a, black glasses) until the cable renegotiates, so a user
// toggle-off keeps the chip in DP mode and only gives the screen back to the console.
static bool g_chipInDp = false;

#define DP_LINK_UP (DP_STATUS_DP_EN | DP_STATUS_HPD)

static bool linkUp(u16 *dpOut) {
    u16 dp = 0;
    for (int i = 0; i < 3; i++) {
        if (R_SUCCEEDED(bmRead16(REG_DP_STATUS, &dp)) && (dp & DP_LINK_UP) == DP_LINK_UP) break;
        sleepMs(20);
    }
    *dpOut = dp;
    return (dp & DP_LINK_UP) == DP_LINK_UP;
}

// Runs the BM92T36 DP Alt Mode sequence. Returns the DP_STATUS seen afterwards.
static u16 chipEnterDp(int attempt) {
    if (attempt > 1) {
        bmCmd(CMD_STOP_HPD_EXIT_DP);
        sleepMs(300);
    }
    Result rc = bmCmd(CMD_DP_ENTER_MODE);
    sleepMs(50);

    u8 vdm[6] = {0};
    Result rcv = bmRead(REG_INCOMING_VDM, vdm, sizeof(vdm));
    u8 pinC = (vdm[2] & 0x04) ? 0x04 : 0x08;
    u8 cfg[6] = {REG_OUTGOING_VDM, 0x04, 0x06, pinC, 0x00, 0x00};
    Result rcw = bmWrite(cfg, sizeof(cfg));
    sleepMs(30);

    Result rc36 = bmCmd(CMD_DP_CFG_AND_START_HPD);
    sleepMs(50);
    Result rc34 = bmCmd(CMD_START_HPD);
    sleepMs(50);

    u16 dp = 0;
    for (int i = 0; i < 10; i++) {
        bmRead16(REG_DP_STATUS, &dp);
        if ((dp & DP_LINK_UP) == DP_LINK_UP) break;
        sleepMs(100);
    }
    logLine("chip attempt %d: enter rc=0x%x vdm rc=0x%x [%02x %02x %02x %02x %02x %02x] pinC=%02x "
            "cfg rc=0x%x 3636 rc=0x%x 3434 rc=0x%x dp=%04x",
            attempt, rc, rcv, vdm[0], vdm[1], vdm[2], vdm[3], vdm[4], vdm[5], pinC, rcw, rc36, rc34, dp);
    return dp;
}

// Returns false (and leaves the console display untouched) if the glasses never raise HPD.
static bool engage(void) {
    logLine("engage start chipInDp=%d", g_chipInDp ? 1 : 0);
    u16 dp = 0;
    if (g_chipInDp && linkUp(&dp)) {
        logLine("chip already in DP with HPD up (dp=%04x), skipping chip sequence", dp);
    } else {
        g_chipInDp = true;
        for (int attempt = 1; attempt <= 3; attempt++) {
            dp = chipEnterDp(attempt);
            if ((dp & DP_LINK_UP) == DP_LINK_UP) break;
        }
        if ((dp & DP_LINK_UP) != DP_LINK_UP) {
            logLine("HPD not up after 3 attempts (dp=%04x); leaving console display alone", dp);
            bmCmd(CMD_STOP_HPD_EXIT_DP);
            sleepMs(50);
            g_chipInDp = false;
            return false;
        }
    }

    {
        Result rc = ommSetPolicy(2);
        g_ommForced = R_SUCCEEDED(rc);
        logLine("omm policy 2 (docked) rc=0x%x", rc);
    }
    fixUnderscan();
    if (g_joyFlag >= '1') controllerOn();

    g_engaged = true;
    logLine("engage done");
    return true;
}

// Soft off: give the screen back to the console, keep the chip in DP mode with HPD up.
static void disengage(const char *reason) {
    logLine("disengage: %s", reason);
    if (g_ommForced) {
        Result rc = ommSetPolicy(0);
        logLine("omm policy 0 (auto) rc=0x%x", rc);
        g_ommForced = false;
    }
    controllerOff();
    g_engaged = false;
}

// Full teardown: soft off plus exit DP Alt Mode on the chip (unplug, sleep, shutdown, link loss).
static void teardown(const char *reason) {
    disengage(reason);
    if (g_chipInDp) {
        Result rc = bmCmd(CMD_STOP_HPD_EXIT_DP);
        sleepMs(50);
        logLine("3B3B rc=0x%x", rc);
        g_chipInDp = false;
    }
}

// ---------------------------------------------------------------------------
// Sleep / wake (psc)
// ---------------------------------------------------------------------------
#define AR_PSC_MODULE_ID ((PscPmModuleId)126)

static PscPmModule g_psc;
static bool g_pscOk = false;

static void pscSetup(void) {
    // Control switch for isolating sleep/wake problems: create this file to skip psc entirely.
    FILE *nf = fopen(STATE_DIR "/no_psc.flag", "rb");
    if (nf) {
        fclose(nf);
        logLine("no_psc.flag present: psc registration skipped (no sleep handling)");
        return;
    }
    Result rc = pscmInitialize();
    if (R_FAILED(rc)) {
        logLine("pscmInitialize rc=0x%x (no sleep handling)", rc);
        return;
    }
    // No dependencies: the handler makes no blocking calls, so it needs no ordering, and
    // the dependency ids declared here previously coincided with a wake softlock (HOS 23).
    static const u32 deps[1] = {0};
    rc = pscmGetPmModule(&g_psc, AR_PSC_MODULE_ID, deps, 0, true);
    logLine("pscmGetPmModule rc=0x%x", rc);
    g_pscOk = R_SUCCEEDED(rc);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
#define TICK_MS              100
#define BOOT_DELAY_MS        15000  // let home menu come up before the first engage
#define PLUG_SETTLE_MS       1000
#define WAKE_SETTLE_MS       2000
#define CABLE_ON_TICKS       3      // 300 ms present => plugged
#define CABLE_OFF_TICKS      12     // 1.2 s absent => unplugged (single-read glitches seen)
#define LINK_LOST_TICKS      15     // 1.5 s without DP_EN while engaged => re-engage
#define MAX_AUTO_REENGAGE    3
#define CHIP_STABLE_TICKS    10     // status1 steady for 1 s before any engage
#define RETRY_DELAY_MS       3000   // pause after an engage that got no HPD
#define FLAG_POLL_TICKS      5
#define STATUS_TICKS         5
#define PERIODIC_LOG_TICKS   100

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    mkdir("sdmc:/switch", 0777);
    mkdir(STATE_DIR, 0777);
    remove(LOG_PREV);
    rename(LOG_FILE, LOG_PREV);

    logLine("=== sys-ar-glasses v2 start: setsys=0x%x fs=0x%x hos=%u.%u.%u ===",
            g_rcSetsys, g_rcFs,
            HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()));

    Result rc = i2cInitialize();
    logLine("i2cInitialize rc=0x%x", rc);
    if (R_SUCCEEDED(rc)) {
        rc = i2cOpenSession(&g_i2c, I2cDevice_Bm92t30mwv);
        logLine("i2cOpenSession rc=0x%x", rc);
        g_i2cOpen = R_SUCCEEDED(rc);
    }
    if (!g_i2cOpen) {
        while (true) {
            writeStatus(STATE_IDLE, 0, 0);
            sleepMs(1000);
        }
    }

    pscSetup();

    const u64 bootReadyAt = nowMs() + BOOT_DELAY_MS;
    u64 settleUntil = 0;
    bool sleeping = false;
    Result sleepAckRc = 0;
    bool wakeCleanup = false;   // something needs undoing once services are back after sleep
    bool chipAtSleep = false;   // chip was in DP mode when sleep began

    bool cable = false;
    int cableOn = 0, cableOff = 0;
    int linkLost = 0;
    int autoReengage = 0;

    char flagActive = readFlag(ACTIVE_FLAG, '0');
    g_joyFlag = readFlag(JOY_FLAG, '1');
    logLine("flags: active=%c controller=%c", flagActive, g_joyFlag);

    u16 st1 = 0, dp = 0, lastSt1 = 0xFFFF, lastDp = 0xFFFF;
    u16 lastGoodSt1 = 0;
    int stableTicks = 0;   // consecutive ticks with the same nonzero status1
    int noDpTicks = 0;     // consecutive ticks without DP_EN while we think the chip is in DP
    int evState[32];
    u64 evMs[32];
    int evN = 0;
    u64 quietUntil = 0;
    u64 tick = 0;

    while (true) {
        // --- sleep / wake --------------------------------------------------
        // Rule: during the sleep window (states 2-5, and everything until Awake) this loop makes
        // NO fs / i2c / omm / vi call. psc waits for our acknowledge of every state, and
        // services that are already asleep (fs, i2c) block forever, which froze the console
        // when the handler logged to the SD card here. Events are kept in memory and written
        // to the log after wake.
        if (g_pscOk && R_SUCCEEDED(eventWait(&g_psc.event, 0))) {
            PscPmState state;
            u32 flags;
            if (R_SUCCEEDED(pscPmModuleGetRequest(&g_psc, &state, &flags))) {
                if (evN < 32) {
                    evState[evN] = (int)state;
                    evMs[evN] = nowMs();
                    evN++;
                }
                bool windowState = state == PscPmState_ReadySleep || state == PscPmState_ReadySleepCritical ||
                                   state == PscPmState_ReadyShutdown || state == PscPmState_ReadyAwakenCritical;
                if (windowState) {
                    g_vpadQuiet = true;
                    if (state != PscPmState_ReadyAwakenCritical) {
                        wakeCleanup = wakeCleanup || g_engaged || g_ommForced || g_chipInDp;
                        chipAtSleep = chipAtSleep || g_chipInDp;
                        g_engaged = false;
                        sleeping = true;
                    }
                    sleepAckRc = pscPmModuleAcknowledge(&g_psc, state);
                } else if (state == PscPmState_ReadyAwaken) {
                    if (!sleeping) {
                        // omm / vi / i2c are already unusable here (a disengage call at this point
                        // hung the console). Only go quiet; cleanup runs after wake.
                        g_vpadQuiet = true;
                        quietUntil = nowMs() + 3000;
                    }
                    sleepAckRc = pscPmModuleAcknowledge(&g_psc, state);
                } else {  // Awake
                    sleepAckRc = pscPmModuleAcknowledge(&g_psc, state);
                    bool wasSleeping = sleeping;
                    sleeping = false;
                    g_vpadQuiet = false;
                    quietUntil = 0;
                    logLine("awake (wasSleeping=%d lastAckRc=0x%x), %d psc events:", wasSleeping ? 1 : 0, sleepAckRc, evN);
                    for (int i = 0; i < evN; i++) logLine("  psc state=%d at %llu", evState[i], (unsigned long long)evMs[i]);
                    evN = 0;
                    if (wakeCleanup) {
                        disengage("wake cleanup");
                        if (chipAtSleep) {
                            Result rcs = bmCmd(CMD_STOP_HPD_EXIT_DP);
                            logLine("wake: 3B3B rc=0x%x", rcs);
                        }
                        g_chipInDp = false;
                        wakeCleanup = false;
                        chipAtSleep = false;
                    }
                    settleUntil = nowMs() + WAKE_SETTLE_MS;
                    cable = false;
                    cableOn = cableOff = 0;
                    stableTicks = 0;
                    autoReengage = 0;
                }
            }
        }
        if (sleeping || nowMs() < quietUntil) {
            tick++;
            sleepMs(TICK_MS);
            continue;
        }

        // --- user flags ----------------------------------------------------
        if (tick % FLAG_POLL_TICKS == 0) {
            char a = readFlag(ACTIVE_FLAG, flagActive);
            char jm = readFlag(JOY_FLAG, g_joyFlag);
            if (jm != g_joyFlag) {
                logLine("controller flag %c->%c", g_joyFlag, jm);
                g_joyFlag = jm;
                if (g_engaged) {
                    if (jm >= '1') controllerOn();
                    else controllerOff();
                }
            }
            if (a != flagActive) {
                logLine("flags changed: active %c->%c", flagActive, a);
                autoReengage = 0;
            }
            flagActive = a;
        }

        // --- chip state ----------------------------------------------------
        Result r1 = bmRead16(REG_STATUS1, &st1);
        Result r2 = bmRead16(REG_DP_STATUS, &dp);
        if (st1 != lastSt1 || dp != lastDp) {
            logLine("regs status1=%04x rc=0x%x dp=%04x rc=0x%x", st1, r1, dp, r2);
            lastSt1 = st1;
            lastDp = dp;
        }

        // status1 reads 0000 for a single tick every few seconds without a real unplug.
        // Ignore those for stability; any other change means the chip renegotiated.
        if (R_SUCCEEDED(r1) && st1 != 0) {
            if (st1 == lastGoodSt1) {
                if (stableTicks < 1000) stableTicks++;
            } else {
                if (lastGoodSt1 != 0) logLine("chip state changed %04x -> %04x, waiting for it to settle", lastGoodSt1, st1);
                lastGoodSt1 = st1;
                stableTicks = 0;
            }
        }
        // The chip can drop out of DP mode on its own (it renegotiates a few seconds after
        // the console stops sending video). Forget our stale belief so the next engage
        // starts from scratch instead of skipping the chip sequence.
        if (g_chipInDp && !g_engaged && !(dp & DP_STATUS_DP_EN) && dp != 0) {
            if (++noDpTicks >= 5) {
                logLine("chip left DP mode on its own (dp=%04x)", dp);
                g_chipInDp = false;
                noDpTicks = 0;
            }
        } else {
            noDpTicks = 0;
        }

        bool present = R_SUCCEEDED(r1) && (st1 & STATUS1_INSERT);
        if (present) {
            cableOff = 0;
            if (!cable && ++cableOn >= CABLE_ON_TICKS) {
                cable = true;
                autoReengage = 0;
                settleUntil = nowMs() + PLUG_SETTLE_MS;
                logLine("cable plugged");
            }
        } else {
            cableOn = 0;
            if (cable && ++cableOff >= CABLE_OFF_TICKS) {
                cable = false;
                logLine("cable unplugged");
                if (g_engaged || g_chipInDp) teardown("unplug");
            }
        }

        // --- link supervision ----------------------------------------------
        if (g_engaged && cable) {
            if (dp & DP_STATUS_DP_EN) {
                linkLost = 0;
            } else if (++linkLost >= LINK_LOST_TICKS) {
                linkLost = 0;
                teardown("link lost");
                if (++autoReengage > MAX_AUTO_REENGAGE) {
                    logLine("giving up auto re-engage until flag toggle or replug");
                }
                settleUntil = nowMs() + PLUG_SETTLE_MS;
            }
        }

        // --- desired state -------------------------------------------------
        u64 now = nowMs();
        bool want = flagActive == '1' && cable && now >= bootReadyAt && now >= settleUntil &&
                    stableTicks >= CHIP_STABLE_TICKS && autoReengage <= MAX_AUTO_REENGAGE;
        if (want && !g_engaged) {
            writeStatus(STATE_NEGOTIATING, st1, dp);
            if (!engage()) {
                if (++autoReengage > MAX_AUTO_REENGAGE) {
                    logLine("giving up until the overlay toggle is flipped or the cable replugged");
                }
                settleUntil = nowMs() + RETRY_DELAY_MS;
                stableTicks = 0;
            }
            linkLost = 0;
        } else if (g_engaged && flagActive != '1') {
            disengage("toggled off");
        }

        // --- telemetry -----------------------------------------------------
        if (tick % STATUS_TICKS == 0) {
            u32 state = g_engaged ? STATE_ACTIVE : (want ? STATE_NEGOTIATING : STATE_IDLE);
            writeStatus(state, st1, dp);
        }

        if (tick % PERIODIC_LOG_TICKS == 0) {
            logLine("alive: cable=%d engaged=%d vpad=%d active=%c joycon=%c status1=%04x dp=%04x",
                    cable ? 1 : 0, g_engaged ? 1 : 0, g_vpadAttached ? 1 : 0, flagActive, g_joyFlag, st1, dp);
        }

        tick++;
        sleepMs(TICK_MS);
    }
}
