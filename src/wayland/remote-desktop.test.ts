import assert from 'node:assert/strict'
import { beforeEach, describe, it } from 'node:test'
import { RemoteDesktopClient, type RemoteDesktopEvent, type RemoteDesktopStatus } from './remote-desktop'

describe('RemoteDesktopClient', () => {
  let sent: string[]
  let connected: boolean
  let client: RemoteDesktopClient
  let statuses: RemoteDesktopStatus[]
  let tokens: string[]

  // Events as the host prints them.
  const receive = (line: string) => client.handle(JSON.parse(line) as RemoteDesktopEvent)

  beforeEach(() => {
    sent = []
    connected = true
    statuses = []
    tokens = []
    client = new RemoteDesktopClient((line) => {
      if (connected) sent.push(line)
      return connected
    })
    client.onStatus((status) => statuses.push(status))
    client.onToken((token) => tokens.push(token))
  })

  it('starts with the given token, or none', () => {
    client.start('abc-123')
    client.start(null)
    assert.deepEqual(sent, ['remote-desktop start abc-123', 'remote-desktop start -'])
  })

  it('drops tokens that cannot travel as one command field', () => {
    client.start('two words')
    assert.deepEqual(sent, ['remote-desktop start -'])
  })

  it('parses status events and reports device availability', () => {
    receive('{"ev":"remote-desktop","state":"starting","keyboard":false,"pointer":false}')
    receive('{"ev":"remote-desktop","state":"active","keyboard":true,"pointer":true}')
    assert.deepEqual(statuses.map((s) => s.state), ['starting', 'active'])
    assert.deepEqual(client.status, { state: 'active', keyboard: true, pointer: true })
  })

  it('reports and keeps new restore tokens', () => {
    client.start('old')
    receive('{"ev":"remote-desktop-token","token":"new"}')
    assert.deepEqual(tokens, ['new'])
    assert.equal(client.restoreToken, 'new')
    client.start()
    assert.equal(sent[sent.length - 1], 'remote-desktop start new')
  })

  it('resumes a wanted session with the newest token after a host restart', () => {
    client.start('old')
    receive('{"ev":"remote-desktop-token","token":"new"}')
    receive('{"ev":"remote-desktop","state":"active","keyboard":true,"pointer":false}')
    client.abandon()
    assert.deepEqual(client.status, { state: 'closed', keyboard: false, pointer: false, reason: 'host exited' })
    client.restarted()
    assert.equal(sent[sent.length - 1], 'remote-desktop start new')
  })

  it('does not resume a session the user cancelled or the app stopped', () => {
    client.start()
    receive('{"ev":"remote-desktop","state":"closed","keyboard":false,"pointer":false,"reason":"cancelled"}')
    client.restarted()
    client.start()
    client.stop()
    client.restarted()
    assert.deepEqual(sent, ['remote-desktop start -', 'remote-desktop start -', 'remote-desktop stop'])
    assert.equal(statuses[0].reason, 'cancelled')
  })

  it('starts once the host is back when it was down', () => {
    connected = false
    assert.equal(client.start('t'), false)
    connected = true
    client.restarted()
    assert.deepEqual(sent, ['remote-desktop start t'])
  })
})
