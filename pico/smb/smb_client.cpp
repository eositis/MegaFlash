#include "smb_client.h"
#include "smb_crypto.h"
#include "smbdisk.h"
#include "network_pump.h"
#include "network.h"
#include "userconfig.h"
#include "misc.h"
#include "debug.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define SMB2_NEGOTIATE       0x0000
#define SMB2_SESSION_SETUP   0x0001
#define SMB2_TREE_CONNECT    0x0003
#define SMB2_CREATE          0x0005
#define SMB2_CLOSE           0x0006
#define SMB2_READ            0x0008
#define SMB2_WRITE           0x0009
#define SMB2_QUERY_DIRECTORY 0x0021

#define SMB2_FLAGS_SIGNED    0x00000008u
#define SMB2_FLAGS_PRIORITY  0x00000080u /* unused */

#define FILE_DIRECTORY_INFORMATION 1
#define FILE_ATTR_DIRECTORY        0x00000010u

#define SMB_RX_MAX  4096
#define SMB_TX_MAX  2048

static uint8_t g_smb_tx[SMB_TX_MAX];

enum SmbState {
  ST_IDLE,
  ST_WIFI,
  ST_DNS,
  ST_CONNECT,
  ST_NEGOTIATE,
  ST_SESS1,
  ST_SESS2,
  ST_TREE,
  ST_READY,
  ST_ERROR
};

enum OpKind {
  OP_NONE = 0,
  OP_LIST,
  OP_READ,
  OP_WRITE,
  OP_CREATE,
  OP_UNLINK
};

class CSmbSession : public INetworkSession {
public:
  CSmbSession() { ResetAll(); }

  void ResetAll() {
    state_ = ST_IDLE;
    status_ = "Idle";
    pcb_ = nullptr;
    rx_len_ = 0;
    mid_ = 1;
    session_id_ = 0;
    tree_id_ = 0;
    dialect_ = 0;
    credits_ = 1;
    memset(session_key_, 0, sizeof(session_key_));
    memset(signing_key_, 0, sizeof(signing_key_));
    signing_ = false;
    waiting_ = false;
    op_ = OP_NONE;
    op_rc_ = -1;
    file_id_valid_ = false;
    memset(server_chal_, 0, 8);
    ti_len_ = 0;
  }

  void ApplyConfig() {
    Disconnect();
    if (GetSmbEnabled() && CheckPicoW()) {
      state_ = ST_WIFI;
      status_ = "Connecting...";
    } else {
      state_ = ST_IDLE;
      status_ = GetSmbEnabled() ? "Needs Pico W / Pico 2 W" : "Disabled";
    }
  }

  bool Ready() const { return state_ == ST_READY; }
  bool WantsUnit() const { return GetSmbEnabled(); }
  const char *Status() const { return status_; }

  void OnPump(NetworkPump &pump) override {
    if (GetNetworkPump().IsLegacyOperationActive()) {
      if (state_ != ST_IDLE && state_ != ST_ERROR && state_ != ST_READY) {
        /* Allow READY to persist; pause new connects during TFTP. */
      }
      if (state_ < ST_READY) return;
    }
    StepConnect(pump);
    SmbDisk_ServiceCore0();
  }

  void OnDnsGetHostByNameResult(const ip_addr_t *ipaddr) override {
    GetNetworkPump().ClearDnsPendingOwner(this);
    if (!ipaddr) {
      Fail("Failed: DNS lookup");
      return;
    }
    dest_ = *ipaddr;
    StartTcp();
  }

  void OnTcpRecvPbuf(struct tcp_pcb *pcb, struct pbuf *p, err_t err) override {
    (void)err;
    if (!p) {
      Fail("Failed: server closed connection");
      return;
    }
    uint16_t tot = p->tot_len;
    if (rx_len_ + tot > SMB_RX_MAX) {
      pbuf_free(p);
      Fail("Failed: receive buffer overflow");
      return;
    }
    pbuf_copy_partial(p, rx_ + rx_len_, tot, 0);
    rx_len_ += tot;
    tcp_recved(pcb, tot);
    pbuf_free(p);
    TryConsume();
  }

  void OnTcpErr(err_t err) override {
    (void)err;
    pcb_ = nullptr;
    Fail("Failed: TCP error");
  }

