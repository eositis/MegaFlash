#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>
#include "defines.h"
#include "ui-menu.h"
#include "ui-wnd.h"
#include "ui-textinput.h"
#include "ui-misc.h"
#include "textstrings.h"
#include "asm.h"
#include "smb.h"

#define XPOS 1
#define YPOS 3
#define WIDTH 38
#define HEIGHT 18

static char smbTitle[] = "SMB Share Disk";

/* Status line buffer (keep out of ti_textBuffer at $200). */
static char statusBuf[80];

static void DrawFrame(bool active, bool clear) {
  wnd_DrawWindow(XPOS, YPOS, WIDTH, HEIGHT, smbTitle, active, clear);
}

static void SanitizeField(char *s, uint8_t max) {
  static_local uint8_t i;
  static_local unsigned char c;
  for (i = 0; i < max; ++i) {
    c = (unsigned char)s[i];
    if (c == 0)
      break;
    if (c < 32 || c > 126) {
      s[0] = '\0';
      return;
    }
  }
  s[i] = '\0';
}

static void SanitizeSettings(SmbSetting_t *s) {
  if (s->version != SMBSETTINGVER ||
      s->checkbyte != (uint8_t)(SMBSETTINGVER ^ SMBSETTING_CHKBYTECOMP)) {
    memset(s, 0, sizeof(*s));
    s->version = SMBSETTINGVER;
    s->checkbyte = (uint8_t)(SMBSETTINGVER ^ SMBSETTING_CHKBYTECOMP);
    return;
  }
  SanitizeField(s->host, SMB_HOST_MAX);
  SanitizeField(s->share, SMB_SHARE_MAX);
  SanitizeField(s->user, SMB_USER_MAX);
  SanitizeField(s->password, SMB_PASS_MAX);
  SanitizeField(s->domain, SMB_DOMAIN_MAX);
}

/////////////////////////////////////////////////////////////////////
// Screen 1: Enable SMB?  Yes / No with inverse highlight
//
static bool AskEnable(SmbSetting_t *s) {
  static_local uint8_t key;

  DrawFrame(true, true);
  cputs("Mount a remote SMB3 folder as a\n\rProDOS disk on this MegaFlash.\n\n\r");
  cputs("Enable SMB Share Disk?\n\r");
  gotoxy(1, HEIGHT - 1);
  cputs(strCancelesc);

  mnu_currentMenuItem = s->enabled ? 0 : 1; /* Yes=0, No=1 */
  do {
    key = DoMenu(yesnoMenuItem, 2, 2, 6);
    if (key == KEY_ESC)
      return false;
  } while (key != KEY_ENTER);

  s->enabled = (mnu_currentMenuItem == 0) ? 1 : 0;
  return true;
}

/////////////////////////////////////////////////////////////////////
// Screen 2: all remaining fields on one page (sequential entry)
//
// Layout (content Y): labels then input rows with room for wrap.
//
static bool EnterConfigFields(SmbSetting_t *s) {
  DrawFrame(true, true);
  cputs("Enter share settings. Esc cancels.\n\n\r");
  cputs("Host (name or IP):\n\n\r\n\r");
  cputs("Share name:\n\n\r\n\r");
  cputs("Username:\n\n\r");
  cputs("Password:\n\n\r\n\r");
  cputs("Domain (optional):\n\r");
  gotoxy(1, HEIGHT - 1);
  cputs(strEditPrompt);

  /* Host — required (y=3, up to 2 lines @ width 36) */
host_again:
  strcpy(ti_textBuffer, s->host);
  if (!ti_EnterHostname(SMB_HOST_MAX, 0, 3, 36))
    return false;
  if (ti_textBuffer[0] == '\0') {
    beep();
    goto host_again;
  }
  strcpy(s->host, ti_textBuffer);

  /* Share — required (y=6) */
share_again:
  strcpy(ti_textBuffer, s->share);
  if (!ti_EnterText(SMB_SHARE_MAX, 0, 6, 36))
    return false;
  if (ti_textBuffer[0] == '\0') {
    beep();
    goto share_again;
  }
  strcpy(s->share, ti_textBuffer);

  /* Username — required (y=9) */
user_again:
  strcpy(ti_textBuffer, s->user);
  if (!ti_EnterText(SMB_USER_MAX, 0, 9, 36))
    return false;
  if (ti_textBuffer[0] == '\0') {
    beep();
    goto user_again;
  }
  strcpy(s->user, ti_textBuffer);

  /* Password — blank keeps existing (y=11) */
  ti_textBuffer[0] = '\0';
  if (!ti_EnterText(SMB_PASS_MAX, 0, 11, 36))
    return false;
  if (ti_textBuffer[0])
    strcpy(s->password, ti_textBuffer);

  /* Domain — optional (y=14) */
  strcpy(ti_textBuffer, s->domain);
  if (!ti_EnterText(SMB_DOMAIN_MAX, 0, 14, 36))
    return false;
  strcpy(s->domain, ti_textBuffer);

  return true;
}

