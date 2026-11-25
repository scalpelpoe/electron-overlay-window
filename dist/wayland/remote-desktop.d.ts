/** State of the host's RemoteDesktop portal session. `keyboard` and
 *  `pointer` tell whether keys and pointer warps can be sent through it. */
export interface RemoteDesktopStatus {
    state: 'starting' | 'active' | 'closed';
    keyboard: boolean;
    pointer: boolean;
    /** Why a session closed: `cancelled` (the user declined), `stopped`,
     *  `closed` (by the portal), `disconnected` (by EIS), `unavailable`,
     *  `host exited`, or a failure message. */
    reason?: string;
}
/** Events the host sends about the session. See src/lib/wayland/README.md. */
export type RemoteDesktopEvent = ({
    ev: 'remote-desktop';
} & RemoteDesktopStatus) | {
    ev: 'remote-desktop-token';
    token: string;
};
/**
 * Client side of the host's RemoteDesktop session. Keeps the latest restore
 * token so that a restarted host can resume the session without a dialog,
 * where the portal honors persistence.
 */
export declare class RemoteDesktopClient {
    private readonly send;
    status: RemoteDesktopStatus;
    private token;
    private wanted;
    private readonly statusListeners;
    private readonly tokenListeners;
    constructor(send: (line: string) => boolean);
    /** The newest restore token, from `start` or from the portal. */
    get restoreToken(): string | null;
    /** Asks the portal for a session. The user is asked for approval unless
     *  `restoreToken` restores an earlier one. Returns false without a host. */
    start(restoreToken?: string | null): boolean;
    stop(): void;
    /** Resumes a wanted session in a restarted host. */
    restarted(): void;
    /** Forgets the session of a host that exited. */
    abandon(): void;
    handle(event: RemoteDesktopEvent): void;
    onStatus(listener: (status: RemoteDesktopStatus) => void): () => void;
    onToken(listener: (token: string) => void): () => void;
    private update;
}
