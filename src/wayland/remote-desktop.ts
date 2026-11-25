/** State of the host's RemoteDesktop portal session. `keyboard` and
 *  `pointer` tell whether keys and pointer warps can be sent through it. */
export interface RemoteDesktopStatus {
  state: 'starting' | 'active' | 'closed'
  keyboard: boolean
  pointer: boolean
  /** Why a session closed: `cancelled` (the user declined), `stopped`,
   *  `closed` (by the portal), `disconnected` (by EIS), `unavailable`,
   *  `host exited`, or a failure message. */
  reason?: string
}

/** Events the host sends about the session. See src/lib/wayland/README.md. */
export type RemoteDesktopEvent =
  | ({ ev: 'remote-desktop' } & RemoteDesktopStatus)
  | { ev: 'remote-desktop-token'; token: string }

const CLOSED: RemoteDesktopStatus = { state: 'closed', keyboard: false, pointer: false }

/**
 * Client side of the host's RemoteDesktop session. Keeps the latest restore
 * token so that a restarted host can resume the session without a dialog,
 * where the portal honors persistence.
 */
export class RemoteDesktopClient {
  status: RemoteDesktopStatus = CLOSED
  private token: string | null = null
  private wanted = false
  private readonly statusListeners = new Set<(status: RemoteDesktopStatus) => void>()
  private readonly tokenListeners = new Set<(token: string) => void>()

  constructor (private readonly send: (line: string) => boolean) {}

  /** The newest restore token, from `start` or from the portal. */
  get restoreToken (): string | null {
    return this.token
  }

  /** Asks the portal for a session. The user is asked for approval unless
   *  `restoreToken` restores an earlier one. Returns false without a host. */
  start (restoreToken?: string | null): boolean {
    if (restoreToken !== undefined) this.token = validToken(restoreToken)
    this.wanted = true
    return this.send(`remote-desktop start ${this.token ?? '-'}`)
  }

  stop (): void {
    this.wanted = false
    this.send('remote-desktop stop')
  }

  /** Resumes a wanted session in a restarted host. */
  restarted (): void {
    if (this.wanted) this.send(`remote-desktop start ${this.token ?? '-'}`)
  }

  /** Forgets the session of a host that exited. */
  abandon (): void {
    if (this.status.state !== 'closed') this.update({ ...CLOSED, reason: 'host exited' })
  }

  handle (event: RemoteDesktopEvent): void {
    if (event.ev === 'remote-desktop-token') {
      this.token = validToken(event.token)
      if (!this.token) return
      for (const listener of this.tokenListeners) listener(this.token)
      return
    }
    const { ev: _, ...status } = event
    // A session the portal or the user ended is not resumed after a restart.
    if (status.state === 'closed') this.wanted = false
    this.update(status)
  }

  onStatus (listener: (status: RemoteDesktopStatus) => void): () => void {
    this.statusListeners.add(listener)
    return () => this.statusListeners.delete(listener)
  }

  onToken (listener: (token: string) => void): () => void {
    this.tokenListeners.add(listener)
    return () => this.tokenListeners.delete(listener)
  }

  private update (status: RemoteDesktopStatus): void {
    this.status = status
    for (const listener of this.statusListeners) listener(status)
  }
}

/** Tokens travel as one field of a command line. */
function validToken (token: string | null): string | null {
  return token && /^[\x21-\x7e]+$/.test(token) ? token : null
}
