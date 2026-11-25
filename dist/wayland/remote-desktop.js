"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.RemoteDesktopClient = void 0;
const CLOSED = { state: 'closed', keyboard: false, pointer: false };
/**
 * Client side of the host's RemoteDesktop session. Keeps the latest restore
 * token so that a restarted host can resume the session without a dialog,
 * where the portal honors persistence.
 */
class RemoteDesktopClient {
    constructor(send) {
        this.send = send;
        this.status = CLOSED;
        this.token = null;
        this.wanted = false;
        this.statusListeners = new Set();
        this.tokenListeners = new Set();
    }
    /** The newest restore token, from `start` or from the portal. */
    get restoreToken() {
        return this.token;
    }
    /** Asks the portal for a session. The user is asked for approval unless
     *  `restoreToken` restores an earlier one. Returns false without a host. */
    start(restoreToken) {
        if (restoreToken !== undefined)
            this.token = validToken(restoreToken);
        this.wanted = true;
        return this.send(`remote-desktop start ${this.token ?? '-'}`);
    }
    stop() {
        this.wanted = false;
        this.send('remote-desktop stop');
    }
    /** Resumes a wanted session in a restarted host. */
    restarted() {
        if (this.wanted)
            this.send(`remote-desktop start ${this.token ?? '-'}`);
    }
    /** Forgets the session of a host that exited. */
    abandon() {
        if (this.status.state !== 'closed')
            this.update({ ...CLOSED, reason: 'host exited' });
    }
    handle(event) {
        if (event.ev === 'remote-desktop-token') {
            this.token = validToken(event.token);
            if (!this.token)
                return;
            for (const listener of this.tokenListeners)
                listener(this.token);
            return;
        }
        const { ev: _, ...status } = event;
        // A session the portal or the user ended is not resumed after a restart.
        if (status.state === 'closed')
            this.wanted = false;
        this.update(status);
    }
    onStatus(listener) {
        this.statusListeners.add(listener);
        return () => this.statusListeners.delete(listener);
    }
    onToken(listener) {
        this.tokenListeners.add(listener);
        return () => this.tokenListeners.delete(listener);
    }
    update(status) {
        this.status = status;
        for (const listener of this.statusListeners)
            listener(status);
    }
}
exports.RemoteDesktopClient = RemoteDesktopClient;
/** Tokens travel as one field of a command line. */
function validToken(token) {
    return token && /^[\x21-\x7e]+$/.test(token) ? token : null;
}
//# sourceMappingURL=remote-desktop.js.map