#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Gdiplus;

// ============================================================================
// FORMAL ATM FINITE STATE MACHINE DEFINITION: M = (Q, Sigma, delta, q0, F)
// ============================================================================
enum AtmState {
    ST_IDLE = 0,     // q0: Accepting / Resting State
    ST_CARD_IN = 1,  // q1: Waiting for PIN
    ST_AUTH = 2,     // q2: Authenticated Menu
    ST_SELECT = 3,   // q3: Verifying Withdrawal Funds
    ST_DISPENSE = 4, // q4: Cash Dispensing
    ST_LOCKED = 5    // q5: Trap / Dead State (3 failed PINs)
};

struct TransitionEdge {
    AtmState from;
    AtmState to;
    std::wstring label;
};

struct UIButton {
    int id;
    std::wstring text;
    RectF bounds;
    Color baseColor;
    Color hoverColor;
    bool hovered = false;
};

// Global Application State
ULONG_PTR g_gdiplusToken = 0;
AtmState g_currentState = ST_IDLE;
AtmState g_lastFrom = ST_IDLE;
AtmState g_lastTo = ST_IDLE;
bool g_hasLastEdge = false;

int g_balance = 15000;
const std::wstring g_correctPin = L"1234";
std::wstring g_pinBuffer = L"";
int g_pinAttempts = 0;

std::wstring g_screenTitle = L"WELCOME TO NEXUS BANK ATM";
std::wstring g_screenLine1 = L"Current State: q0 (IDLE - Accepting)";
std::wstring g_screenLine2 = L"Click [Insert Card] to begin a session.";
std::wstring g_statusBarText = L"Ready — Formal FSM initialized in state q0 (IDLE).";
Color g_statusColor(255, 14, 165, 233);

std::vector<std::wstring> g_traceLog = {
    L"INIT : Machine powered on in state q0 (IDLE)."
};

std::vector<TransitionEdge> g_edges = {
    {ST_IDLE, ST_CARD_IN, L"insert_card"},
    {ST_CARD_IN, ST_AUTH, L"valid_pin"},
    {ST_CARD_IN, ST_CARD_IN, L"bad_pin(<3)"},
    {ST_CARD_IN, ST_LOCKED, L"3rd_fail"},
    {ST_CARD_IN, ST_IDLE, L"cancel"},
    {ST_AUTH, ST_AUTH, L"check_bal"},
    {ST_AUTH, ST_SELECT, L"withdraw"},
    {ST_AUTH, ST_IDLE, L"eject"},
    {ST_SELECT, ST_DISPENSE, L"ok_funds"},
    {ST_SELECT, ST_AUTH, L"low_bal"},
    {ST_DISPENSE, ST_IDLE, L"take_cash"},
    {ST_LOCKED, ST_IDLE, L"admin_reset"}
};

std::vector<UIButton> g_buttons;

// Button IDs
enum BtnID {
    BTN_INSERT = 101,
    BTN_COLLECT,
    BTN_CANCEL,
    BTN_BAL,
    BTN_W2000,
    BTN_W20000,
    BTN_ADMIN,
    BTN_CLR,
    BTN_ENTER,
    BTN_KEY_0 = 200 // 200..209 for digits 0..9
};