/////////////////////////////////////////////////////////////////////
// Classify Pico status text into pass / fail / wait
//
// Pico uses prefixes: "OK:", "Failed:", "Disabled", "Needs ", or
// in-progress phrases without those prefixes.
//
typedef enum {
  SMBST_WAIT = 0,
  SMBST_PASS,
  SMBST_FAIL,
  SMBST_OFF
} SmbStatusKind;

static SmbStatusKind ClassifyStatus(const char *t) {
  if (!t || !t[0])
    return SMBST_WAIT;
  if (t[0] == 'O' && t[1] == 'K' && t[2] == ':')
    return SMBST_PASS;
  if (strncmp(t, "Failed:", 7) == 0)
    return SMBST_FAIL;
  if (strncmp(t, "Disabled", 8) == 0)
    return SMBST_OFF;
  if (strncmp(t, "Needs ", 6) == 0)
    return SMBST_FAIL;
  if (strncmp(t, "SMB compiled", 12) == 0)
    return SMBST_FAIL;
  /* Legacy short strings during transition */
  if (strcmp(t, "SMB ready") == 0)
    return SMBST_PASS;
  if (strcmp(t, "SMB disabled") == 0)
    return SMBST_OFF;
  if (strncmp(t, "SMB ", 4) == 0) {
    /* "SMB DNS failed", "SMB auth failed", etc. */
    if (strstr(t, "fail") || strstr(t, "Fail") || strstr(t, "error") ||
        strstr(t, "Error") || strstr(t, "rejected") || strstr(t, "closed") ||
        strstr(t, "overflow") || strstr(t, "aborted"))
      return SMBST_FAIL;
  }
  return SMBST_WAIT;
}

static void FetchStatus(void) {
  SendCommand(CMD_SMBSTATUS);
  statusBuf[0] = '\0';
  CopyStringFromDataBuffer(statusBuf);
  statusBuf[sizeof(statusBuf) - 1] = '\0';
}

static void ShowStatusResult(bool saveOk) {
  static_local uint8_t i;
  static_local SmbStatusKind kind;

  DrawFrame(true, true);
  if (!saveOk) {
    cputs(strError);
    gotoxy(1, HEIGHT - 1);
    cputs(strOKAnyKey);
    cgetc_showclock();
    return;
  }

  cputs("Settings saved.\n\rChecking connection...\n\n\r");
  cputs(strStatus);
  newline();

  /* Poll until ready / failed / disabled, or timeout (~25 s). */
  for (i = 0; i < 50; ++i) {
    FetchStatus();
    gotoxy(0, 4);
    clreol();
    cputs(statusBuf);
    clreol();
    kind = ClassifyStatus(statusBuf);
    if (kind != SMBST_WAIT)
      break;
    Delay(200);
    if ((i & 7) == 0)
      DisplayTime();
  }

  gotoxy(0, 6);
  clreol();
  cputs(strResult);
  newline();

  if (kind == SMBST_PASS) {
    cputs("PASSED\n\n\r");
    cputs(statusBuf);
  } else if (kind == SMBST_OFF) {
    cputs("DISABLED\n\n\r");
    cputs("SMB Share Disk is off.\n\rNo remote volume is mounted.");
  } else if (kind == SMBST_FAIL) {
    cputs("FAILED\n\n\r");
    cputs("Reason:\n\r");
    cputs(statusBuf);
  } else {
    cputs("TIMEOUT\n\n\r");
    cputs("Still connecting.\n\rLast status:\n\r");
    cputs(statusBuf);
  }

  gotoxy(1, HEIGHT - 1);
  cputs(strOKAnyKey);
  cgetc_showclock();
}

void DoSmbSettings(void) {
  static_local SmbSetting_t s;
  static_local bool success;

  memset(&s, 0, sizeof(SmbSetting_t));
  LoadSetting(CMD_GETSMBSETTINGS, (uint8_t)sizeof(SmbSetting_t), &s);
  s.version = SMBSETTINGVER;
  s.checkbyte = (uint8_t)(SMBSETTINGVER ^ SMBSETTING_CHKBYTECOMP);
  SanitizeSettings(&s);

  if (!AskEnable(&s))
    return;

  if (s.enabled) {
    if (!EnterConfigFields(&s))
      return;
  }

  DrawFrame(true, true);
  cputs(strSaving);
  success = SaveSetting(CMD_SAVESMBSETTINGS, (uint8_t)sizeof(SmbSetting_t), &s);
  ShowStatusResult(success);
}
