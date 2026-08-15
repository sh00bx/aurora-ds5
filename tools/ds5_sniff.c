/* ds5_sniff — passive HCI-monitor probe for the DS5 latency programme (Phase 2).
 *
 * Answers 2.3 (0x31 wire rate during episodes), 2.4 (role audit) and 2.5 (LE /
 * Magic-Remote correlation) from ONE capture, and provides the independent
 * ground truth that Phase 1's acceptance asks for ("capture-thread gaps agree
 * with monitor ground truth to +-2ms") — ds5_txd cannot be its own witness.
 *
 * STRICTLY READ-ONLY. It binds HCI_CHANNEL_MONITOR, which is a receive-only
 * broadcast channel: there is no code path here that can emit an HCI command,
 * so it cannot cost link budget or perturb the very thing it measures. Multiple
 * monitor readers coexist (ds5_txd's own ds5-cap keeps running untouched).
 *
 * Why not extend ds5_txd instead: every daemon change needs a rebuild and a
 * restart, and the daemon must never be restarted mid-session. A separate
 * process attaches and detaches at will, including in the middle of a running
 * game, which is exactly when the interesting episodes happen.
 *
 * Output (line-oriented, one file, grep-able by record type):
 *   HDR  ... one line at start: wall clock anchor + argv
 *   EV   ... link lifecycle: conn/disconn/role/mode/flush/slots, timestamped
 *   GAP  ... one line per NOCP gap >=30ms that closed, WITH the in-gap traffic
 *            breakdown. This is the 2.3/2.5 join: what was on the wire while
 *            our credits were starving, without any post-hoc log alignment.
 *   SEC  ... one aggregate line per second (rates, background context)
 *   SUM  ... totals at exit
 *
 * Build (see tools/README-ds5_sniff.md):
 *   ~/x-tools/armv5-eabi--musl--stable-2025.08-1/bin/arm-linux-gcc -O2 -static \
 *       -Wall -Wextra ds5_sniff.c -o ds5_sniff
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/resource.h>

/* Same wire constants ds5_txd uses; kept literal so this tool builds standalone
 * against any libc without BlueZ headers (the TV has none). */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI         1
#define HCI_CHANNEL_MONITOR 2
#define HCI_DEV_NONE        0xffff

#define MON_COMMAND_PKT     2
#define MON_EVENT_PKT       3
#define MON_ACL_TX_PKT      4
#define MON_ACL_RX_PKT      5

#define EV_CONN_COMPLETE    0x03
#define EV_DISCONN_COMPLETE 0x05
#define EV_FLUSH_OCCURRED   0x11   /* the L1 (Write_Automatic_Flush_Timeout) witness */
#define EV_ROLE_CHANGE      0x12
#define EV_NUM_COMP_PKTS    0x13
#define EV_MODE_CHANGE      0x14
#define EV_MAX_SLOTS_CHANGE 0x1b
#define EV_LE_META          0x3e
#define SUBEV_LE_CONN       0x01
#define SUBEV_LE_ENH_CONN   0x0a

#define OP_ACCEPT_CONN_REQ  0x0409
#define OP_SWITCH_ROLE      0x080b

#define GAP_MIN_MS          30     /* same floor as the daemon's first bin */
#define NHANDLE             4096

struct hci_mon_hdr { uint16_t opcode, index, len; } __attribute__((packed));

/* Link type. A handle that was already up when we attached has no Connection
 * Complete to learn from, and attaching mid-session is the main use case (the
 * interesting episodes happen during a running game). So an unseen handle starts
 * UNKNOWN and is promoted to BR/EDR by the only evidence available on the wire:
 * an ACL payload no LE link can carry. LE's data channel PDU tops out at 251
 * bytes even with Data Length Extension, and the DS5 audio frames are 398/547 —
 * a >100B payload is therefore proof, not a guess. Everything still UNKNOWN is
 * kept OUT of the gap ledger rather than guessed into it: a Magic-Remote LE
 * handle counted as a starving DS5 link would manufacture exactly the gaps this
 * tool exists to attribute. */