// ============================================================================
// GDI+ DRAWING HELPERS
// ============================================================================
void AddRoundedRectPath(GraphicsPath& path, RectF r, float radius) {
    float d = radius * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void DrawGlassCard(Graphics& g, RectF rect, float radius, Color fillCol, Color borderCol, float borderW = 1.5f) {
    GraphicsPath path;
    AddRoundedRectPath(path, rect, radius);
    SolidBrush brush(fillCol);
    Pen pen(borderCol, borderW);
    g.FillPath(&brush, &path);
    g.DrawPath(&pen, &path);
}

void DrawArrowHead(Graphics& g, Pen& pen, PointF tip, float angleRad, float size = 10.0f) {
    PointF p1(tip.X - size * cosf(angleRad - 0.42f), tip.Y - size * sinf(angleRad - 0.42f));
    PointF p2(tip.X - size * cosf(angleRad + 0.42f), tip.Y - size * sinf(angleRad + 0.42f));
    SolidBrush b(Color(255, 0, 0, 0));
    Color c;
    pen.GetColor(&c);
    b.SetColor(c);
    PointF pts[3] = {tip, p1, p2};
    g.FillPolygon(&b, pts, 3);
}

std::wstring StateName(AtmState s) {
    switch (s) {
        case ST_IDLE: return L"IDLE";
        case ST_CARD_IN: return L"CARD_IN";
        case ST_AUTH: return L"AUTH";
        case ST_SELECT: return L"SELECT";
        case ST_DISPENSE: return L"DISPENSE";
        case ST_LOCKED: return L"LOCKED";
    }
    return L"?";
}

void PushLog(AtmState prev, const std::wstring& sym, AtmState next, const std::wstring& note) {
    std::wstring line = L"δ(" + StateName(prev) + L", " + sym + L") → " + StateName(next) + L"   |  " + note;
    g_traceLog.push_back(line);
    if (g_traceLog.size() > 7) {
        g_traceLog.erase(g_traceLog.begin());
    }
    g_lastFrom = prev;
    g_lastTo = next;
    g_hasLastEdge = true;
}

std::wstring FormatRupees(int val) {
    std::wstring s = std::to_wstring(val);
    if (s.length() > 3) {
        s.insert(s.length() - 3, L",");
    }
    return L"₹" + s;
}

// ============================================================================
// ATM FSM TRANSITION ENGINE
// ============================================================================
void HandleButtonClick(int id) {
    AtmState prev = g_currentState;

    if (id >= BTN_KEY_0 && id <= BTN_KEY_0 + 9) {
        int digit = id - BTN_KEY_0;
        if (g_currentState != ST_CARD_IN) {
            g_statusBarText = L"Keypad active only in state q1 (CARD_IN). Click [Insert Card] first.";
            g_statusColor = Color(255, 245, 158, 11);
            return;
        }
        if (g_pinBuffer.size() < 4) {
            g_pinBuffer += std::to_wstring(digit);
            std::wstring masked(g_pinBuffer.size(), L'*');
            g_screenTitle = L"ENTER 4-DIGIT PIN";
            g_screenLine1 = L"State: q1 (PIN_WAIT)  |  Attempts: " + std::to_wstring(g_pinAttempts) + L"/3";
            g_screenLine2 = L"PIN Input: [ " + masked + L" ]   (Press ENTER)";
        }
        return;
    }

    switch (id) {
        case BTN_INSERT:
            if (g_currentState == ST_IDLE) {
                g_currentState = ST_CARD_IN;
                g_pinBuffer.clear();
                g_pinAttempts = 0;
                PushLog(prev, L"insert_card", ST_CARD_IN, L"Card authenticated by reader");
                g_screenTitle = L"CARD DETECTED — ENTER PIN";
                g_screenLine1 = L"State: q1 (PIN_WAIT)  |  Default PIN: 1234";
                g_screenLine2 = L"Use keypad to enter PIN and press ENTER.";
                g_statusBarText = L"Transition: δ(IDLE, insert_card) = CARD_IN";
                g_statusColor = Color(255, 16, 185, 129);
            } else {
                g_statusBarText = L"Input 'insert_card' ignored in state " + StateName(g_currentState) + L".";
                g_statusColor = Color(255, 245, 158, 11);
            }
            break;

        case BTN_CLR:
            if (g_currentState == ST_CARD_IN) {
                g_pinBuffer.clear();
                g_screenLine2 = L"PIN Input: [      ]   (Cleared)";
            }
            break;

        case BTN_ENTER:
            if (g_currentState != ST_CARD_IN) {
                g_statusBarText = L"Not waiting for PIN entry right now.";
                g_statusColor = Color(255, 245, 158, 11);
                break;
            }
            if (g_pinBuffer == g_correctPin) {
                g_currentState = ST_AUTH;
                g_pinBuffer.clear();
                g_pinAttempts = 0;
                PushLog(prev, L"valid_pin", ST_AUTH, L"PIN matched → Menu unlocked");
                g_screenTitle = L"AUTHENTICATED — SELECT SERVICE";
                g_screenLine1 = L"State: q2 (MENU)  |  Balance: " + FormatRupees(g_balance);
                g_screenLine2 = L"Choose [Check Balance] or [Withdraw].";
                g_statusBarText = L"Transition: δ(CARD_IN, valid_pin) = AUTH";
                g_statusColor = Color(255, 16, 185, 129);
            } else {
                g_pinAttempts++;
                g_pinBuffer.clear();
                if (g_pinAttempts >= 3) {
                    g_currentState = ST_LOCKED;
                    PushLog(prev, L"3rd_fail", ST_LOCKED, L"3 invalid PINs → Trap state q5!");
                    g_screenTitle = L"CARD LOCKED (TRAP STATE q5)";
                    g_screenLine1 = L"State: q5 (LOCKED) — Card retained for security.";
                    g_screenLine2 = L"Click [Bank Admin Reset] to unlock terminal.";
                    g_statusBarText = L"Transition: δ(CARD_IN, 3rd_fail) = LOCKED (Trap State)";
                    g_statusColor = Color(255, 244, 63, 94);
                } else {
                    PushLog(prev, L"bad_pin(<3)", ST_CARD_IN, L"Wrong PIN (" + std::to_wstring(g_pinAttempts) + L"/3)");
                    g_screenTitle = L"INCORRECT PIN — TRY AGAIN";
                    g_screenLine1 = L"State: q1 (PIN_WAIT)  |  Attempts: " + std::to_wstring(g_pinAttempts) + L"/3";
                    g_screenLine2 = L"Hint: Default PIN is 1234. Enter & press ENTER.";
                    g_statusBarText = L"Self-loop: δ(CARD_IN, bad_pin) = CARD_IN";
                    g_statusColor = Color(255, 245, 158, 11);
                }
            }
            break;

        case BTN_BAL:
            if (g_currentState == ST_AUTH) {
                PushLog(prev, L"check_bal", ST_AUTH, L"Balance queried: " + FormatRupees(g_balance));
                g_screenTitle = L"ACCOUNT BALANCE INQUIRY";
                g_screenLine1 = L"State: q2 (MENU)  |  Available: " + FormatRupees(g_balance);
                g_screenLine2 = L"Select Withdraw or Eject Card.";
                g_statusBarText = L"Self-loop: δ(AUTH, check_bal) = AUTH";
                g_statusColor = Color(255, 14, 165, 233);
            } else {
                g_statusBarText = L"Authenticate with PIN first to reach state q2 (AUTH).";
                g_statusColor = Color(255, 245, 158, 11);
            }
            break;

        case BTN_W2000:
        case BTN_W20000: {
            int amt = (id == BTN_W2000) ? 2000 : 20000;
            if (g_currentState != ST_AUTH) {
                g_statusBarText = L"Authenticate with PIN first to reach state q2 (AUTH).";
                g_statusColor = Color(255, 245, 158, 11);
                break;
            }
            PushLog(ST_AUTH, L"withdraw(" + FormatRupees(amt) + L")", ST_SELECT, L"Validating account funds...");
            if (amt <= g_balance) {
                g_balance -= amt;
                g_currentState = ST_DISPENSE;
                PushLog(ST_SELECT, L"ok_funds", ST_DISPENSE, L"Dispensing " + FormatRupees(amt));
                g_screenTitle = L"DISPENSING CASH...";
                g_screenLine1 = L"State: q4 (DISPENSE)  |  Dispensed: " + FormatRupees(amt);
                g_screenLine2 = L"New Bal: " + FormatRupees(g_balance) + L" — Click [Collect Cash].";
                g_statusBarText = L"Transition: AUTH → SELECT → DISPENSE (" + FormatRupees(amt) + L")";
                g_statusColor = Color(255, 16, 185, 129);
            } else {
                g_currentState = ST_AUTH;
                PushLog(ST_SELECT, L"low_bal", ST_AUTH, L"Insufficient funds for " + FormatRupees(amt));
                g_screenTitle = L"INSUFFICIENT FUNDS";
                g_screenLine1 = L"State: q2 (MENU)  |  Requested " + FormatRupees(amt);
                g_screenLine2 = L"Available Balance is only " + FormatRupees(g_balance) + L".";
                g_statusBarText = L"Transition: δ(SELECT, low_bal) = AUTH";
                g_statusColor = Color(255, 244, 63, 94);
            }
            break;
        }

        case BTN_COLLECT:
            if (g_currentState == ST_DISPENSE) {
                g_currentState = ST_IDLE;
                PushLog(prev, L"take_cash", ST_IDLE, L"Cash taken & card returned");
                g_screenTitle = L"THANK YOU — SESSION COMPLETE";
                g_screenLine1 = L"State: q0 (IDLE - Accepting State)";
                g_screenLine2 = L"Transaction finished. Insert card anytime.";
                g_statusBarText = L"Transition: δ(DISPENSE, take_cash) = IDLE (Accepting State)";
                g_statusColor = Color(255, 16, 185, 129);
            } else {
                g_statusBarText = L"No cash waiting in dispenser (only valid in q4 DISPENSE).";
                g_statusColor = Color(255, 245, 158, 11);
            }
            break;

        case BTN_CANCEL:
            if (g_currentState == ST_CARD_IN || g_currentState == ST_AUTH || g_currentState == ST_SELECT) {
                std::wstring sym = (prev == ST_CARD_IN) ? L"cancel" : L"eject";
                g_currentState = ST_IDLE;
                g_pinBuffer.clear();
                PushLog(prev, sym, ST_IDLE, L"Session cancelled & card ejected");
                g_screenTitle = L"CARD EJECTED";
                g_screenLine1 = L"State: q0 (IDLE)";
                g_screenLine2 = L"Session aborted by user. Insert card to start.";
                g_statusBarText = L"Transition: δ(" + StateName(prev) + L", " + sym + L") = IDLE";
                g_statusColor = Color(255, 14, 165, 233);
            } else {
                g_statusBarText = L"Cannot eject in current state " + StateName(g_currentState) + L".";
                g_statusColor = Color(255, 245, 158, 11);
            }
            break;

        case BTN_ADMIN:
            if (g_currentState == ST_LOCKED) {
                g_currentState = ST_IDLE;
                g_pinAttempts = 0;
                g_pinBuffer.clear();
                PushLog(prev, L"admin_reset", ST_IDLE, L"Bank technician unlocked terminal");
                g_screenTitle = L"TERMINAL UNLOCKED BY ADMIN";
                g_screenLine1 = L"State: q0 (IDLE)";
                g_screenLine2 = L"Trap state q5 cleared. Ready for customer.";
                g_statusBarText = L"Transition: δ(LOCKED, admin_reset) = IDLE";
                g_statusColor = Color(255, 16, 185, 129);
            } else {
                g_statusBarText = L"Admin reset is only needed when terminal is in q5 (LOCKED).";
                g_statusColor = Color(255, 245, 158, 11);
            }
            break;
    }
}

// ============================================================================
// LAYOUT & RENDERING
// ============================================================================
void RebuildLayout(int width, int height) {
    g_buttons.clear();
    float pad = 20.0f;
    float headerH = 54.0f;
    float statusH = 40.0f;
    float rightW = 410.0f;
    float rightX = width - pad - rightW;
    float topY = pad + headerH + 8.0f;
    float contentH = height - topY - statusH - pad - 10.0f;

    float innerX = rightX + 16.0f;
    float innerW = rightW - 32.0f;
    float curY = topY + 168.0f;

    // Row 1: Card / Cash / Eject
    float b3W = (innerW - 16.0f) / 3.0f;
    g_buttons.push_back({BTN_INSERT, L"Insert Card", RectF(innerX, curY, b3W, 36), Color(255, 14, 165, 233), Color(255, 2, 132, 199)});
    g_buttons.push_back({BTN_COLLECT, L"Collect Cash", RectF(innerX + b3W + 8, curY, b3W, 36), Color(255, 16, 185, 129), Color(255, 5, 150, 105)});
    g_buttons.push_back({BTN_CANCEL, L"Eject / Cancel", RectF(innerX + (b3W + 8) * 2, curY, b3W, 36), Color(255, 244, 63, 94), Color(255, 225, 29, 72)});
    curY += 58.0f;

    // Row 2: Service Actions
    g_buttons.push_back({BTN_BAL, L"Check Balance", RectF(innerX, curY, b3W, 34), Color(255, 30, 41, 59), Color(255, 51, 65, 85)});
    g_buttons.push_back({BTN_W2000, L"Withdraw ₹2,000", RectF(innerX + b3W + 8, curY, b3W, 34), Color(255, 30, 41, 59), Color(255, 51, 65, 85)});
    g_buttons.push_back({BTN_W20000, L"Withdraw ₹20,000", RectF(innerX + (b3W + 8) * 2, curY, b3W, 34), Color(255, 30, 41, 59), Color(255, 51, 65, 85)});
    curY += 56.0f;

    // Keypad 4x3 Grid
    float adminH = 36.0f;
    float bottomLimit = topY + contentH - 16.0f - adminH - 10.0f;
    float padH = (std::max)(140.0f, bottomLimit - curY);
    float keyH = (padH - 24.0f) / 4.0f;

    const wchar_t* labels[4][3] = {
        {L"1", L"2", L"3"},
        {L"4", L"5", L"6"},
        {L"7", L"8", L"9"},
        {L"CLR", L"0", L"ENTER"}
    };
    int ids[4][3] = {
        {BTN_KEY_0 + 1, BTN_KEY_0 + 2, BTN_KEY_0 + 3},
        {BTN_KEY_0 + 4, BTN_KEY_0 + 5, BTN_KEY_0 + 6},
        {BTN_KEY_0 + 7, BTN_KEY_0 + 8, BTN_KEY_0 + 9},
        {BTN_CLR, BTN_KEY_0 + 0, BTN_ENTER}
    };

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
            RectF kr(innerX + c * (b3W + 8), curY + r * (keyH + 8), b3W, keyH);
            Color base(255, 15, 23, 42);
            Color hov(255, 30, 41, 59);
            if (ids[r][c] == BTN_ENTER) {
                base = Color(255, 16, 185, 129);
                hov = Color(255, 5, 150, 105);
            } else if (ids[r][c] == BTN_CLR) {
                base = Color(255, 217, 119, 6);
                hov = Color(255, 180, 83, 9);
            }
            g_buttons.push_back({ids[r][c], labels[r][c], kr, base, hov});
        }
    }

    // Admin Reset Button at Bottom of Right Card
    g_buttons.push_back({
        BTN_ADMIN,
        L"Bank Admin Reset (Unlock Trap State q5)",
        RectF(innerX, topY + contentH - 16.0f - adminH, innerW, adminH),
        Color(255, 124, 58, 237),
        Color(255, 109, 40, 217)
    });
}

