// Switchlens: Tesla/Ultrahand overlay for the sys-ar-glasses sysmodule.
// Talks to the sysmodule only through small files in sdmc:/switch/ar_glasses/.
#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define OVL_VERSION     "1.0.0"
#define STATE_DIR       "sdmc:/switch/ar_glasses"
#define ACTIVE_FLAG     STATE_DIR "/active.flag"       // '1' AR on, '0' AR off
#define CONTROLLER_FLAG STATE_DIR "/controller.flag"   // '1' Joy-Con passthrough (default), '0' off
#define STATUS_FILE     STATE_DIR "/state.bin"

#pragma pack(push, 1)
struct ArGlassesStatus {
    u32 magic;       // 'ARGS'
    u32 version;
    u32 state;       // 0 idle, 1 negotiating, 2 active
    u16 status1;     // BM92T36 STATUS1, bit7 = cable inserted
    u16 dp_status;
    u16 alert_status;
    u16 flags;       // bit0: Joy-Con passthrough (virtual Pro Controller) attached
    u64 heartbeat;
};
#pragma pack(pop)

static bool g_fsMounted = false;
static u32  g_frame     = 0;

// Telemetry
static bool g_hbSeen    = false;   // heartbeat has changed at least once
static u64  g_lastHb    = 0;
static u32  g_hbFrame   = 0;       // frame of the last heartbeat change
static u32  g_state     = 0;
static u16  g_status1   = 0;
static u16  g_flags     = 0;

// User intent (mirrors the flag files)
static bool g_arOn      = false;
static bool g_ctlOn     = true;

static const tsl::Color kGreen (0x3, 0xD, 0x6, 0xF);
static const tsl::Color kYellow(0xF, 0xB, 0x2, 0xF);
static const tsl::Color kRed   (0xF, 0x4, 0x4, 0xF);
static const tsl::Color kBlue  (0x4, 0x9, 0xF, 0xF);
static const tsl::Color kGrey  (0x7, 0x7, 0x8, 0xF);
static const tsl::Color kCard  (0x2, 0x2, 0x3, 0xF);
static const tsl::Color kLine  (0x4, 0x4, 0x5, 0xF);
static const tsl::Color kWhite (0xF, 0xF, 0xF, 0xF);
static const tsl::Color kDim   (0x9, 0x9, 0xA, 0xF);

static bool readFlag(const char *path, bool def) {
    if (!g_fsMounted) return def;
    FILE *fp = fopen(path, "rb");
    if (!fp) return def;
    int c = fgetc(fp);
    fclose(fp);
    if (c == '1') return true;
    if (c == '0') return false;
    return def;
}

static void writeFlag(const char *path, bool on) {
    if (!g_fsMounted) return;
    mkdir(STATE_DIR, 0777);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fputc(on ? '1' : '0', fp);
    fclose(fp);
}

static void readStatus() {
    if (!g_fsMounted) return;
    FILE *fp = fopen(STATUS_FILE, "rb");
    if (!fp) return;
    ArGlassesStatus st;
    if (fread(&st, sizeof(st), 1, fp) == 1 && st.magic == 0x53475241) {
        if (g_lastHb != 0 && st.heartbeat != g_lastHb) {
            g_hbSeen = true;
            g_hbFrame = g_frame;
        }
        g_lastHb = st.heartbeat;
        g_state = st.state;
        g_status1 = st.status1;
        g_flags = st.flags;
    }
    fclose(fp);
}

struct Headline {
    const char *title;
    const char *sub;
    tsl::Color color;
};

static bool sysmoduleAlive() { return g_hbSeen && (g_frame - g_hbFrame) < 300; }  // ~5 s without a heartbeat
static bool cablePresent()   { return (g_status1 & (1 << 7)) != 0; }

static Headline headline() {
    if (!sysmoduleAlive()) {
        if (!g_hbSeen && g_frame < 150) return {"Checking...", "Contacting the background service", kGrey};
        return {"Service not running", "Reboot with Atmosphere (boot2 flag set?)", kRed};
    }
    if (g_state == 2) return {"AR is ON", "The picture is on your glasses", kGreen};
    if (g_state == 1) return {"Connecting...", "Negotiating with the glasses", kYellow};
    if (!cablePresent()) {
        if (g_arOn) return {"Waiting for glasses", "AR starts when you plug them in", kYellow};
        return {"No glasses connected", "Plug them into the USB-C port", kGrey};
    }
    if (g_arOn) return {"Starting...", "Glasses detected", kYellow};
    return {"Glasses connected", "AR is off. Switch it on below", kBlue};
}