#define LT_BREDR   0
#define LT_LE      1
#define LT_UNKNOWN 2
#define MTU_PROVES_BREDR 100

/* Per-handle state. Indexed by the 12-bit handle, like ds5_txd's g_htab, so a
 * handle reassignment cannot alias into a stale slot. */
struct hstate {
    uint8_t  known, is_le, mode, have_role, role;   /* role: 0=master(TV) 1=slave(TV) */
    uint8_t  learned;            /* type/addr inferred mid-session, not from a conn event */
    uint8_t  addr[6];
    long     outstanding;        /* TX packets sent minus NOCP-credited */
    uint64_t last_nocp;          /* monotonic ms of the last NOCP for this handle */
    /* running per-handle counters; the GAP record reports deltas against the
     * snapshot taken when the gap opened */
    uint64_t tx31, tx32, tx36, tx39, tx_other, rx_pkts;
    uint64_t s_tx31, s_tx32, s_tx36, s_tx39, s_tx_other, s_rx, s_le_rx, s_le_tx,
             s_oth_rx, s_oth_tx;
    long     gap30, gap50, gap80;
    uint64_t gapmax;
};

static struct hstate g_h[NHANDLE];
static volatile sig_atomic_t g_stop;
/* Foreign-link airtime, split by how well we know it. Proven LE is the
 * Magic-Remote / LE-advertising channel 2.5 asks about; "oth" is traffic on
 * handles whose type is still unproven. Kept apart on purpose — merging them
 * would let unclassified BR/EDR chatter masquerade as an LE correlation. */
static uint64_t g_le_rx, g_le_tx, g_oth_rx, g_oth_tx;
static uint64_t g_gaps_total, g_ev_total;
static FILE *g_out;

static void on_sig(int s){ (void)s; g_stop=1; }

static uint64_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000u + (uint64_t)(ts.tv_nsec/1000000);
}
/* Wall clock in the same HH:MM:SS shape the daemon log carries, so a human can
 * line up a GAP record with /tmp/ds5_txd.log without any tooling. */
static void wall(char *buf, size_t n){
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
    struct tm tm; time_t t=(time_t)ts.tv_sec; localtime_r(&t,&tm);
    snprintf(buf,n,"%02d:%02d:%02d.%03d",tm.tm_hour,tm.tm_min,tm.tm_sec,
             (int)(ts.tv_nsec/1000000));
}
static const char *addrstr(const uint8_t a[6], char *b, size_t n){
    snprintf(b,n,"%02X:%02X:%02X:%02X:%02X:%02X",a[5],a[4],a[3],a[2],a[1],a[0]);
    return b;
}

static void emit(const char *kind, const char *fmt, ...)
    __attribute__((format(printf,2,3)));
static void emit(const char *kind, const char *fmt, ...){
    char w[32]; wall(w,sizeof w);
    fprintf(g_out,"%s %s ",kind,w);
    va_list ap; va_start(ap,fmt); vfprintf(g_out,fmt,ap); va_end(ap);
    fputc('\n',g_out);
    g_ev_total++;
}

/* A NOCP closed a starvation window on `hh`. Report the gap together with what
 * was actually on the air while it lasted — the whole point of the tool.
 *
 * `pre` is the outstanding count BEFORE this NOCP's credits were applied: a gap
 * only counts as starvation if we were in fact waiting for credits. That is the
 * same gate ds5_txd applies, so the two ledgers are comparable. */