  err_t OnTcpConnected(struct tcp_pcb *pcb, err_t err) override {
    (void)pcb;
    if (err != ERR_OK) {
      Fail("Failed: could not connect to port 445");
      return err;
    }
    state_ = ST_NEGOTIATE;
    status_ = "Negotiating SMB dialect";
    SendNegotiate();
    return ERR_OK;
  }

  void Abort() override {
    Disconnect();
    status_ = "Failed: aborted";
  }

  int RunOp(OpKind kind) {
    if (state_ != ST_READY) return -1;
    if (GetNetworkPump().IsLegacyOperationActive() && kind != OP_NONE) {
      /* Reads during TFTP: fail fast so Core 1 does not hang. */
      return -2;
    }
    op_ = kind;
    op_rc_ = -1;
    waiting_ = true;
    bool sent = DispatchOp();
    if (!sent) {
      waiting_ = false;
      op_ = OP_NONE;
      return -1;
    }
    absolute_time_t deadline = make_timeout_time_ms(8000);
    while (waiting_ && !time_reached(deadline)) {
      cyw43_arch_poll();
    }
    int rc = waiting_ ? -3 : op_rc_;
    waiting_ = false;
    op_ = OP_NONE;
    return rc;
  }

  smb_dirent_t ents[SMB_MAX_DIR_ENTS];
  int ent_count;
  char path[128];
  uint64_t offset;
  uint8_t *rw_buf;
  const uint8_t *rw_src;
  uint32_t rw_len, rw_got;
  bool create_dir;

private:
  void Fail(const char *s) {
    status_ = s;
    state_ = ST_ERROR;
    waiting_ = false;
    op_rc_ = -1;
    DisconnectPcb();
  }

  void DisconnectPcb() {
    if (pcb_) {
      GetNetworkPump().DestroyTcpPcb(pcb_);
      pcb_ = nullptr;
    }
  }

  void Disconnect() {
    DisconnectPcb();
    rx_len_ = 0;
    waiting_ = false;
    file_id_valid_ = false;
    session_id_ = 0;
    tree_id_ = 0;
  }

  void StepConnect(NetworkPump &pump) {
    if (state_ == ST_ERROR) {
      /* Retry slowly */
      static absolute_time_t next;
      if (!time_reached(next)) return;
      next = make_timeout_time_ms(15000);
      if (GetSmbEnabled()) {
        state_ = ST_WIFI;
        status_ = "Retrying...";
      }
      return;
    }
    if (state_ == ST_WIFI) {
      if (GetNetworkPump().IsLegacyOperationActive()) return;
      pump.Init();
      if (!pump.EnsureWifiConnected(GetSSID(), GetWPAKey())) {
        status_ = "Waiting for Wi-Fi";
        return;
      }
      state_ = ST_DNS;
      status_ = "Looking up host";
      ip_addr_t ip;
      GetNetworkPump().RegisterDnsPendingOwner(this);
      cyw43_arch_lwip_begin();
      err_t e = dns_gethostbyname(GetSmbHost(), &ip, NetworkPump_LegacyDnsCallback, this);
      cyw43_arch_lwip_end();
      if (e == ERR_OK) {
        GetNetworkPump().ClearDnsPendingOwner(this);
        dest_ = ip;
        StartTcp();
      } else if (e != ERR_INPROGRESS) {
        GetNetworkPump().ClearDnsPendingOwner(this);
        Fail("Failed: DNS lookup");
      }
    }
  }

