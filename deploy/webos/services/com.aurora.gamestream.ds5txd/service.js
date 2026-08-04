/*
 * Aurora DS5 raw-ACL transport service.
 *
 * This exists for exactly one reason: ds5_txd needs to run as root (it opens an
 * HCI MONITOR socket and writes raw ACL frames), and the aurora app itself is
 * jailed as uid 6261. Shipping the daemon inside the IPK and letting Homebrew
 * Channel's elevate-service strip the jailer off THIS service's unit is what
 * removes the last hand-installed piece from the TV.
 *
 * Deliberately thin: it does NOT supervise ds5_txd itself. ds5-tmpld.sh next to
 * this file is the supervisor that has been running on the TV for months, and it
 * carries a lot of hard-won behaviour (atomic pidfile singleton, the SCHED_OTHER
 * demotion with the busybox relative-renice trap, log rotation with an in-run 1MB
 * cap, and the 10s socket-vanish window that must NOT race ds5_txd's in-process
 * jail-tmp self-heal). Reimplementing any of that in JS would only reintroduce
 * bugs, so we spawn it verbatim.
 *
 * The supervisor is spawned DETACHED on purpose. A Type=dynamic luna service is
 * reaped by ls-hubd once it goes idle; a normal child would die with it and take
 * the audio transport down mid-session. Detached, the transport outlives us and
 * the singleton pidfile inside ds5-tmpld.sh keeps repeated start calls harmless.
 */

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const Service = require('webos-service');

const SERVICE_ID = 'com.aurora.gamestream.ds5txd';
const service = new Service(SERVICE_ID);

const HERE = __dirname;
const SUPERVISOR = path.join(HERE, 'ds5-tmpld.sh');
const TXD_BIN = path.join(HERE, 'ds5_txd');
const PIDFILE = '/tmp/ds5-tmpld.pid';
const SOCK = '/var/palm/jail/com.aurora.gamestream/tmp/ds5_acl.sock';
const TMPL = '/var/palm/jail/com.aurora.gamestream/tmp/ds5_acl_tmpl';
const LOG = '/tmp/ds5_txd.log';

/* ds5_txd publishes its readiness record with a plain open()/write(), so the
 * inherited umask decides the mode. A 0600 record is invisible to the jailed app
 * and the readiness is then silently dead — the app just stays on hidraw forever
 * with no error anywhere. Pin 0022 here so the child cannot inherit anything
 * tighter from ls-hubd. */
process.umask(0o022);

function log(msg) {
    console.log('[ds5txd-svc] ' + msg);
}

/* Process identity via /proc rather than a bare kill(pid, 0): pids get recycled,
 * and a recycled pid that merely happens to be alive must not read as "the
 * transport is up" — that would strand the app on hidraw with a green status. */
function cmdlineOf(pid) {
    try {
        return fs.readFileSync('/proc/' + pid + '/cmdline', 'utf8').replace(/\0/g, ' ').trim();
    } catch (e) {
        return null;
    }
}

function supervisorPid() {
    let pid;
    try {
        pid = parseInt(fs.readFileSync(PIDFILE, 'utf8').trim(), 10);
    } catch (e) {
        return 0;
    }
    if (!pid) {
        return 0;
    }
    const cmd = cmdlineOf(pid);
    return cmd && cmd.indexOf('ds5-tmpld') !== -1 ? pid : 0;
}

function txdPid() {
    let entries;
    try {
        entries = fs.readdirSync('/proc');
    } catch (e) {
        return 0;
    }
    for (const name of entries) {
        if (!/^\d+$/.test(name)) {
            continue;
        }
        const cmd = cmdlineOf(name);
        if (cmd && cmd.indexOf('ds5_txd') !== -1 && cmd.indexOf('ds5-tmpld') === -1) {
            return parseInt(name, 10);
        }
    }
    return 0;
}

/* The readiness record the jailed app polls. Mirrors the client-side check in
 * ds5_acl_tx.c: 16 bytes, magic DS5T, version 1, bit0 of the flag byte set. */
function templateValid() {
    try {
        const fd = fs.openSync(TMPL, 'r');
        const buf = Buffer.alloc(16);
        const n = fs.readSync(fd, buf, 0, 16, 0);
        fs.closeSync(fd);
        return n === 16 && buf.toString('latin1', 0, 4) === 'DS5T' && buf[4] === 1 && (buf[5] & 1) === 1;
    } catch (e) {
        return false;
    }
}