class GuiGlasses : public tsl::Gui {
public:
    GuiGlasses() {
        g_arOn  = readFlag(ACTIVE_FLAG, false);
        g_ctlOn = readFlag(CONTROLLER_FLAG, true);
        readStatus();
    }

    virtual tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("Switchlens", "v" OVL_VERSION "  -  USB-C display glasses");
        auto *list = new tsl::elm::List();

        // ---- status card ---------------------------------------------------
        list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
            (void)h;
            Headline hl = headline();
            r->drawRect(x, y + 4, w, 150, kCard);
            r->drawCircle(x + 26, y + 36, 10, true, hl.color);
            r->drawString(hl.title, false, x + 48, y + 44, 26, kWhite);
            r->drawString(hl.sub, false, x + 20, y + 72, 16, kDim);
            r->drawRect(x + 14, y + 88, w - 28, 1, kLine);

            // USB-C row
            r->drawString("USB-C", false, x + 20, y + 118, 19, kDim);
            if (!sysmoduleAlive())   r->drawString("unknown",       false, x + 150, y + 118, 19, kGrey);
            else if (cablePresent()) r->drawString("Connected",     false, x + 150, y + 118, 19, kGreen);
            else                     r->drawString("Not connected", false, x + 150, y + 118, 19, kGrey);

            // Joy-Con row
            r->drawString("Joy-Cons", false, x + 20, y + 146, 19, kDim);
            if (!g_ctlOn)                  r->drawString("Passthrough off",          false, x + 150, y + 146, 19, kGrey);
            else if (g_flags & 1)          r->drawString("Pro Controller active",    false, x + 150, y + 146, 19, kGreen);
            else if (g_state == 2)         r->drawString("Starting...",              false, x + 150, y + 146, 19, kYellow);
            else                           r->drawString("Ready, starts with AR",    false, x + 150, y + 146, 19, kDim);
        }), 166);

        // ---- controls ------------------------------------------------------
        list->addItem(new tsl::elm::CategoryHeader("Controls"));

        auto *arToggle = new tsl::elm::ToggleListItem("AR Display", g_arOn);
        arToggle->setStateChangedListener([](bool on) {
            g_arOn = on;
            writeFlag(ACTIVE_FLAG, on);
        });
        list->addItem(arToggle);

        auto *ctlToggle = new tsl::elm::ToggleListItem("Joy-Con Support", g_ctlOn);
        ctlToggle->setStateChangedListener([](bool on) {
            g_ctlOn = on;
            writeFlag(CONTROLLER_FLAG, on);
        });
        list->addItem(ctlToggle);

        // ---- help ----------------------------------------------------------
        list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
            (void)w;
            (void)h;
            r->drawString("AR Display sends the console picture", false, x + 6, y + 24, 15, kDim);
            r->drawString("to the glasses. Unplugging gives it back.", false, x + 6, y + 44, 15, kDim);
            r->drawString("Joy-Con Support makes Joy-Cons on the rails", false, x + 6, y + 74, 15, kDim);
            r->drawString("work as a wireless Pro Controller while AR", false, x + 6, y + 94, 15, kDim);
            r->drawString("is on (needed for Mario Kart 8 Deluxe).", false, x + 6, y + 114, 15, kDim);
        }), 130);

        frame->setContent(list);
        return frame;
    }

    virtual void update() override {
        g_frame++;
        if (g_frame % 15 == 0) readStatus();
    }
};

class OverlayGlasses : public tsl::Overlay {
public:
    virtual void initServices() override {
        smInitialize();
        if (R_SUCCEEDED(fsdevMountSdmc())) g_fsMounted = true;
    }

    virtual void exitServices() override {
        if (g_fsMounted) {
            fsdevUnmountDevice("sdmc");
            g_fsMounted = false;
        }
        smExit();
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<GuiGlasses>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<OverlayGlasses>(argc, argv);
}