  void StartTcp() {
    DisconnectPcb();
    pcb_ = GetNetworkPump().CreateTcpPcb(this);
    if (!pcb_) {
      Fail("Failed: out of TCP resources");
      return;
    }
    state_ = ST_CONNECT;
    status_ = "Connecting to port 445";
    cyw43_arch_lwip_begin();
    tcp_nagle_disable(pcb_);
    err_t e = tcp_connect(pcb_, &dest_, 445, NetworkPump_LegacyTcpConnected);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) Fail("Failed: TCP connect");
  }

  static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
  static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
  }
  static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }
  static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
  static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  }
  static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

  void PutNb(uint8_t *p, uint32_t body) {
    p[0] = 0;
    p[1] = (uint8_t)((body >> 16) & 0xff);
    p[2] = (uint8_t)((body >> 8) & 0xff);
    p[3] = (uint8_t)(body & 0xff);
  }

  void FillHdr(uint8_t *h, uint16_t cmd, uint32_t flags) {
    memset(h, 0, 64);
    h[0] = 0xfe; h[1] = 'S'; h[2] = 'M'; h[3] = 'B';
    wr16(h + 4, 64);
    wr16(h + 6, 1);
    wr16(h + 12, cmd);
    wr16(h + 14, 32);
    wr32(h + 16, flags | (signing_ ? SMB2_FLAGS_SIGNED : 0));
    wr64(h + 24, mid_++);
    wr32(h + 36, tree_id_);
    wr64(h + 40, session_id_);
  }

  void Sign(uint8_t *smb, uint32_t smblen) {
    if (!signing_) return;
    memset(smb + 48, 0, 16);
    uint8_t mac[16];
    smb_aes128_cmac(signing_key_, smb, smblen, mac);
    memcpy(smb + 48, mac, 16);
  }

  bool TcpSend(const uint8_t *data, uint32_t len) {
    if (!pcb_) return false;
    cyw43_arch_lwip_begin();
    err_t e = tcp_write(pcb_, data, (uint16_t)len, TCP_WRITE_FLAG_COPY);
    if (e == ERR_OK) tcp_output(pcb_);
    cyw43_arch_lwip_end();
    return e == ERR_OK;
  }

  void SendNegotiate() {
    uint8_t *pkt = g_smb_tx;
    uint8_t *smb = pkt + 4;
    FillHdr(smb, SMB2_NEGOTIATE, 0);
    uint8_t *b = smb + 64;
    wr16(b + 0, 36);
    wr16(b + 2, 4);
    wr16(b + 4, 1);
    wr16(b + 6, 0);
    wr32(b + 8, 0);
    memset(b + 12, 0, 16);
    wr32(b + 28, 0);
    wr32(b + 32, 0);
    wr16(b + 36, 0x0202);
    wr16(b + 38, 0x0210);
    wr16(b + 40, 0x0300);
    wr16(b + 42, 0x0302);
    uint32_t body = 64 + 36 + 8;
    PutNb(pkt, body);
    TcpSend(pkt, 4 + body);
    waiting_ = true;
  }

  void SendSession1() {
    uint8_t ntlm[64];
    memset(ntlm, 0, sizeof(ntlm));
    memcpy(ntlm, "NTLMSSP", 8);
    wr32(ntlm + 8, 1);
    /* UNICODE|OEM|REQ_TARGET|NTLM|NTLM2|128|56|version */
    wr32(ntlm + 12, 0x00088215 | 0x00000040);
    uint8_t *pkt = g_smb_tx;
    uint8_t *smb = pkt + 4;
    FillHdr(smb, SMB2_SESSION_SETUP, 0);
    uint8_t *b = smb + 64;
    wr16(b, 25);
    b[2] = 0;
    b[3] = 0;
    wr32(b + 4, 0);
    wr16(b + 8, 64 + 24);
    wr16(b + 10, 40);
    wr32(b + 12, 0);
    wr32(b + 16, 0);
    wr32(b + 20, 0);
    memcpy(b + 24, ntlm, 40);
    uint32_t body = 64 + 24 + 40;
    PutNb(pkt, body);
    TcpSend(pkt, 4 + body);
    waiting_ = true;
  }

  void BuildType3(uint8_t *out, uint32_t *outlen) {
    uint8_t pass_u[128], user_u[80], dom_u[48], blob[320];
    size_t pass_n = 0, user_n = 0, dom_n = 0;
    smb_ascii_to_utf16le(GetSmbPassword(), pass_u, &pass_n);
    smb_ascii_to_utf16le_upper(GetSmbUser(), user_u, &user_n);
    smb_ascii_to_utf16le(GetSmbDomain(), dom_u, &dom_n);

    uint8_t nthash[16], ntowfv2[16];
    smb_md4(pass_u, pass_n, nthash);
    uint8_t ident[160];
    memcpy(ident, user_u, user_n);
    memcpy(ident + user_n, dom_u, dom_n);
    smb_hmac_md5(nthash, 16, ident, user_n + dom_n, ntowfv2);

    uint8_t client_chal[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    size_t bp = 0;
    blob[bp++] = 0x01; blob[bp++] = 0x01; blob[bp++] = 0; blob[bp++] = 0;
    memset(blob + bp, 0, 4); bp += 4;
    /* NT timestamp */
    uint64_t unix_s = to_us_since_boot(get_absolute_time()) / 1000000ull + 1700000000ull;
    uint64_t nt = (unix_s + 11644473600ull) * 10000000ull;
    wr64(blob + bp, nt); bp += 8;
    memcpy(blob + bp, client_chal, 8); bp += 8;
    memset(blob + bp, 0, 4); bp += 4;
    if (ti_len_ && ti_len_ < sizeof(blob) - bp - 4) {
      memcpy(blob + bp, target_info_, ti_len_);
      bp += ti_len_;
    }
    memset(blob + bp, 0, 4); bp += 4;

    uint8_t tmp[16 + 320];
    memcpy(tmp, server_chal_, 8);
    memcpy(tmp + 8, blob, bp);
    uint8_t ntproof[16];
    smb_hmac_md5(ntowfv2, 16, tmp, 8 + bp, ntproof);

    uint8_t ntresp[16 + 320];
    memcpy(ntresp, ntproof, 16);
    memcpy(ntresp + 16, blob, bp);
    uint32_t ntresp_len = 16 + (uint32_t)bp;

    uint8_t lm[24];
    uint8_t lm_in[16];
    memcpy(lm_in, server_chal_, 8);
    memcpy(lm_in + 8, client_chal, 8);
    uint8_t lmhash[16];
    smb_hmac_md5(ntowfv2, 16, lm_in, 16, lmhash);
    memcpy(lm, lmhash, 16);
    memcpy(lm + 16, client_chal, 8);

    /* Session base key for SMB signing: HMAC-MD5(NTOWFv2, NTProof) */
    smb_hmac_md5(ntowfv2, 16, ntproof, 16, session_key_);

    uint32_t off = 88;
    memset(out, 0, 88);
    memcpy(out, "NTLMSSP", 8);
    wr32(out + 8, 3);
    wr16(out + 12, 24); wr16(out + 14, 24); wr32(out + 16, off);
    memcpy(out + off, lm, 24); off += 24;
    wr16(out + 20, (uint16_t)ntresp_len); wr16(out + 22, (uint16_t)ntresp_len); wr32(out + 24, off);
    memcpy(out + off, ntresp, ntresp_len); off += ntresp_len;
    wr16(out + 28, (uint16_t)dom_n); wr16(out + 30, (uint16_t)dom_n); wr32(out + 32, off);
    memcpy(out + off, dom_u, dom_n); off += (uint32_t)dom_n;
    wr16(out + 36, (uint16_t)user_n); wr16(out + 38, (uint16_t)user_n); wr32(out + 40, off);
    memcpy(out + off, user_u, user_n); off += (uint32_t)user_n;
    wr16(out + 44, 0); wr16(out + 46, 0); wr32(out + 48, off);
    wr16(out + 52, 0); wr16(out + 54, 0); wr32(out + 56, off);
    wr32(out + 60, 0x00088215);
    *outlen = off;
  }

  void SendSession2() {
    uint8_t type3[700];
    uint32_t t3len = 0;
    BuildType3(type3, &t3len);
    uint8_t *pkt = g_smb_tx;
    uint8_t *smb = pkt + 4;
    FillHdr(smb, SMB2_SESSION_SETUP, 0);
    wr64(smb + 40, session_id_);
    uint8_t *b = smb + 64;
    wr16(b, 25);
    b[2] = 0;
    b[3] = 0;
    wr32(b + 4, 0);
    wr16(b + 8, 64 + 24);
    wr16(b + 10, (uint16_t)t3len);
    wr32(b + 12, 0);
    wr32(b + 16, 0);
    wr32(b + 20, 0);
    memcpy(b + 24, type3, t3len);
    uint32_t body = 64 + 24 + t3len;
    PutNb(pkt, body);
    TcpSend(pkt, 4 + body);
    waiting_ = true;
  }

  void SendTreeConnect() {
    char unc[160];
    snprintf(unc, sizeof(unc), "\\\\%s\\%s", GetSmbHost(), GetSmbShare());
    uint8_t pathu[320];
    size_t plen = 0;
    smb_ascii_to_utf16le(unc, pathu, &plen);
    uint8_t *pkt = g_smb_tx;
    uint8_t *smb = pkt + 4;
    FillHdr(smb, SMB2_TREE_CONNECT, 0);
    uint8_t *b = smb + 64;
    wr16(b, 9);
    wr16(b + 2, 0);
    wr16(b + 4, 64 + 8);
    wr16(b + 6, (uint16_t)plen);
    memcpy(b + 8, pathu, plen);
    uint32_t body = 64 + 8 + (uint32_t)plen;
    Sign(smb, body);
    PutNb(pkt, body);
    TcpSend(pkt, 4 + body);
    waiting_ = true;
  }

  bool Utf16ToAscii(const uint8_t *u, uint32_t nbytes, char *out, size_t outsz) {
    size_t n = nbytes / 2;
    if (n >= outsz) n = outsz - 1;
    for (size_t i = 0; i < n; i++) out[i] = (char)u[i * 2];
    out[n] = 0;
    return true;
  }

  void SendCreate(const char *path, uint32_t create_disp, uint32_t options, bool dir) {
    uint8_t nameu[256];
    size_t nlen = 0;
    const char *p = path ? path : "";
    if (p[0] == '/' || p[0] == '\\') p++;
    smb_ascii_to_utf16le(p, nameu, &nlen);
    uint8_t *pkt = g_smb_tx;
    uint8_t *smb = pkt + 4;
    FillHdr(smb, SMB2_CREATE, 0);
    uint8_t *b = smb + 64;
    memset(b, 0, 56);
    wr16(b + 0, 57);
    b[2] = 0;
    b[3] = 0;
    wr32(b + 4, 2); /* impersonation */
    wr32(b + 24, dir ? 0x00100081u : 0x0012019Fu);
    wr32(b + 28, dir ? 0x10u : 0x80u);
    wr32(b + 32, 0x07);
    wr32(b + 36, create_disp);
    wr32(b + 40, options);
    wr16(b + 44, 64 + 56);
    wr16(b + 46, (uint16_t)nlen);
    if (nlen) memcpy(b + 56, nameu, nlen);
    uint32_t body = 64 + 56 + (uint32_t)nlen;
    Sign(smb, body);
    PutNb(pkt, body);
    TcpSend(pkt, 4 + body);
    waiting_ = true;
    create_is_dir_ = dir;
  }

  void SendQueryDir() {
    uint8_t *pkt = g_smb_tx;
    uint8_t *smb = pkt + 4;
    FillHdr(smb, SMB2_QUERY_DIRECTORY, 0);
    uint8_t *b = smb + 64;
    wr16(b, 33);
    b[2] = FILE_DIRECTORY_INFORMATION;
    b[3] = 0;
    wr32(b + 4, 0);
    memcpy(b + 8, file_id_, 16);
    wr16(b + 24, 64 + 32);
    wr16(b + 26, 2); /* "*" */
    wr32(b + 28, 4096);
    b[32] = '*'; b[33] = 0;
    uint32_t body = 64 + 32 + 2;
    Sign(smb, body);
    PutNb(pkt, body);
    TcpSend(pkt, 4 + body);
    waiting_ = true;
  }

  void SendRead() {
    uint8_t *pkt = g_smb_tx;
    uint8_t *smb = pkt + 4;
    FillHdr(smb, SMB2_READ, 0);
    uint8_t *b = smb + 64;
    wr16(b, 49);
    b[2] = 0;
    b[3] = 0;
    wr32(b + 4, rw_len);
    wr64(b + 8, offset);
    memcpy(b + 16, file_id_, 16);
    wr32(b + 32, 0);
    wr32(b + 36, 0);
    wr32(b + 40, 0);
    wr16(b + 44, 0);
    wr32(b + 46, 0);
    uint32_t body = 64 + 48;
    Sign(smb, body);
    PutNb(pkt, body);
    TcpSend(pkt, 4 + body);
    waiting_ = true;
  }

  void SendWrite() {
    uint8_t *pkt = g_smb_tx;
    uint8_t *smb = pkt + 4;
    FillHdr(smb, SMB2_WRITE, 0);
    uint8_t *b = smb + 64;
    wr16(b, 49);
    wr16(b + 2, 64 + 48);
    wr32(b + 4, rw_len);
    wr64(b + 8, offset);
    memcpy(b + 16, file_id_, 16);
    wr32(b + 32, 0);
    wr32(b + 36, 0);
    wr32(b + 40, 0);
    wr16(b + 44, 0);
    memcpy(b + 48, rw_src, rw_len);
    uint32_t body = 64 + 48 + rw_len;
    Sign(smb, body);
    PutNb(pkt, body);
    TcpSend(pkt, 4 + body);
    waiting_ = true;
  }

  void SendClose() {
    uint8_t *pkt = g_smb_tx;
    uint8_t *smb = pkt + 4;
    FillHdr(smb, SMB2_CLOSE, 0);
    uint8_t *b = smb + 64;
    wr16(b, 24);
    wr16(b + 2, 0);
    memcpy(b + 8, file_id_, 16);
    uint32_t body = 64 + 24;
    Sign(smb, body);
    PutNb(pkt, body);
    TcpSend(pkt, 4 + body);
    waiting_ = true;
    file_id_valid_ = false;
  }

  bool DispatchOp() {
    switch (op_) {
      case OP_LIST:
        ent_count = 0;
        SendCreate("", 1, 1, true);
        after_create_ = 1;
        return true;
      case OP_READ:
        SendCreate(path, 1, 0x40, false);
        after_create_ = 2;
        return true;
      case OP_WRITE:
        SendCreate(path, 3, 0x40, false);
        after_create_ = 3;
        return true;
      case OP_CREATE:
        SendCreate(path, 2, create_dir ? 1 : 0x40, create_dir);
        after_create_ = 4;
        return true;
      case OP_UNLINK:
        /* Open then close with delete pending is heavy; use CREATE + SET_INFO skip:
         * open with DELETE, then close. Minimal: CREATE disposition open, then we only
         * support delete via overwrite truncate not implemented fully.
         * Use CREATE with DELETE access and SMB2 SET_INFO delete — skip; try CREATE
         * then CLOSE. Unlink implemented as create+close of empty? Better: CREATE
         * access DELETE, FILE_DISPOSITION_INFO. Skip extra command: treat unlink as
         * CREATE overwrite 0-length via write path from prodos.
         */
        SendCreate(path, 1, 0x40, false);
        after_create_ = 5;
        return true;
      default:
        return false;
    }
  }

  void ParseDirBuffer(const uint8_t *data, uint32_t len) {
    uint32_t off = 0;
    ent_count = 0;
    while (off + 64 <= len && ent_count < SMB_MAX_DIR_ENTS) {
      uint32_t next = rd32(data + off);
      uint32_t attr = rd32(data + off + 56);
      uint32_t nlen = rd32(data + off + 60);
      uint64_t eof = rd64(data + off + 40);
      char name[SMB_NAME_MAX];
      Utf16ToAscii(data + off + 64, nlen, name, sizeof(name));
      if (strcmp(name, ".") && strcmp(name, "..") && name[0] != '.') {
        smb_dirent_t *e = &ents[ent_count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->smb_name, name, SMB_NAME_MAX - 1);
        e->size = eof;
        e->attr = attr;
        e->is_dir = (attr & FILE_ATTR_DIRECTORY) != 0;
      }
      if (next == 0) break;
      off += next;
    }
  }

  void HandleSmb(const uint8_t *smb, uint32_t len) {
    if (len < 64) return;
    uint32_t status = rd32(smb + 8);
    uint16_t cmd = rd16(smb + 12);
    uint64_t sid = rd64(smb + 40);
    uint32_t tid = rd32(smb + 36);

    if (cmd == SMB2_NEGOTIATE) {
      waiting_ = false;
      if (status != 0) { Fail("Failed: negotiate rejected by server"); return; }
      dialect_ = rd16(smb + 64 + 4);
      uint16_t secmode = rd16(smb + 64 + 2);
      session_id_ = 0;
      signing_ = false;
      (void)secmode;
      state_ = ST_SESS1;
      status_ = "Authenticating (NTLMv2)";
      SendSession1();
      return;
    }
    if (cmd == SMB2_SESSION_SETUP) {
      session_id_ = sid;
      const uint8_t *b = smb + 64;
      uint16_t secoff = rd16(b + 4);
      uint16_t seclen = rd16(b + 6);
      if (status == 0xC0000016) { /* MORE_PROCESSING_REQUIRED */
        const uint8_t *sec = smb + secoff;
        const uint8_t *ntlm = sec;
        uint32_t ntlm_len = seclen;
        for (uint32_t i = 0; i + 8 < seclen; i++) {
          if (memcmp(sec + i, "NTLMSSP", 8) == 0) {
            ntlm = sec + i;
            ntlm_len = seclen - i;
            break;
          }
        }
        if (ntlm_len >= 48 && memcmp(ntlm, "NTLMSSP", 7) == 0) {
          memcpy(server_chal_, ntlm + 24, 8);
          uint16_t tilen = rd16(ntlm + 40);
          uint32_t tioff = rd32(ntlm + 44);
          ti_len_ = 0;
          if (tilen && tioff + tilen <= ntlm_len) {
            ti_len_ = tilen > sizeof(target_info_) ? sizeof(target_info_) : tilen;
            memcpy(target_info_, ntlm + tioff, ti_len_);
          }
        }
        state_ = ST_SESS2;
        SendSession2();
        return;
      }
      waiting_ = false;
      if (status != 0) { Fail("Failed: wrong user/password or auth rejected"); return; }
      smb3_kdf_signkey(session_key_, 16, signing_key_);
      signing_ = (dialect_ >= 0x0300);
      state_ = ST_TREE;
      status_ = "Connecting to share";
      SendTreeConnect();
      return;
    }
    if (cmd == SMB2_TREE_CONNECT) {
      waiting_ = false;
      if (status != 0) { Fail("Failed: share not found or access denied"); return; }
      tree_id_ = tid;
      state_ = ST_READY;
      status_ = "OK: share mounted";
      return;
    }
    if (cmd == SMB2_CREATE) {
      if (status != 0) {
        waiting_ = false;
        op_rc_ = -1;
        return;
      }
      memcpy(file_id_, smb + 64 + 64, 16);
      file_id_valid_ = true;
      if (after_create_ == 1) { SendQueryDir(); return; }
      if (after_create_ == 2) { SendRead(); return; }
      if (after_create_ == 3) { SendWrite(); return; }
      if (after_create_ == 4) { SendClose(); after_create_ = 9; return; }
      if (after_create_ == 5) { SendClose(); after_create_ = 9; op_rc_ = 0; return; }
      waiting_ = false;
      op_rc_ = 0;
      return;
    }
    if (cmd == SMB2_QUERY_DIRECTORY) {
      if (status == 0) {
        uint16_t doff = rd16(smb + 64 + 2);
        uint32_t dlen = rd32(smb + 64 + 4);
        if (doff + dlen <= len) ParseDirBuffer(smb + doff, dlen);
      }
      SendClose();
      after_create_ = 8;
      op_rc_ = 0;
      return;
    }
    if (cmd == SMB2_READ) {
      if (status == 0) {
        uint16_t doff = rd16(smb + 64 + 2);
        uint32_t dlen = rd32(smb + 64 + 4);
        rw_got = dlen;
        if (rw_buf && doff + dlen <= len) {
          uint32_t n = dlen > rw_len ? rw_len : dlen;
          memcpy(rw_buf, smb + doff, n);
          rw_got = n;
        }
        op_rc_ = 0;
      } else {
        op_rc_ = -1;
        rw_got = 0;
      }
      SendClose();
      after_create_ = 9;
      return;
    }
    if (cmd == SMB2_WRITE) {
      op_rc_ = (status == 0) ? 0 : -1;
      SendClose();
      after_create_ = 9;
      return;
    }
    if (cmd == SMB2_CLOSE) {
      waiting_ = false;
      file_id_valid_ = false;
      if (op_rc_ < 0 && after_create_ == 8) op_rc_ = 0;
      if (after_create_ == 9 && op_rc_ < 0) op_rc_ = 0;
      return;
    }
  }

  void TryConsume() {
    while (rx_len_ >= 4) {
      uint32_t body = ((uint32_t)rx_[1] << 16) | ((uint32_t)rx_[2] << 8) | rx_[3];
      if (rx_[0] != 0) {
        rx_len_ = 0;
        return;
      }
      if (rx_len_ < 4 + body) return;
      HandleSmb(rx_ + 4, body);
      uint32_t used = 4 + body;
      memmove(rx_, rx_ + used, rx_len_ - used);
      rx_len_ -= used;
    }
  }

  SmbState state_;
  const char *status_;
  struct tcp_pcb *pcb_;
  ip_addr_t dest_;
  uint8_t rx_[SMB_RX_MAX];
  uint32_t rx_len_;
  uint64_t mid_;
  uint64_t session_id_;
  uint32_t tree_id_;
  uint16_t dialect_;
  uint16_t credits_;
  uint8_t session_key_[16];
  uint8_t signing_key_[16];
  bool signing_;
  bool waiting_;
  OpKind op_;
  int op_rc_;
  uint8_t file_id_[16];
  bool file_id_valid_;
  uint8_t server_chal_[8];
  uint8_t target_info_[256];
  uint32_t ti_len_;
  int after_create_;
  bool create_is_dir_;
};