static void close_gap(struct hstate *H, uint16_t hh, uint64_t gap, long pre){
    H->gapmax = gap>H->gapmax ? gap : H->gapmax;
    if(gap>=80)      H->gap80++;
    else if(gap>=50) H->gap50++;
    else             H->gap30++;
    g_gaps_total++;
    emit("GAP","h=0x%03x gap_ms=%llu out=%ld role=%s mode=%u%s "
               "in_gap tx31=%llu tx32=%llu tx36=%llu tx39=%llu txo=%llu rx=%llu "
               "le_rx=%llu le_tx=%llu oth_rx=%llu oth_tx=%llu",
         hh,(unsigned long long)gap,pre,
         H->have_role ? (H->role?"slave":"master") : "?",
         (unsigned)H->mode, H->learned?" [learned]":"",
         (unsigned long long)(H->tx31-H->s_tx31),
         (unsigned long long)(H->tx32-H->s_tx32),
         (unsigned long long)(H->tx36-H->s_tx36),
         (unsigned long long)(H->tx39-H->s_tx39),
         (unsigned long long)(H->tx_other-H->s_tx_other),
         (unsigned long long)(H->rx_pkts-H->s_rx),
         (unsigned long long)(g_le_rx-H->s_le_rx),
         (unsigned long long)(g_le_tx-H->s_le_tx),
         (unsigned long long)(g_oth_rx-H->s_oth_rx),
         (unsigned long long)(g_oth_tx-H->s_oth_tx));
}
static void snap(struct hstate *H){
    H->s_tx31=H->tx31; H->s_tx32=H->tx32; H->s_tx36=H->tx36; H->s_tx39=H->tx39;
    H->s_tx_other=H->tx_other; H->s_rx=H->rx_pkts;
    H->s_le_rx=g_le_rx; H->s_le_tx=g_le_tx;
    H->s_oth_rx=g_oth_rx; H->s_oth_tx=g_oth_tx;
}