void RenderScene(Graphics& g, int width, int height) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Deep Obsidian Backdrop
    SolidBrush bgBrush(Color(255, 7, 11, 20));
    g.FillRectangle(&bgBrush, 0, 0, width, height);

    FontFamily fontUI(L"Segoe UI");
    FontFamily fontMono(L"Consolas");
    Font fTitle(&fontUI, 17, FontStyleBold, UnitPixel);
    Font fCardTitle(&fontUI, 13, FontStyleBold, UnitPixel);
    Font fSub(&fontUI, 10, FontStyleRegular, UnitPixel);
    Font fMono(&fontMono, 11, FontStyleRegular, UnitPixel);
    Font fMonoBold(&fontMono, 11, FontStyleBold, UnitPixel);
    Font fNode(&fontUI, 10, FontStyleBold, UnitPixel);
    Font fBtn(&fontUI, 11, FontStyleBold, UnitPixel);

    SolidBrush whiteBrush(Color(255, 248, 250, 252));
    SolidBrush mutedBrush(Color(255, 148, 163, 184));
    SolidBrush cyanBrush(Color(255, 56, 189, 248));
    SolidBrush greenBrush(Color(255, 16, 185, 129));

    float pad = 20.0f;

    // Header Bar
    g.DrawString(L"Σ  ATM TRANSACTION FINITE STATE MACHINE STUDIO", -1, &fTitle, PointF(pad, pad + 6), &whiteBrush);
    RectF badgeRect(width - pad - 220.0f, pad + 4, 220.0f, 30.0f);
    DrawGlassCard(g, badgeRect, 12.0f, Color(255, 22, 33, 56), Color(255, 56, 189, 248), 1.0f);
    StringFormat centerFmt;
    centerFmt.SetAlignment(StringAlignmentCenter);
    centerFmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(L"M = (Q, Σ, δ, q₀, F)  |  C++ GDI+", -1, &fMonoBold, badgeRect, &centerFmt, &cyanBrush);

    float topY = pad + 54.0f;
    float statusH = 40.0f;
    float rightW = 410.0f;
    float leftW = width - pad * 2 - rightW - 14.0f;
    float contentH = height - topY - statusH - pad - 10.0f;
    float logH = 165.0f;
    float diagH = contentH - logH - 12.0f;

    // ------------------------------------------------------------------------
    // LEFT TOP CARD: LIVE STATE TRANSITION DIAGRAM
    // ------------------------------------------------------------------------
    RectF diagCard(pad, topY, leftW, diagH);
    DrawGlassCard(g, diagCard, 18.0f, Color(255, 17, 25, 44), Color(255, 38, 53, 84));
    g.DrawString(L"Live Automaton State Diagram", -1, &fCardTitle, PointF(pad + 16, topY + 12), &whiteBrush);
    g.DrawString(L"Double Circle = Accepting State (q0)   |   Green Glow = Active State & Last δ", -1, &fSub, PointF(pad + 16, topY + 32), &mutedBrush);

    RectF canvasRect(pad + 14, topY + 52, leftW - 28, diagH - 66);
    DrawGlassCard(g, canvasRect, 14.0f, Color(255, 9, 15, 29), Color(255, 30, 41, 59));

    // Subtle dot grid inside diagram well
    SolidBrush dotBrush(Color(255, 24, 36, 60));
    for (float gx = canvasRect.X + 24; gx < canvasRect.X + canvasRect.Width - 16; gx += 36) {
        for (float gy = canvasRect.Y + 24; gy < canvasRect.Y + canvasRect.Height - 16; gy += 36) {
            g.FillEllipse(&dotBrush, gx - 1.0f, gy - 1.0f, 2.0f, 2.0f);
        }
    }

    // Node Coordinates (normalized inside canvasRect)
    PointF pts[6] = {
        PointF(canvasRect.X + canvasRect.Width * 0.14f, canvasRect.Y + canvasRect.Height * 0.50f), // q0 IDLE
        PointF(canvasRect.X + canvasRect.Width * 0.39f, canvasRect.Y + canvasRect.Height * 0.24f), // q1 CARD_IN
        PointF(canvasRect.X + canvasRect.Width * 0.65f, canvasRect.Y + canvasRect.Height * 0.24f), // q2 AUTH
        PointF(canvasRect.X + canvasRect.Width * 0.87f, canvasRect.Y + canvasRect.Height * 0.50f), // q3 SELECT
        PointF(canvasRect.X + canvasRect.Width * 0.65f, canvasRect.Y + canvasRect.Height * 0.78f), // q4 DISPENSE
        PointF(canvasRect.X + canvasRect.Width * 0.39f, canvasRect.Y + canvasRect.Height * 0.78f)  // q5 LOCKED
    };
    const wchar_t* nodeTitles[6] = {
        L"q0\nIDLE",
        L"q1\nPIN_WAIT",
        L"q2\nMENU",
        L"q3\nVERIFY",
        L"q4\nDISPENSE",
        L"q5\nLOCKED"
    };
    float nodeR = 35.0f;

    // Draw Edges
    for (const auto& e : g_edges) {
        bool isActiveEdge = g_hasLastEdge && (e.from == g_lastFrom && e.to == g_lastTo);
        Color edgeCol = isActiveEdge ? Color(255, 16, 185, 129) : Color(255, 71, 85, 105);
        Color textCol = isActiveEdge ? Color(255, 16, 185, 129) : Color(255, 56, 189, 248);
        Pen edgePen(edgeCol, isActiveEdge ? 2.8f : 1.7f);
        SolidBrush lblBrush(textCol);

        PointF p1 = pts[e.from];
        PointF p2 = pts[e.to];

        if (e.from == e.to) {
            RectF arcRect(p1.X - 24, p1.Y - 76, 48, 52);
            g.DrawArc(&edgePen, arcRect, 150.0f, 240.0f);
            RectF lr(p1.X - 55, p1.Y - 94, 110, 18);
            g.DrawString(e.label.c_str(), -1, &fMonoBold, lr, &centerFmt, &lblBrush);
        } else {
            float dx = p2.X - p1.X;
            float dy = p2.Y - p1.Y;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist < 1.0f) dist = 1.0f;
            float ux = dx / dist;
            float uy = dy / dist;

            // Check if reverse edge exists so arrows don't overlap
            bool hasReverse = false;
            for (const auto& rev : g_edges) {
                if (rev.from == e.to && rev.to == e.from) {
                    hasReverse = true;
                    break;
                }
            }
            float offset = hasReverse ? 14.0f : 0.0f;
            float nx = -uy * offset;
            float ny = ux * offset;

            PointF start(p1.X + ux * nodeR + nx, p1.Y + uy * nodeR + ny);
            PointF end(p2.X - ux * (nodeR + 4.0f) + nx, p2.Y - uy * (nodeR + 4.0f) + ny);

            g.DrawLine(&edgePen, start, end);
            DrawArrowHead(g, edgePen, end, atan2f(end.Y - start.Y, end.X - start.X), 9.0f);

            PointF mid((start.X + end.X) * 0.5f - uy * 13.0f, (start.Y + end.Y) * 0.5f + ux * 13.0f);
            RectF pill(mid.X - 46, mid.Y - 9, 92, 18);
            SolidBrush pillBg(Color(230, 9, 15, 29));
            g.FillRectangle(&pillBg, pill);
            g.DrawString(e.label.c_str(), -1, &fMonoBold, pill, &centerFmt, &lblBrush);
        }
    }

    // Draw State Nodes
    for (int i = 0; i < 6; ++i) {
        bool isCur = (i == (int)g_currentState);
        PointF p = pts[i];

        if (isCur) {
            Pen auraPen(Color(160, 16, 185, 129), 2.5f);
            g.DrawEllipse(&auraPen, p.X - nodeR - 7, p.Y - nodeR - 7, (nodeR + 7) * 2, (nodeR + 7) * 2);
        }

        Color fill = isCur ? Color(255, 6, 78, 59) : Color(255, 22, 33, 59);
        Color outline = isCur ? Color(255, 16, 185, 129) : Color(255, 56, 189, 248);
        SolidBrush nb(fill);
        Pen np(outline, 2.4f);

        g.FillEllipse(&nb, p.X - nodeR, p.Y - nodeR, nodeR * 2, nodeR * 2);
        g.DrawEllipse(&np, p.X - nodeR, p.Y - nodeR, nodeR * 2, nodeR * 2);

        // Double ring for Accepting State q0 (IDLE)
        if (i == ST_IDLE) {
            Pen innerPen(outline, 1.4f);
            float ir = nodeR - 5.5f;
            g.DrawEllipse(&innerPen, p.X - ir, p.Y - ir, ir * 2, ir * 2);

            // Start arrow pointing into q0
            Pen startPen(Color(255, 14, 165, 233), 2.5f);
            PointF sFrom(p.X - nodeR - 36, p.Y);
            PointF sTo(p.X - nodeR - 4, p.Y);
            g.DrawLine(&startPen, sFrom, sTo);
            DrawArrowHead(g, startPen, sTo, 0.0f, 9.0f);
        }

        RectF tr(p.X - nodeR, p.Y - nodeR, nodeR * 2, nodeR * 2);
        g.DrawString(nodeTitles[i], -1, &fNode, tr, &centerFmt, &whiteBrush);
    }

    // ------------------------------------------------------------------------
    // LEFT BOTTOM CARD: EXECUTION TRACE LOG
    // ------------------------------------------------------------------------
    float logY = topY + diagH + 12.0f;
    RectF logCard(pad, logY, leftW, logH);
    DrawGlassCard(g, logCard, 18.0f, Color(255, 17, 25, 44), Color(255, 38, 53, 84));
    g.DrawString(L"Live Transition Trace  δ(q, σ) = q'", -1, &fCardTitle, PointF(pad + 16, logY + 10), &whiteBrush);

    RectF logWell(pad + 14, logY + 34, leftW - 28, logH - 46);
    DrawGlassCard(g, logWell, 12.0f, Color(255, 9, 15, 29), Color(255, 30, 41, 59));

    float ly = logWell.Y + 8.0f;
    for (size_t i = 0; i < g_traceLog.size(); ++i) {
        bool isNewest = (i + 1 == g_traceLog.size());
        g.DrawString(g_traceLog[i].c_str(), -1, &fMono, PointF(logWell.X + 12, ly), isNewest ? &greenBrush : &mutedBrush);
        ly += 16.0f;
    }

    // ------------------------------------------------------------------------
    // RIGHT CARD: INTERACTIVE ATM HARDWARE CONSOLE
    // ------------------------------------------------------------------------
    float rightX = pad + leftW + 14.0f;
    RectF hwCard(rightX, topY, rightW, contentH);
    DrawGlassCard(g, hwCard, 18.0f, Color(255, 17, 25, 44), Color(255, 38, 53, 84));
    g.DrawString(L"Interactive ATM Hardware Terminal", -1, &fCardTitle, PointF(rightX + 16, topY + 12), &whiteBrush);

    // Virtual ATM LCD Display
    RectF lcd(rightX + 16, topY + 40, rightW - 32, 96);
    DrawGlassCard(g, lcd, 12.0f, Color(255, 4, 19, 28), Color(255, 56, 189, 248), 1.6f);
    g.DrawString(g_screenTitle.c_str(), -1, &fMonoBold, PointF(lcd.X + 14, lcd.Y + 12), &cyanBrush);
    g.DrawString(g_screenLine1.c_str(), -1, &fMono, PointF(lcd.X + 14, lcd.Y + 38), &whiteBrush);
    g.DrawString(g_screenLine2.c_str(), -1, &fMono, PointF(lcd.X + 14, lcd.Y + 60), &mutedBrush);

    // Section Labels
    g.DrawString(L"Card Slot & Dispenser", -1, &fSub, PointF(rightX + 16, topY + 148), &mutedBrush);
    g.DrawString(L"Service Operations (State q2 MENU)", -1, &fSub, PointF(rightX + 16, topY + 206), &mutedBrush);
    g.DrawString(L"Numeric Keypad (Default PIN: 1234)", -1, &fSub, PointF(rightX + 16, topY + 264), &mutedBrush);

    // Render All Interactive Pill Buttons
    for (const auto& b : g_buttons) {
        Color col = b.hovered ? b.hoverColor : b.baseColor;
        DrawGlassCard(g, b.bounds, 10.0f, col, Color(80, 255, 255, 255), 1.0f);
        g.DrawString(b.text.c_str(), -1, &fBtn, b.bounds, &centerFmt, &whiteBrush);
    }

    // ------------------------------------------------------------------------
    // BOTTOM STATUS BAR
    // ------------------------------------------------------------------------
    float stY = height - pad - statusH;
    RectF stCard(pad, stY, width - pad * 2, statusH);
    DrawGlassCard(g, stCard, 12.0f, Color(255, 17, 25, 44), Color(255, 38, 53, 84));
    SolidBrush dotSt(g_statusColor);
    g.FillEllipse(&dotSt, pad + 14, stY + 15, 10.0f, 10.0f);
    g.DrawString(g_statusBarText.c_str(), -1, &fSub, PointF(pad + 32, stY + 12), &whiteBrush);
}

// ============================================================================
// WIN32 WINDOW PROCEDURE
// ============================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            if (w > 0 && h > 0) {
                RebuildLayout(w, h);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            float mx = (float)LOWORD(lParam);
            float my = (float)HIWORD(lParam);
            bool changed = false;
            for (auto& b : g_buttons) {
                bool now = b.bounds.Contains(mx, my);
                if (now != b.hovered) {
                    b.hovered = now;
                    changed = true;
                }
            }
            if (changed) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            float mx = (float)LOWORD(lParam);
            float my = (float)HIWORD(lParam);
            for (const auto& b : g_buttons) {
                if (b.bounds.Contains(mx, my)) {
                    HandleButtonClick(b.id);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            // Double-buffered rendering to eliminate flicker
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            {
                Graphics g(memDC);
                RenderScene(g, w, h);
            }

            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int main() {
    SetProcessDPIAware();

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

    HINSTANCE hInstance = GetModuleHandle(nullptr);

    WNDCLASSEX wc = {sizeof(WNDCLASSEX)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AtmFsmLiquidGlassClass";
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(
        0, wc.lpszClassName,
        L"TOC Project — ATM Transaction Finite State Machine (C++)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1260, 780,
        nullptr, nullptr, hInstance, nullptr
    );

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}