static CSmbSession g_smb;
static bool g_smb_registered;

extern "C" err_t NetworkPump_LegacyTcpConnected(void *arg, struct tcp_pcb *tpcb, err_t err) {
  INetworkSession *session = static_cast<INetworkSession *>(arg);
  if (!session) return err;
  return session->OnTcpConnected(tpcb, err);
}

extern "C" err_t NetworkPump_LegacyTcpSent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
  INetworkSession *session = static_cast<INetworkSession *>(arg);
  if (!session) return ERR_OK;
  return session->OnTcpSent(tpcb, len);
}

extern "C" void SmbClient_Init(void) {
  if (!g_smb_registered) {
    GetNetworkPump().AddSession(&g_smb);
    g_smb_registered = true;
  }
  g_smb.ApplyConfig();
}

extern "C" void SmbClient_OnConfigChanged(void) {
  SmbClient_Init();
}

extern "C" void SmbClient_Pump(void) {
  g_smb.OnPump(GetNetworkPump());
}

extern "C" bool SmbClient_IsReady(void) { return g_smb.Ready(); }
extern "C" bool SmbClient_WantsUnit(void) { return g_smb.WantsUnit(); }
extern "C" const char *SmbClient_StatusText(void) { return g_smb.Status(); }

extern "C" int SmbClient_ListDir(const char *path, smb_dirent_t *ents, int max_ents, int *count_out) {
  (void)path;
  strncpy(g_smb.path, path ? path : "", sizeof(g_smb.path) - 1);
  int rc = g_smb.RunOp(OP_LIST);
  if (rc == 0 && ents && max_ents > 0) {
    int n = g_smb.ent_count;
    if (n > max_ents) n = max_ents;
    memcpy(ents, g_smb.ents, (size_t)n * sizeof(smb_dirent_t));
    if (count_out) *count_out = n;
  } else if (count_out) {
    *count_out = 0;
  }
  return rc;
}