static void handle_event(const uint8_t *e, int el){
    if(el<2) return;
    uint8_t code=e[0]; const uint8_t *p=e+2; int pl=el-2;
    char ab[24];
    switch(code){
    case EV_CONN_COMPLETE:
        if(pl<11 || p[0]!=0x00) return;   /* status,handle(2),bdaddr(6),link_type,encr */
        {   uint16_t hh=(uint16_t)((p[1]|(p[2]<<8))&0x0fff);
            struct hstate *H=&g_h[hh];
            memset(H,0,sizeof *H);
            memcpy(H->addr,p+3,6); H->known=1; H->last_nocp=now_ms();
            emit("EV","conn h=0x%03x addr=%s link_type=%u",hh,
                 addrstr(H->addr,ab,sizeof ab),(unsigned)p[9]);
        }
        return;
    case EV_DISCONN_COMPLETE:
        if(pl<4 || p[0]!=0x00) return;
        {   uint16_t hh=(uint16_t)((p[1]|(p[2]<<8))&0x0fff);
            emit("EV","disconn h=0x%03x reason=0x%02x gaps=%ld/%ld/%ld gmax=%llu",hh,
                 (unsigned)p[3],g_h[hh].gap30,g_h[hh].gap50,g_h[hh].gap80,
                 (unsigned long long)g_h[hh].gapmax);
            memset(&g_h[hh],0,sizeof g_h[hh]);
        }
        return;
    case EV_ROLE_CHANGE:
        /* status, bdaddr(6), new_role. This is the passive half of 2.4: no
         * Read_Role command needed, the controller announces every switch. */
        if(pl<8) return;
        for(int h=0;h<NHANDLE;h++)
            if(g_h[h].known && memcmp(g_h[h].addr,p+1,6)==0){
                g_h[h].have_role=1; g_h[h].role=p[7];
            }
        emit("EV","role_change status=0x%02x addr=%s new_role=%s",
             (unsigned)p[0],addrstr(p+1,ab,sizeof ab),p[7]?"slave":"master");
        return;
    case EV_MODE_CHANGE:
        if(pl<6 || p[0]!=0x00) return;
        {   uint16_t hh=(uint16_t)((p[1]|(p[2]<<8))&0x0fff);
            g_h[hh].mode=p[3];
            emit("EV","mode h=0x%03x mode=%u interval=%u",hh,(unsigned)p[3],
                 (unsigned)(p[4]|(p[5]<<8)));
        }
        return;
    case EV_MAX_SLOTS_CHANGE:
        /* T5 (packet-type mask) reads directly off this: 552B audio needs 3-DH3,
         * and a drop to 1 slot re-prices that lever. */
        if(pl<3) return;
        emit("EV","max_slots h=0x%03x slots=%u",
             (unsigned)((p[0]|(p[1]<<8))&0x0fff),(unsigned)p[2]);
        return;
    case EV_FLUSH_OCCURRED:
        if(pl<2) return;
        emit("EV","flush_occurred h=0x%03x",(unsigned)((p[0]|(p[1]<<8))&0x0fff));
        return;
    case EV_LE_META:
        if(pl<12) return;
        if((p[0]!=SUBEV_LE_CONN && p[0]!=SUBEV_LE_ENH_CONN) || p[1]!=0x00) return;
        {   uint16_t hh=(uint16_t)((p[2]|(p[3]<<8))&0x0fff);
            struct hstate *H=&g_h[hh];
            memset(H,0,sizeof *H);
            memcpy(H->addr,p+6,6); H->known=1; H->is_le=1;
            H->have_role=1; H->role=p[4]; H->last_nocp=now_ms();
            emit("EV","le_conn h=0x%03x addr=%s role=%s",hh,
                 addrstr(H->addr,ab,sizeof ab),p[4]?"slave":"master");
        }
        return;
    case EV_NUM_COMP_PKTS: {
        if(pl<1) return;
        int nh=p[0];
        if(pl < 1+nh*4) return;
        uint64_t t=now_ms();
        for(int i=0;i<nh;i++){
            uint16_t hh=(uint16_t)((p[1+i*4]|(p[2+i*4]<<8))&0x0fff);
            uint16_t cnt=(uint16_t)(p[3+i*4]|(p[4+i*4]<<8));
            struct hstate *H=&g_h[hh];
            long pre=H->outstanding;
            /* Starvation gate: a gap only counts while we were actually waiting
             * for credits. Without it, an idle link's silence would be logged as
             * a stall — the exact miscount the daemon's demand gate exists for. */
            if(pre>0 && H->last_nocp){
                uint64_t gap=t-H->last_nocp;
                if(gap>=GAP_MIN_MS) close_gap(H,hh,gap,pre);
            }
            H->outstanding = pre>(long)cnt ? pre-(long)cnt : 0;
            H->last_nocp=t;
            snap(H);
        }
        return;
    }
    default: return;
    }
}

static void handle_acl(const uint8_t *d, int dl, int tx){
    if(dl<4) return;
    uint16_t hh=(uint16_t)((d[0]|(d[1]<<8))&0x0fff);
    struct hstate *H=&g_h[hh];
    if(!H->known){                       /* attached mid-session: no conn event to learn from */
        memset(H,0,sizeof *H);
        H->known=1; H->learned=1; H->is_le=LT_UNKNOWN;
    }
    if(H->is_le==LT_UNKNOWN && (dl-4)>MTU_PROVES_BREDR){
        H->is_le=LT_BREDR;               /* payload no LE PDU can carry */
        H->last_nocp=now_ms();           /* start its gap clock here, not at t=0 */
        emit("EV","learn h=0x%03x type=bredr (acl_payload=%d, addr/role unknown — "
                  "start the sniffer before the pad connects for full attribution)",
             hh,dl-4);
    }
    if(H->is_le==LT_LE){      if(tx) g_le_tx++;  else g_le_rx++;  return; }
    if(H->is_le==LT_UNKNOWN){ if(tx) g_oth_tx++; else g_oth_rx++; return; }
    if(!tx){ H->rx_pkts++; return; }

    /* Every ACL TX packet consumes one controller credit, whatever it carries —
     * so outstanding is counted before any HID classification, or the ledger
     * drifts against the NOCPs that credit those same packets back. */
    H->outstanding++;
    if(!H->last_nocp) H->last_nocp=now_ms();

    /* HID classification for 2.3. Continuation fragments (PB=1) carry no L2CAP
     * header, so they are credit-relevant but not classifiable. */
    uint8_t pb=(uint8_t)((d[1]>>4)&0x3);
    if(pb==1 || dl<10){ H->tx_other++; return; }
    if(d[8]!=0xA2){ H->tx_other++; return; }   /* not a HID DATA/output transaction */
    switch(d[9]){
    case 0x31: H->tx31++; break;
    case 0x32: H->tx32++; break;
    case 0x36: H->tx36++; break;
    case 0x39: H->tx39++; break;
    default:   H->tx_other++; break;
    }
}