function snapshot() {
    const sup = supervisorPid();
    const txd = txdPid();
    return {
        uid: process.getuid(),
        elevated: process.getuid() === 0,
        supervisorRunning: sup !== 0,
        supervisorPid: sup,
        txdRunning: txd !== 0,
        txdPid: txd,
        socket: fs.existsSync(SOCK),
        templateValid: templateValid(),
        binary: TXD_BIN,
        log: LOG,
    };
}

/* An IPK install does not reliably preserve the exec bit through every install
 * path (ares-install, luna dev/install, a hbchannel-side copy), and a daemon that
 * is merely non-executable fails in a way that looks exactly like "elevation
 * didn't work". Cheap to just assert it every start. */
function ensureExecutable() {
    for (const f of [SUPERVISOR, TXD_BIN]) {
        try {
            const mode = fs.statSync(f).mode & 0o777;
            if ((mode & 0o111) !== 0o111) {
                fs.chmodSync(f, 0o755);
                log('restored exec bit on ' + f);
            }
        } catch (e) {
            log('cannot stat ' + f + ': ' + e.message);
        }
    }
}

service.register('status', (message) => {
    message.respond(Object.assign({ returnValue: true }, snapshot()));
});

service.register('start', (message) => {
    if (process.getuid() !== 0) {
        /* We are the jailed instance. elevate-service rewrites the unit but does
         * not touch a process that is already holding the bus name, so as long as
         * this instance lives every call keeps landing here and the caller can
         * never reach an elevated one. Report it, then exit: ls-hubd starts the
         * service on demand, so aurora's retry gets a fresh process off the
         * rewritten (elevated) unit. Responding first matters — exiting with the
         * reply unsent would park the caller in its own reply wait. */
        message.respond(Object.assign(
            { returnValue: false, errorText: 'service is not elevated (still jailed) — retry after elevation' },
            snapshot()));
        log('running jailed (uid=' + process.getuid() + ') — exiting so ls-hubd can restart us elevated');
        setTimeout(() => process.exit(0), 250);
        return;
    }

    const running = supervisorPid();
    if (running) {
        message.respond(Object.assign({ returnValue: true, started: false, reason: 'already running' }, snapshot()));
        return;
    }

    ensureExecutable();

    if (!fs.existsSync(TXD_BIN)) {
        message.respond({ returnValue: false, errorText: 'daemon binary missing at ' + TXD_BIN });
        return;
    }

    let child;
    try {
        child = spawn('/bin/sh', [SUPERVISOR], {
            detached: true,
            stdio: 'ignore',
            env: Object.assign({}, process.env, { TXD_BIN: TXD_BIN }),
        });
        child.unref();
    } catch (e) {
        message.respond({ returnValue: false, errorText: 'spawn failed: ' + e.message });
        return;
    }

    log('supervisor spawned pid=' + child.pid + ' txd=' + TXD_BIN);

    /* The supervisor needs a moment to win the pidfile and for ds5_txd to bind
     * its socket. Report the settled state rather than an optimistic one, so the
     * app's log line reflects what actually happened. */
    setTimeout(() => {
        message.respond(Object.assign({ returnValue: true, started: true, spawnedPid: child.pid }, snapshot()));
    }, 1500);
});

service.register('stop', (message) => {
    if (process.getuid() !== 0) {
        message.respond({ returnValue: false, errorText: 'service is not elevated' });
        return;
    }
    const sup = supervisorPid();
    const txd = txdPid();
    /* Supervisor first: it respawns ds5_txd on death, so killing the daemon while
     * its supervisor lives just gets it restarted a second later. */
    if (sup) {
        try { process.kill(sup, 'SIGTERM'); } catch (e) { /* already gone */ }
    }
    if (txd) {
        try { process.kill(txd, 'SIGTERM'); } catch (e) { /* already gone */ }
    }
    try { fs.unlinkSync(PIDFILE); } catch (e) { /* nothing to clear */ }
    log('stopped supervisor=' + sup + ' txd=' + txd);
    setTimeout(() => {
        message.respond(Object.assign({ returnValue: true, stoppedSupervisor: sup, stoppedTxd: txd }, snapshot()));
    }, 300);
});

log('registered as ' + SERVICE_ID + ' uid=' + process.getuid() + ' dir=' + HERE);