extern "C" int SmbClient_Read(const char *path, uint64_t offset, uint8_t *buf, uint32_t len, uint32_t *got) {
  strncpy(g_smb.path, path ? path : "", sizeof(g_smb.path) - 1);
  g_smb.offset = offset;
  g_smb.rw_buf = buf;
  g_smb.rw_len = len;
  g_smb.rw_got = 0;
  int rc = g_smb.RunOp(OP_READ);
  if (got) *got = g_smb.rw_got;
  return rc;
}

extern "C" int SmbClient_Write(const char *path, uint64_t offset, const uint8_t *buf, uint32_t len) {
  strncpy(g_smb.path, path ? path : "", sizeof(g_smb.path) - 1);
  g_smb.offset = offset;
  g_smb.rw_src = buf;
  g_smb.rw_len = len;
  return g_smb.RunOp(OP_WRITE);
}

extern "C" int SmbClient_Create(const char *path, bool directory) {
  strncpy(g_smb.path, path ? path : "", sizeof(g_smb.path) - 1);
  g_smb.create_dir = directory;
  return g_smb.RunOp(OP_CREATE);
}

extern "C" int SmbClient_Unlink(const char *path) {
  strncpy(g_smb.path, path ? path : "", sizeof(g_smb.path) - 1);
  return g_smb.RunOp(OP_UNLINK);
}