int main(int argc, char **argv){
    int index=0; long dur=0; const char *path="/tmp/ds5_sniff.log"; int sec_rows=1;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-i") && i+1<argc)      index=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-d") && i+1<argc) dur=atol(argv[++i]);
        else if(!strcmp(argv[i],"-o") && i+1<argc) path=argv[++i];
        else if(!strcmp(argv[i],"-q"))             sec_rows=0;
        else {
            fprintf(stderr,"usage: %s [-i hci_index] [-d seconds] [-o file] [-q]\n"
                           "       passive HCI monitor probe; sends nothing.\n",argv[0]);
            return 2;
        }
    }
    /* Never compete with the audio path we are measuring: the observer must not
     * change the observation. ds5_txd's own threads sit at nice -5/19. */
    setpriority(PRIO_PROCESS,0,19);

    g_out = strcmp(path,"-") ? fopen(path,"w") : stdout;
    if(!g_out){ perror("open out"); return 1; }
    setvbuf(g_out,NULL,_IOLBF,0);

    int mfd=socket(AF_BLUETOOTH,SOCK_RAW,BTPROTO_HCI);
    if(mfd<0){ perror("socket(monitor)"); return 1; }
    struct sockaddr_hci { unsigned short hci_family, hci_dev, hci_channel; } ma;
    memset(&ma,0,sizeof ma);
    ma.hci_family=AF_BLUETOOTH; ma.hci_dev=HCI_DEV_NONE; ma.hci_channel=HCI_CHANNEL_MONITOR;
    if(bind(mfd,(struct sockaddr*)&ma,sizeof ma)<0){ perror("bind(monitor)"); return 1; }
    struct timeval tv={.tv_sec=0,.tv_usec=200000};
    setsockopt(mfd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);

    signal(SIGINT,on_sig); signal(SIGTERM,on_sig); signal(SIGPIPE,SIG_IGN);

    char w[32]; wall(w,sizeof w);
    fprintf(g_out,"HDR %s ds5_sniff index=%d dur=%lds READ-ONLY (monitor channel, "
                  "zero HCI commands)\n",w,index,dur);

    uint64_t t0=now_ms(), tsec=t0;
    uint64_t p_tx31=0,p_tx36=0,p_tx39=0,p_rx=0,p_lerx=0,p_letx=0,p_oth=0,p_gaps=0;
    uint8_t buf[2048];
    while(!g_stop){
        int r=(int)recv(mfd,buf,sizeof buf,0);
        if(r>=(int)sizeof(struct hci_mon_hdr)){
            struct hci_mon_hdr *h=(struct hci_mon_hdr*)buf;
            uint8_t *d=buf+sizeof *h; int dl=h->len;
            if(h->index==(uint16_t)index && r>=(int)(sizeof *h+dl)){
                if(h->opcode==MON_EVENT_PKT)        handle_event(d,dl);
                else if(h->opcode==MON_ACL_TX_PKT)  handle_acl(d,dl,1);
                else if(h->opcode==MON_ACL_RX_PKT)  handle_acl(d,dl,0);
                else if(h->opcode==MON_COMMAND_PKT && dl>=4){
                    /* 2.4's other half: the initial role is chosen here, in the
                     * Accept_Connection_Request the stack sends when the PAD pages
                     * the TV. No Role_Change ever follows if the stack simply
                     * remains slave, so without this the role would stay unknown
                     * for exactly the case we suspect. */
                    uint16_t op=(uint16_t)(d[0]|(d[1]<<8));
                    char ab[24];
                    if(op==OP_ACCEPT_CONN_REQ && dl>=10)
                        emit("EV","cmd accept_conn addr=%s role=%s",
                             addrstr(d+3,ab,sizeof ab),d[9]?"slave":"master");
                    else if(op==OP_SWITCH_ROLE && dl>=10)
                        emit("EV","cmd switch_role addr=%s role=%s",
                             addrstr(d+3,ab,sizeof ab),d[9]?"slave":"master");
                }
            }
        } else if(r<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){
            usleep(50000);
        }

        uint64_t t=now_ms();
        if(sec_rows && t-tsec>=1000){
            uint64_t tx31=0,tx36=0,tx39=0,rx=0; long out=0; int nlinks=0;
            for(int i=0;i<NHANDLE;i++){
                struct hstate *H=&g_h[i];
                if(!H->known || H->is_le) continue;
                nlinks++; tx31+=H->tx31; tx36+=H->tx36; tx39+=H->tx39; rx+=H->rx_pkts;
                if(H->outstanding>out) out=H->outstanding;
            }
            double dt=(double)(t-tsec)/1000.0;
            emit("SEC","t=%.1f links=%d out_max=%ld tx31/s=%.1f tx36/s=%.1f tx39/s=%.1f "
                       "rx/s=%.1f le_rx/s=%.1f le_tx/s=%.1f oth/s=%.1f gaps/s=%.2f",
                 (double)(t-t0)/1000.0,nlinks,out,
                 (double)(tx31-p_tx31)/dt,(double)(tx36-p_tx36)/dt,
                 (double)(tx39-p_tx39)/dt,(double)(rx-p_rx)/dt,
                 (double)(g_le_rx-p_lerx)/dt,(double)(g_le_tx-p_letx)/dt,
                 (double)((g_oth_rx+g_oth_tx)-p_oth)/dt,
                 (double)(g_gaps_total-p_gaps)/dt);
            p_tx31=tx31; p_tx36=tx36; p_tx39=tx39; p_rx=rx;
                    p_lerx=g_le_rx; p_letx=g_le_tx; p_oth=g_oth_rx+g_oth_tx;
            p_gaps=g_gaps_total;
            tsec=t;
        }
        if(dur>0 && (t-t0)/1000 >= (uint64_t)dur) break;
    }

    for(int i=0;i<NHANDLE;i++){
        struct hstate *H=&g_h[i];
        if(!H->known) continue;
        char ab[24];
        emit("SUM","h=0x%03x %s%s addr=%s role=%s gaps=%ld/%ld/%ld gmax=%llu "
                   "tx31=%llu tx32=%llu tx36=%llu tx39=%llu txo=%llu rx=%llu",
             i,H->is_le==LT_LE?"le":H->is_le==LT_UNKNOWN?"unknown":"bredr",
             H->learned?"/learned":"",addrstr(H->addr,ab,sizeof ab),
             H->have_role?(H->role?"slave":"master"):"?",
             H->gap30,H->gap50,H->gap80,(unsigned long long)H->gapmax,
             (unsigned long long)H->tx31,(unsigned long long)H->tx32,
             (unsigned long long)H->tx36,(unsigned long long)H->tx39,
             (unsigned long long)H->tx_other,(unsigned long long)H->rx_pkts);
    }
    emit("SUM","run_s=%.1f gaps=%llu le_rx=%llu le_tx=%llu oth_rx=%llu oth_tx=%llu",
         (double)(now_ms()-t0)/1000.0,(unsigned long long)g_gaps_total,
         (unsigned long long)g_le_rx,(unsigned long long)g_le_tx,
         (unsigned long long)g_oth_rx,(unsigned long long)g_oth_tx);
    if(g_out!=stdout) fclose(g_out);
    return 0;
}
