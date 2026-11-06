package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	bolt "go.etcd.io/bbolt"
)

type testEnv struct {
	t     *testing.T
	srv   *httptest.Server
	hub   *Hub
	rv    *Rendezvous
	s     *Server
	store *Store
}

func newEnv(t *testing.T) *testEnv { return newEnvWith(t, nil, false) }

// newEnvWith lets a test adjust the config, and keep the real rate limits.
func newEnvWith(t *testing.T, tune func(*Config), realLimits bool) *testEnv {
	t.Helper()
	store, err := OpenStore(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { store.Close() })
	hub := NewHub()
	rv, err := NewRendezvous("127.0.0.1:0", hub)
	if err != nil {
		t.Fatal(err)
	}
	go rv.Serve()
	t.Cleanup(func() { rv.Close() })
	cfg := Config{PublicURL: "http://rincast.test", RendezvousHost: "127.0.0.1",
		RendezvousPort: rv.LocalAddr().Port, AllowSignups: true}
	if tune != nil {
		tune(&cfg)
	}
	s := NewServer(cfg, store, hub, []byte("0123456789abcdef0123456789abcdef"))
	if !realLimits {
		s.limiter = NewRateLimiter(1000, time.Minute)
		s.linkLimiter = NewRateLimiter(1000, time.Minute)
		s.pollLimiter = NewRateLimiter(1000, time.Minute)
		s.connectLimiter = NewRateLimiter(1000, time.Minute)
		s.pcLimiter = NewRateLimiter(1000, time.Minute)
	}
	ts := httptest.NewServer(s.Routes())
	t.Cleanup(ts.Close)
	return &testEnv{t: t, srv: ts, hub: hub, rv: rv, s: s, store: store}
}

// userToken adds an account directly, skipping the password hashing, and returns
// an access token for it.
func (e *testEnv) userToken(email string) (string, *User) {
	e.t.Helper()
	u := &User{ID: randomID(), Email: email, Name: "Test", PasswordHash: "unset", CreatedAt: time.Now().UTC()}
	if err := e.store.CreateUser(u, 0); err != nil {
		e.t.Fatal(err)
	}
	return e.accessToken(u, time.Now()), u
}

func (e *testEnv) accessToken(u *User, issued time.Time) string {
	return e.s.signer.Sign(Claims{Subject: u.ID, DeviceKind: "web", IssuedAt: issued.Unix(), ExpiresAt: issued.Add(time.Hour).Unix()})
}

// status makes a request and returns the status code, headers and raw body.
func (e *testEnv) status(method, path, token string, body any) (int, http.Header, []byte) {
	e.t.Helper()
	var buf bytes.Buffer
	if body != nil {
		_ = json.NewEncoder(&buf).Encode(body)
	}
	req, _ := http.NewRequest(method, e.srv.URL+"/api/v1"+path, &buf)
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	defer res.Body.Close()
	data, _ := io.ReadAll(res.Body)
	return res.StatusCode, res.Header, data
}

func (e *testEnv) call(method, path, token string, body any, wantStatus int) map[string]any {
	e.t.Helper()
	var buf bytes.Buffer
	if body != nil {
		_ = json.NewEncoder(&buf).Encode(body)
	}
	req, _ := http.NewRequest(method, e.srv.URL+"/api/v1"+path, &buf)
	req.Header.Set("Content-Type", "application/json")
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	defer res.Body.Close()
	var out map[string]any
	_ = json.NewDecoder(res.Body).Decode(&out)
	if res.StatusCode != wantStatus {
		e.t.Fatalf("%s %s: status %d (want %d): %v", method, path, res.StatusCode, wantStatus, out)
	}
	return out
}

func TestPasswordHashing(t *testing.T) {
	h, err := HashPassword("correct horse")
	if err != nil {
		t.Fatal(err)
	}
	if !VerifyPassword(h, "correct horse") || VerifyPassword(h, "wrong") {
		t.Fatal("password verification mismatch")
	}
}

func TestUserCodes(t *testing.T) {
	c := randomUserCode()
	if len(c) != 9 || c[4] != '-' {
		t.Fatalf("bad code %q", c)
	}
	if normalizeUserCode(strings.ToLower(strings.ReplaceAll(c, "-", " "))) != c {
		t.Fatal("normalization failed")
	}
}

func TestFullFlow(t *testing.T) {
	e := newEnv(t)

	// Register + duplicate + login.
	reg := e.call("POST", "/auth/register", "", map[string]any{"email": "Ana@Example.com", "password": "hunter2hunter2", "name": "Ana", "deviceKind": "pc", "deviceId": "pc-1"}, 201)
	e.call("POST", "/auth/register", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2"}, 409)
	e.call("POST", "/auth/login", "", map[string]any{"email": "ana@example.com", "password": "nope"}, 401)
	login := e.call("POST", "/auth/login", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2", "deviceKind": "pc", "deviceId": "pc-1"}, 200)
	pcToken := login["accessToken"].(string)

	// Refresh rotates the token; the old one stops working.
	ref := e.call("POST", "/auth/refresh", "", map[string]any{"refreshToken": reg["refreshToken"]}, 200)
	e.call("POST", "/auth/refresh", "", map[string]any{"refreshToken": reg["refreshToken"]}, 401)
	if ref["accessToken"] == "" {
		t.Fatal("missing access token")
	}

	// PC registration and presence.
	e.call("POST", "/pcs", pcToken, map[string]any{"id": "pc-1", "name": "Desk", "gpu": "RTX", "localAddrs": []string{"192.168.1.5:38711"}}, 200)
	pcs := e.call("GET", "/pcs", pcToken, nil, 200)["pcs"].([]any)
	if len(pcs) != 1 || pcs[0].(map[string]any)["online"] != true {
		t.Fatalf("unexpected pcs %v", pcs)
	}

	// Headset device-code sign-in.
	dc := e.call("POST", "/device/code", "", map[string]any{"deviceId": "hs-1", "deviceName": "Quest 3", "model": "Quest 3"}, 200)
	pending := e.call("POST", "/device/token", "", map[string]any{"deviceCode": dc["deviceCode"]}, 400)
	if pending["error"] != "authorization_pending" {
		t.Fatalf("expected pending, got %v", pending)
	}
	e.call("GET", "/device/lookup?code="+dc["userCode"].(string), pcToken, nil, 200)
	e.call("POST", "/device/approve", pcToken, map[string]any{"userCode": strings.ToLower(dc["userCode"].(string))}, 200)
	// Respect polling interval.
	e.hub.mu.Lock()
	e.hub.deviceCodes[dc["deviceCode"].(string)].LastPoll = time.Time{}
	e.hub.mu.Unlock()
	hsTok := e.call("POST", "/device/token", "", map[string]any{"deviceCode": dc["deviceCode"]}, 200)
	headsetToken := hsTok["accessToken"].(string)
	e.call("POST", "/device/token", "", map[string]any{"deviceCode": dc["deviceCode"]}, 400) // consumed

	hs := e.call("GET", "/headsets", headsetToken, nil, 200)["headsets"].([]any)
	if len(hs) != 1 {
		t.Fatalf("expected headset, got %v", hs)
	}

	// Connect: headset requests, PC receives the event via long-poll.
	eventsCh := make(chan map[string]any, 1)
	go func() { eventsCh <- e.call("GET", "/pcs/pc-1/events?wait=5", pcToken, nil, 200) }()
	time.Sleep(100 * time.Millisecond)
	conn := e.call("POST", "/connect", headsetToken, map[string]any{"pcId": "pc-1"}, 200)
	evs := (<-eventsCh)["events"].([]any)
	if len(evs) != 1 {
		t.Fatalf("expected one event, got %v", evs)
	}
	ev := evs[0].(map[string]any)["data"].(map[string]any)
	if ev["sessionId"] != conn["sessionId"] || ev["connectSecret"] != conn["connectSecret"] {
		t.Fatal("session mismatch between PC and headset")
	}

	// Another user cannot see or connect to the PC.
	other := e.call("POST", "/auth/register", "", map[string]any{"email": "bob@example.com", "password": "hunter2hunter2"}, 201)
	e.call("POST", "/connect", other["accessToken"].(string), map[string]any{"pcId": "pc-1"}, 404)
	e.call("POST", "/pcs", other["accessToken"].(string), map[string]any{"id": "pc-1", "name": "Stolen"}, 409)

	// Rendezvous: both sides register and learn each other (observed endpoint + reported candidates).
	rvAddr := e.rv.LocalAddr()
	pcSock, _ := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	hsSock, _ := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	defer pcSock.Close()
	defer hsSock.Close()

	register := func(sock *net.UDPConn, role int, token string, candidates ...string) {
		m, _ := json.Marshal(rvMessage{Op: "register", Session: conn["sessionId"].(string), Role: role, Token: token, Candidates: candidates})
		_, _ = sock.WriteToUDP(append([]byte("RCRV"), m...), rvAddr)
	}
	read := func(sock *net.UDPConn) []byte {
		_ = sock.SetReadDeadline(time.Now().Add(2 * time.Second))
		buf := make([]byte, 2048)
		n, _, err := sock.ReadFromUDP(buf)
		if err != nil {
			t.Fatalf("udp read: %v", err)
		}
		return buf[:n]
	}
	register(pcSock, rolePC, ev["relayToken"].(string), "192.168.1.20:38711", "[2001:db8::20]:38711", "evil.example:1", "0.0.0.0:5")
	first := read(pcSock)
	if !bytes.Contains(first, []byte(`"registered"`)) {
		t.Fatalf("unexpected %s", first)
	}
	register(hsSock, roleHeadset, conn["relayToken"].(string))
	gotPeerPC := read(pcSock)
	gotPeerHS := read(hsSock)
	if !bytes.Contains(gotPeerPC, []byte(hsSock.LocalAddr().String())) || !bytes.Contains(gotPeerHS, []byte(pcSock.LocalAddr().String())) {
		t.Fatalf("peer exchange failed: %s / %s", gotPeerPC, gotPeerHS)
	}
	// The headset also learns the candidates the PC reported; junk is filtered out.
	var hsMsg rvMessage
	_ = json.Unmarshal(gotPeerHS[4:], &hsMsg)
	want := map[string]bool{pcSock.LocalAddr().String(): false, "192.168.1.20:38711": false, "[2001:db8::20]:38711": false}
	for _, p := range hsMsg.Peers {
		if _, ok := want[p]; !ok {
			t.Fatalf("unexpected peer candidate %q in %v", p, hsMsg.Peers)
		}
		want[p] = true
	}
	for k, seen := range want {
		if !seen {
			t.Fatalf("missing peer candidate %q in %v", k, hsMsg.Peers)
		}
	}
	// Bad token is rejected.
	register(hsSock, roleHeadset, strings.Repeat("0", 64))
	if !bytes.Contains(read(hsSock), []byte("invalid_session")) {
		t.Fatal("bogus token accepted")
	}
	// Nothing is relayed: stream datagrams sent to the rendezvous go nowhere.
	_, _ = hsSock.WriteToUDP([]byte("hello-pc"), rvAddr)
	_ = pcSock.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	if n, _, err := pcSock.ReadFromUDP(make([]byte, 64)); err == nil {
		t.Fatalf("server relayed a datagram (%d bytes)", n)
	}

	// Unlinking the PC.
	e.call("DELETE", "/pcs/pc-1", pcToken, nil, 200)
	if pcs := e.call("GET", "/pcs", pcToken, nil, 200)["pcs"].([]any); len(pcs) != 0 {
		t.Fatal("pc not deleted")
	}
}

func TestDeleteAccount(t *testing.T) {
	e := newEnv(t)
	reg := e.call("POST", "/auth/register", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2", "deviceKind": "pc", "deviceId": "pc-1"}, 201)
	tok := reg["accessToken"].(string)
	e.call("POST", "/pcs", tok, map[string]any{"id": "pc-1", "name": "Desk", "localAddrs": []string{"192.168.1.5:38711"}}, 200)
	id := reg["user"].(map[string]any)["id"].(string)
	if err := e.store.AddHeadset(&Headset{ID: "hs-1", UserID: id, Name: "Quest"}, maxHeadsetsPerUser); err != nil {
		t.Fatal(err)
	}

	// Another account must be left alone.
	other := e.call("POST", "/auth/register", "", map[string]any{"email": "bob@example.com", "password": "hunter2hunter2"}, 201)
	e.call("POST", "/pcs", other["accessToken"].(string), map[string]any{"id": "pc-2", "name": "Bob's"}, 200)

	// Wrong password: 403, not 401, which the site would read as an expired session.
	bad := e.call("DELETE", "/me", tok, map[string]any{"password": "wrong-password"}, 403)
	if bad["message"] == "" || bad["message"] == nil {
		t.Fatalf("no message in %v", bad)
	}
	e.call("DELETE", "/me", "", map[string]any{"password": "hunter2hunter2"}, 401)
	e.call("GET", "/me", tok, nil, 200)

	e.call("DELETE", "/me", tok, map[string]any{"password": "hunter2hunter2"}, 200)

	// Everything is gone: sign-in, refresh, and the access token still in hand.
	e.call("POST", "/auth/login", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2"}, 401)
	e.call("POST", "/auth/refresh", "", map[string]any{"refreshToken": reg["refreshToken"]}, 401)
	e.call("GET", "/me", tok, nil, 401)
	e.call("POST", "/pcs", tok, map[string]any{"id": "pc-3", "name": "Ghost"}, 401)
	if pcs, _ := e.store.PCsForUser(id); len(pcs) != 0 {
		t.Fatalf("PCs left: %v", pcs)
	}
	if hs, _ := e.store.HeadsetsForUser(id); len(hs) != 0 {
		t.Fatalf("headsets left: %v", hs)
	}
	if ts, _ := e.store.RefreshTokensForUser(id); len(ts) != 0 {
		t.Fatalf("tokens left: %v", ts)
	}

	// The linked PC is told, and forgets its addresses.
	evs := e.hub.WaitEvents("pc-1", 0, nil)
	if len(evs) != 1 || evs[0].Type != "unlinked" {
		t.Fatalf("expected unlinked event, got %v", evs)
	}
	if a := e.hub.LocalAddrs("pc-1"); len(a) != 0 {
		t.Fatalf("addresses kept: %v", a)
	}

	if pcs := e.call("GET", "/pcs", other["accessToken"].(string), nil, 200)["pcs"].([]any); len(pcs) != 1 {
		t.Fatalf("other account's PC affected: %v", pcs)
	}

	// The email and the PC id are free again.
	again := e.call("POST", "/auth/register", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2"}, 201)
	e.call("POST", "/pcs", again["accessToken"].(string), map[string]any{"id": "pc-1", "name": "Desk"}, 200)
}

func TestExportAccount(t *testing.T) {
	e := newEnv(t)
	reg := e.call("POST", "/auth/register", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2", "name": "Ana", "deviceKind": "pc", "deviceId": "pc-1", "deviceName": "Desk"}, 201)
	tok := reg["accessToken"].(string)
	e.call("POST", "/pcs", tok, map[string]any{"id": "pc-1", "name": "Desk", "gpu": "RTX", "localAddrs": []string{"192.168.1.5:38711"}}, 200)
	u, _ := e.store.UserByEmail("ana@example.com")
	_ = e.store.AddHeadset(&Headset{ID: "hs-1", UserID: u.ID, Name: "Quest"}, maxHeadsetsPerUser)

	e.call("GET", "/me/export", "", nil, 401)
	code, _, raw := e.status("GET", "/me/export", tok, nil)
	if code != 200 {
		t.Fatalf("status %d: %s", code, raw)
	}
	var out struct {
		User     map[string]any   `json:"user"`
		PCs      []map[string]any `json:"pcs"`
		Headsets []map[string]any `json:"headsets"`
		Sessions []map[string]any `json:"sessions"`
	}
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatal(err)
	}
	if out.User["email"] != "ana@example.com" || len(out.PCs) != 1 || len(out.Headsets) != 1 || len(out.Sessions) != 1 {
		t.Fatalf("incomplete export: %s", raw)
	}
	if out.PCs[0]["name"] != "Desk" || out.Sessions[0]["deviceName"] != "Desk" {
		t.Fatalf("incomplete export: %s", raw)
	}
	for _, secret := range []string{u.PasswordHash, "passwordHash", "password", sha256Hex(reg["refreshToken"].(string)), reg["refreshToken"].(string), "sessionId"} {
		if bytes.Contains(raw, []byte(secret)) {
			t.Fatalf("export contains %q: %s", secret, raw)
		}
	}

	// An account with nothing linked exports empty lists, not null.
	empty, _ := e.userToken("empty@example.com")
	_, _, raw = e.status("GET", "/me/export", empty, nil)
	if bytes.Contains(raw, []byte("null")) {
		t.Fatalf("null in export: %s", raw)
	}
}

func TestServerInfo(t *testing.T) {
	e := newEnv(t)
	info := e.call("GET", "/info", "", nil, 200)
	if _, ok := info["privacyUrl"]; ok {
		t.Fatalf("unset privacyUrl present: %v", info)
	}
	if _, ok := info["imprintUrl"]; ok {
		t.Fatalf("unset imprintUrl present: %v", info)
	}
	if info["allowSignups"] != true || info["publicUrl"] != "http://rincast.test" {
		t.Fatalf("unexpected info: %v", info)
	}

	e = newEnvWith(t, func(c *Config) {
		c.PrivacyURL = "https://example.com/privacy"
		c.ImprintURL = "https://example.com/imprint"
	}, false)
	info = e.call("GET", "/info", "", nil, 200)
	if info["privacyUrl"] != "https://example.com/privacy" || info["imprintUrl"] != "https://example.com/imprint" {
		t.Fatalf("unexpected info: %v", info)
	}
}

func TestChangePasswordRevokesOtherSessions(t *testing.T) {
	e := newEnv(t)
	reg := e.call("POST", "/auth/register", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2"}, 201)
	other := e.call("POST", "/auth/login", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2", "deviceKind": "headset", "deviceId": "hs-1"}, 200)
	u, _ := e.store.UserByEmail("ana@example.com")
	stolen := e.accessToken(u, time.Now().Add(-time.Minute))

	// The session id survives rotation, so this still counts as the same session.
	cur := e.call("POST", "/auth/refresh", "", map[string]any{"refreshToken": reg["refreshToken"]}, 200)
	tok := cur["accessToken"].(string)

	e.call("PATCH", "/me", tok, map[string]any{"currentPassword": "wrong-password", "newPassword": "brand-new-password"}, 403)
	e.call("PATCH", "/me", tok, map[string]any{"currentPassword": "hunter2hunter2", "newPassword": "short"}, 400)
	e.call("PATCH", "/me", tok, map[string]any{"currentPassword": "hunter2hunter2", "newPassword": strings.Repeat("x", 257)}, 400)
	e.call("GET", "/me", stolen, nil, 200)
	e.call("POST", "/auth/refresh", "", map[string]any{"refreshToken": other["refreshToken"]}, 200)
	other = e.call("POST", "/auth/login", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2", "deviceKind": "headset", "deviceId": "hs-1"}, 200)

	e.call("PATCH", "/me", tok, map[string]any{"currentPassword": "hunter2hunter2", "newPassword": "brand-new-password"}, 200)

	e.call("POST", "/auth/refresh", "", map[string]any{"refreshToken": other["refreshToken"]}, 401)
	e.call("GET", "/me", stolen, nil, 401)
	e.call("POST", "/auth/login", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2"}, 401)
	e.call("POST", "/auth/login", "", map[string]any{"email": "ana@example.com", "password": "brand-new-password"}, 200)

	// The session that changed it carries on.
	renewed := e.call("POST", "/auth/refresh", "", map[string]any{"refreshToken": cur["refreshToken"]}, 200)
	e.call("GET", "/me", renewed["accessToken"].(string), nil, 200)

	// Changing only the name signs nobody out.
	e.call("PATCH", "/me", renewed["accessToken"].(string), map[string]any{"name": "Ana"}, 200)
	e.call("POST", "/auth/refresh", "", map[string]any{"refreshToken": renewed["refreshToken"]}, 200)
}

func TestResetPasswordCommand(t *testing.T) {
	dir := t.TempDir()
	var out bytes.Buffer
	if err := resetPassword(dir, "ana@example.com", &out); err == nil || !strings.Contains(err.Error(), "no database") {
		t.Fatalf("expected a missing database error, got %v", err)
	}

	path := filepath.Join(dir, "rincast.db")
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	hash, _ := HashPassword("old-password")
	u := &User{ID: randomID(), Email: "ana@example.com", Name: "Ana", PasswordHash: hash, CreatedAt: time.Now().UTC()}
	if err := store.CreateUser(u, 0); err != nil {
		t.Fatal(err)
	}
	_ = store.PutRefreshToken("h1", &RefreshToken{UserID: u.ID, SessionID: "s1", ExpiresAt: time.Now().Add(time.Hour)})

	// While the database is open (the server is running) it says what to do.
	if err := resetPassword(dir, "ana@example.com", &out); err == nil || !strings.Contains(err.Error(), "stop the server") {
		t.Fatalf("expected a locked database error, got %v", err)
	}
	store.Close()

	if err := resetPassword(dir, "nobody@example.com", &out); err == nil || !strings.Contains(err.Error(), "no account") {
		t.Fatalf("expected a no account error, got %v", err)
	}
	out.Reset()
	if err := resetPassword(dir, "  ANA@example.com", &out); err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`New password for ana@example\.com: (\S+)`).FindStringSubmatch(out.String())
	if m == nil {
		t.Fatalf("unexpected output %q", out.String())
	}

	store, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	got, _ := store.UserByEmail("ana@example.com")
	if !VerifyPassword(got.PasswordHash, m[1]) || VerifyPassword(got.PasswordHash, "old-password") {
		t.Fatal("password not replaced")
	}
	if len(m[1]) < 20 || !validPassword(m[1]) {
		t.Fatalf("weak generated password %q", m[1])
	}
	if ts, _ := store.RefreshTokensForUser(u.ID); len(ts) != 0 {
		t.Fatalf("sessions not revoked: %v", ts)
	}
	if got.TokensValidAfter == 0 {
		t.Fatal("access tokens not revoked")
	}
}

func TestRateLimits(t *testing.T) {
	e := newEnvWith(t, nil, true)
	tok, _ := e.userToken("ana@example.com")

	// exhausts a route, which must allow at least free requests first.
	exhausts := func(method, path, token string, body any, free int) {
		t.Helper()
		for i := 0; i < 2000; i++ {
			code, hdr, raw := e.status(method, path, token, body)
			if code != 429 {
				continue
			}
			if i < free {
				t.Fatalf("%s %s limited after %d requests, want at least %d", method, path, i, free)
			}
			var msg apiError
			_ = json.Unmarshal(raw, &msg)
			if msg.Message == "" || hdr.Get("Retry-After") == "" {
				t.Fatalf("%s %s: unhelpful 429: %s", method, path, raw)
			}
			return
		}
		t.Fatalf("%s %s is not rate limited", method, path)
	}
	exhausts("POST", "/device/token", "", map[string]any{"deviceCode": "x"}, 100)
	exhausts("GET", "/device/lookup?code=AAAA-AAAA", tok, nil, 10)
	exhausts("POST", "/device/approve", tok, map[string]any{"userCode": "AAAA-AAAA"}, 0)
	exhausts("POST", "/connect", tok, map[string]any{"pcId": "x"}, 20)
	exhausts("GET", "/pcs", tok, nil, 200)
	exhausts("POST", "/pcs", tok, map[string]any{}, 0)

	// A headset polling at its normal interval (20 a minute) stays well inside
	// the limit, also with several headsets behind one address.
	if perMinute := int(e.s.pollLimiter.refill * 60); perMinute < 5*20 {
		t.Fatalf("poll limit of %d/min is too tight", perMinute)
	}
	// Limited routes also stop unauthenticated guessing, before the token check.
	if code, _, _ := e.status("POST", "/connect", "", map[string]any{"pcId": "x"}); code != 429 {
		t.Fatalf("expected 429 without a token, got %d", code)
	}
}

func TestLimiterStaysBounded(t *testing.T) {
	l := NewRateLimiter(10, time.Minute)
	for i := 0; i < maxTrackedKeys+100; i++ {
		l.Allow(fmt.Sprint(i))
	}
	if n := len(l.buckets); n > maxTrackedKeys {
		t.Fatalf("limiter holds %d keys", n)
	}
}

func TestClientIP(t *testing.T) {
	trusted := &Server{cfg: Config{TrustProxy: true}}
	direct := &Server{}
	req := func(headers ...string) *http.Request {
		r := httptest.NewRequest("GET", "/", nil)
		r.RemoteAddr = "10.0.0.1:1234"
		for _, h := range headers {
			r.Header.Add("X-Forwarded-For", h)
		}
		return r
	}
	for _, tc := range []struct {
		s       *Server
		headers []string
		want    string
	}{
		{direct, nil, "10.0.0.1"},
		{direct, []string{"1.2.3.4"}, "10.0.0.1"},
		{trusted, nil, "10.0.0.1"},
		{trusted, []string{"6.6.6.6, 1.2.3.4"}, "1.2.3.4"},
		{trusted, []string{"6.6.6.6", "1.2.3.4"}, "1.2.3.4"}, // a forged first header must not win
	} {
		if got := tc.s.clientIP(req(tc.headers...)); got != tc.want {
			t.Fatalf("clientIP(%v) = %q, want %q", tc.headers, got, tc.want)
		}
	}
	if limitKey("2001:db8:1:2:aaaa::1") != limitKey("2001:db8:1:2:bbbb::9") {
		t.Fatal("addresses in one /64 should share a limit")
	}
	if limitKey("2001:db8:1:2::1") == limitKey("2001:db8:1:3::1") {
		t.Fatal("different /64s should not share a limit")
	}
	if limitKey("::ffff:1.2.3.4") != "1.2.3.4" {
		t.Fatal("IPv4-mapped addresses should count as IPv4")
	}
}

func TestLoginBackoff(t *testing.T) {
	e := newEnv(t)
	e.call("POST", "/auth/register", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2"}, 201)

	// Existing and unknown accounts behave the same at every step.
	login := func(email, password string) (int, string, string) {
		code, hdr, raw := e.status("POST", "/auth/login", "", map[string]any{"email": email, "password": password})
		if code == 200 {
			return code, "", hdr.Get("Retry-After")
		}
		return code, string(raw), hdr.Get("Retry-After")
	}
	for i := 0; i < loginFreeAttempts; i++ {
		c1, b1, _ := login("ana@example.com", "wrong-password")
		c2, b2, _ := login("nobody@example.com", "wrong-password")
		if c1 != 401 || c1 != c2 || b1 != b2 {
			t.Fatalf("attempt %d differs: %d %s / %d %s", i, c1, b1, c2, b2)
		}
	}
	c1, b1, r1 := login("ana@example.com", "wrong-password")
	c2, b2, r2 := login("nobody@example.com", "wrong-password")
	if c1 != 429 || c1 != c2 || b1 != b2 || r1 == "" || r2 == "" {
		t.Fatalf("back-off differs: %d %s %s / %d %s %s", c1, b1, r1, c2, b2, r2)
	}
	// Blocked means blocked, even with the right password or another spelling.
	if c, _, _ := login(" ANA@example.com", "hunter2hunter2"); c != 429 {
		t.Fatalf("expected 429, got %d", c)
	}
	// Other accounts aren't affected.
	e.call("POST", "/auth/login", "", map[string]any{"email": "bob@example.com", "password": "nope-nope"}, 401)

	// It wears off, and a successful sign-in clears the count.
	expire := func() {
		b := e.s.loginBackoff
		b.mu.Lock()
		b.fails[backoffKey("ana@example.com")].until = time.Time{}
		b.mu.Unlock()
	}
	expire()
	if c, _, _ := login("ana@example.com", "hunter2hunter2"); c != 200 {
		t.Fatalf("expected 200, got %d", c)
	}
	for i := 0; i < loginFreeAttempts-1; i++ {
		if c, _, _ := login("ana@example.com", "wrong-password"); c != 401 {
			t.Fatalf("attempt %d after reset: %d", i, c)
		}
	}

	// A failure right after a long block extends it, up to the maximum.
	b := NewLoginBackoff()
	var longest time.Duration
	for i := 0; i < loginFreeAttempts+12; i++ {
		b.Fail("x@example.com")
		if w := b.Wait("x@example.com"); w > longest {
			longest = w
		}
	}
	if longest > loginMaxDelay || longest < loginMaxDelay-time.Minute {
		t.Fatalf("longest delay %v", longest)
	}

	// Password checks on a signed-in session count too.
	reg := e.call("POST", "/auth/register", "", map[string]any{"email": "cara@example.com", "password": "hunter2hunter2"}, 201)
	tok := reg["accessToken"].(string)
	for i := 0; i < loginFreeAttempts; i++ {
		e.call("DELETE", "/me", tok, map[string]any{"password": "wrong-password"}, 403)
	}
	e.call("DELETE", "/me", tok, map[string]any{"password": "hunter2hunter2"}, 429)
}

func TestDeviceCaps(t *testing.T) {
	e := newEnv(t)
	tok, u := e.userToken("ana@example.com")

	for i := 0; i < maxPCsPerUser; i++ {
		e.call("POST", "/pcs", tok, map[string]any{"id": fmt.Sprintf("pc-%d", i), "name": "PC"}, 200)
	}
	res := e.call("POST", "/pcs", tok, map[string]any{"id": "pc-extra", "name": "PC"}, 409)
	if res["error"] != "too_many_pcs" || res["message"] == nil {
		t.Fatalf("unexpected %v", res)
	}
	// A PC that's already linked can still re-register, and removing one frees a place.
	e.call("POST", "/pcs", tok, map[string]any{"id": "pc-0", "name": "Renamed"}, 200)
	e.call("DELETE", "/pcs/pc-1", tok, nil, 200)
	e.call("POST", "/pcs", tok, map[string]any{"id": "pc-extra", "name": "PC"}, 200)

	for i := 0; i < maxHeadsetsPerUser; i++ {
		if err := e.store.AddHeadset(&Headset{ID: fmt.Sprintf("hs-%d", i), UserID: u.ID}, maxHeadsetsPerUser); err != nil {
			t.Fatal(err)
		}
	}
	if err := e.store.AddHeadset(&Headset{ID: "hs-extra", UserID: u.ID}, maxHeadsetsPerUser); err != errLimit {
		t.Fatalf("expected errLimit, got %v", err)
	}
	dc := e.call("POST", "/device/code", "", map[string]any{"deviceId": "hs-extra"}, 200)
	res = e.call("POST", "/device/approve", tok, map[string]any{"userCode": dc["userCode"]}, 409)
	if res["error"] != "too_many_headsets" || res["message"] == nil {
		t.Fatalf("unexpected %v", res)
	}
	// Signing a headset in again that's already linked still works.
	dc = e.call("POST", "/device/code", "", map[string]any{"deviceId": "hs-0"}, 200)
	e.call("POST", "/device/approve", tok, map[string]any{"userCode": dc["userCode"]}, 200)
	e.call("POST", "/device/token", "", map[string]any{"deviceCode": dc["deviceCode"]}, 200)
}

func TestMaxUsers(t *testing.T) {
	e := newEnvWith(t, func(c *Config) { c.MaxUsers = 1 }, false)
	if e.call("GET", "/config", "", nil, 200)["allowSignups"] != true {
		t.Fatal("signups should be open below the cap")
	}
	e.call("POST", "/auth/register", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2"}, 201)
	res := e.call("POST", "/auth/register", "", map[string]any{"email": "bob@example.com", "password": "hunter2hunter2"}, 403)
	if res["error"] != "signups_full" || res["message"] == nil {
		t.Fatalf("unexpected %v", res)
	}
	if e.call("GET", "/config", "", nil, 200)["allowSignups"] != false {
		t.Fatal("/config should say signups are closed at the cap")
	}
	if e.call("GET", "/info", "", nil, 200)["allowSignups"] != false {
		t.Fatal("/info should say signups are closed at the cap")
	}
	e.call("POST", "/auth/login", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2"}, 200)
}

func TestSignupsDisabled(t *testing.T) {
	e := newEnvWith(t, func(c *Config) { c.AllowSignups = false }, false)
	res := e.call("POST", "/auth/register", "", map[string]any{"email": "ana@example.com", "password": "hunter2hunter2"}, 403)
	if res["error"] != "signups_disabled" || res["message"] == nil {
		t.Fatalf("unexpected %v", res)
	}
	if e.call("GET", "/config", "", nil, 200)["allowSignups"] != false || e.call("GET", "/info", "", nil, 200)["allowSignups"] != false {
		t.Fatal("server info should say signups are closed")
	}
	if n, _ := e.store.UserCount(); n != 0 {
		t.Fatalf("%d accounts created", n)
	}

	// Existing accounts keep working, including linking a headset. A device code
	// on its own never creates an account.
	hash, _ := HashPassword("hunter2hunter2")
	_ = e.store.CreateUser(&User{ID: randomID(), Email: "bob@example.com", PasswordHash: hash}, 0)
	login := e.call("POST", "/auth/login", "", map[string]any{"email": "bob@example.com", "password": "hunter2hunter2"}, 200)
	dc := e.call("POST", "/device/code", "", map[string]any{"deviceId": "hs-1"}, 200)
	e.call("POST", "/device/token", "", map[string]any{"deviceCode": dc["deviceCode"]}, 400)
	if n, _ := e.store.UserCount(); n != 1 {
		t.Fatalf("%d accounts after a device code", n)
	}
	e.call("POST", "/device/approve", login["accessToken"].(string), map[string]any{"userCode": dc["userCode"]}, 200)
}

func TestEmailValidation(t *testing.T) {
	e := newEnv(t)
	for _, bad := range []string{"", "plain", "Name <a@example.com>", "<a@example.com>", "a@example.com (Name)",
		"a@example.com, b@example.com", `"a b"@example.com`, strings.Repeat("a", 250) + "@example.com"} {
		res := e.call("POST", "/auth/register", "", map[string]any{"email": bad, "password": "hunter2hunter2"}, 400)
		if res["error"] != "invalid_email" {
			t.Fatalf("%q: unexpected %v", bad, res)
		}
	}
	reg := e.call("POST", "/auth/register", "", map[string]any{"email": "  Ana.B+tag@Example.COM ", "password": "hunter2hunter2"}, 201)
	user := reg["user"].(map[string]any)
	if user["email"] != "ana.b+tag@example.com" || user["name"] != "ana.b+tag" {
		t.Fatalf("unexpected user %v", user)
	}
	e.call("POST", "/auth/login", "", map[string]any{"email": "ANA.b+tag@example.com", "password": "hunter2hunter2"}, 200)

	// Accounts already stored in the old form can still sign in.
	hash, _ := HashPassword("hunter2hunter2")
	_ = e.store.CreateUser(&User{ID: randomID(), Email: "Old <old@example.com>", PasswordHash: hash}, 0)
	e.call("POST", "/auth/login", "", map[string]any{"email": "Old <old@example.com>", "password": "hunter2hunter2"}, 200)
}

func TestNameLengthsBounded(t *testing.T) {
	e := newEnv(t)
	long := strings.Repeat("é", 500)
	reg := e.call("POST", "/auth/register", "", map[string]any{"email": strings.Repeat("a", 100) + "@example.com", "password": "hunter2hunter2", "deviceId": long, "deviceName": long}, 201)
	user := reg["user"].(map[string]any)
	if n := utf8.RuneCountInString(user["name"].(string)); n > 64 {
		t.Fatalf("default name has %d characters", n)
	}
	tok := reg["accessToken"].(string)
	if n := utf8.RuneCountInString(e.call("PATCH", "/me", tok, map[string]any{"name": long}, 200)["name"].(string)); n > 64 {
		t.Fatalf("name has %d characters", n)
	}

	e.call("POST", "/pcs", tok, map[string]any{"id": strings.Repeat("x", 65), "name": "PC"}, 400)
	e.call("POST", "/pcs", tok, map[string]any{"id": "pc-1", "name": long, "gpu": long, "os": long, "version": long}, 200)
	pc, err := e.store.PCByID("pc-1")
	if err != nil {
		t.Fatal(err)
	}
	if utf8.RuneCountInString(pc.Name) > 64 || utf8.RuneCountInString(pc.GPU) > 128 || utf8.RuneCountInString(pc.OS) > 64 || utf8.RuneCountInString(pc.Version) > 32 {
		t.Fatalf("PC fields not bounded: %+v", pc)
	}

	dc := e.call("POST", "/device/code", "", map[string]any{"deviceId": long, "deviceName": long, "model": long}, 200)
	found := e.call("GET", "/device/lookup?code="+dc["userCode"].(string), tok, nil, 200)
	if utf8.RuneCountInString(found["deviceName"].(string)) > 64 || utf8.RuneCountInString(found["model"].(string)) > 64 {
		t.Fatalf("headset fields not bounded: %v", found)
	}
}

func TestLocalAddrsValidatedAndKeptOnlyWhileOnline(t *testing.T) {
	e := newEnv(t)
	tok, _ := e.userToken("ana@example.com")
	stored := func() string {
		var raw string
		_ = e.store.db.View(func(tx *bolt.Tx) error {
			raw = string(tx.Bucket(bucketPCs).Get([]byte("pc-1")))
			return nil
		})
		return raw
	}

	addrs := []string{"192.168.1.5:38711", "192.168.1.5:38711", "[2001:db8::5]:38711", "evil.example:1", "junk", "<script>",
		"0.0.0.0:5", "127.0.0.1:5", "224.0.0.1:5", "192.168.1.6:0", "192.168.1.6:70000", ""}
	e.call("POST", "/pcs", tok, map[string]any{"id": "pc-1", "name": "Desk", "localAddrs": addrs}, 200)
	got := e.call("POST", "/connect", tok, map[string]any{"pcId": "pc-1"}, 200)["pcLocalAddrs"].([]any)
	if len(got) != 2 || got[0] != "192.168.1.5:38711" || got[1] != "[2001:db8::5]:38711" {
		t.Fatalf("unexpected addresses %v", got)
	}
	if strings.Contains(stored(), "192.168.1.5") || strings.Contains(stored(), "localAddrs") {
		t.Fatalf("addresses written to disk: %s", stored())
	}

	// The count is capped, and heartbeats go through the same checks.
	var many []string
	for i := 1; i <= 40; i++ {
		many = append(many, fmt.Sprintf("10.0.0.%d:38711", i))
	}
	e.call("POST", "/pcs/pc-1/heartbeat", tok, map[string]any{"localAddrs": append(many, "junk")}, 200)
	got = e.call("POST", "/connect", tok, map[string]any{"pcId": "pc-1"}, 200)["pcLocalAddrs"].([]any)
	if len(got) != maxCandidates {
		t.Fatalf("got %d addresses", len(got))
	}
	if strings.Contains(stored(), "10.0.0.1") {
		t.Fatalf("addresses written to disk: %s", stored())
	}

	// Only junk: an empty list, not null, which clients would choke on.
	e.call("POST", "/pcs/pc-1/heartbeat", tok, map[string]any{"localAddrs": []string{"junk"}}, 200)
	if v, ok := e.call("POST", "/connect", tok, map[string]any{"pcId": "pc-1"}, 200)["pcLocalAddrs"].([]any); !ok || len(v) != 0 {
		t.Fatalf("expected an empty list, got %v", v)
	}

	// A heartbeat without addresses leaves them alone.
	e.call("POST", "/pcs/pc-1/heartbeat", tok, map[string]any{"localAddrs": []string{"192.168.1.5:38711"}}, 200)
	e.call("POST", "/pcs/pc-1/heartbeat", tok, map[string]any{"busy": true}, 200)
	if a := e.hub.LocalAddrs("pc-1"); len(a) != 1 {
		t.Fatalf("addresses lost: %v", a)
	}

	// Still online: kept. Offline: gone.
	e.hub.Sweep(time.Now())
	if a := e.hub.LocalAddrs("pc-1"); len(a) != 1 {
		t.Fatalf("addresses dropped while online: %v", a)
	}
	e.hub.Sweep(time.Now().Add(presenceTTL + time.Second))
	if a := e.hub.LocalAddrs("pc-1"); len(a) != 0 {
		t.Fatalf("addresses kept while offline: %v", a)
	}

	// Removing the PC forgets them too.
	e.call("POST", "/pcs/pc-1/heartbeat", tok, map[string]any{"localAddrs": []string{"192.168.1.5:38711"}}, 200)
	e.call("DELETE", "/pcs/pc-1", tok, nil, 200)
	if a := e.hub.LocalAddrs("pc-1"); len(a) != 0 {
		t.Fatalf("addresses kept after unlinking: %v", a)
	}
}

func TestOldStoredAddrsAreScrubbed(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.db")
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	old := `{"id":"pc-1","userId":"u1","name":"Desk","localAddrs":["203.0.113.5:38711"],"createdAt":"2026-01-01T00:00:00Z","lastSeenAt":"2026-01-01T00:00:00Z"}`
	_ = store.db.Update(func(tx *bolt.Tx) error { return tx.Bucket(bucketPCs).Put([]byte("pc-1"), []byte(old)) })
	store.Close()

	store, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	_ = store.db.View(func(tx *bolt.Tx) error {
		if raw := tx.Bucket(bucketPCs).Get([]byte("pc-1")); bytes.Contains(raw, []byte("203.0.113.5")) {
			t.Fatalf("address still stored: %s", raw)
		}
		return nil
	})
	if pc, err := store.PCByID("pc-1"); err != nil || pc.Name != "Desk" || pc.UserID != "u1" {
		t.Fatalf("record damaged: %+v %v", pc, err)
	}
}

func udpExchange(t *testing.T, sock *net.UDPConn, to *net.UDPAddr, msg string) []byte {
	t.Helper()
	if _, err := sock.WriteToUDP([]byte(msg), to); err != nil {
		t.Fatal(err)
	}
	_ = sock.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	buf := make([]byte, 2048)
	n, _, err := sock.ReadFromUDP(buf)
	if err != nil {
		return nil
	}
	return buf[:n]
}

func TestRendezvousDoesNotAmplify(t *testing.T) {
	e := newEnv(t)
	sock, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer sock.Close()
	to := e.rv.LocalAddr()

	// Too small to answer: silence, for pings and for bad registrations alike.
	for _, msg := range []string{`RCRV{"op":"ping"}`, `RCRV{"op":"register"}`, `RCRV{"op":"register","session":"s","token":"t"}`} {
		if got := udpExchange(t, sock, to, msg); got != nil {
			t.Fatalf("%s answered with %s", msg, got)
		}
	}

	// A padded ping is answered, with the address, and no more bytes than it cost.
	pad := `RCRV{"op":"ping","pad":"` + strings.Repeat("x", 100-len(`RCRV{"op":"ping","pad":""}`)) + `"}`
	if len(pad) != 100 {
		t.Fatalf("padded ping is %d bytes", len(pad))
	}
	got := udpExchange(t, sock, to, pad)
	if got == nil || len(got) > len(pad) || !bytes.Contains(got, []byte(`"pong"`)) || !bytes.Contains(got, []byte(sock.LocalAddr().String())) {
		t.Fatalf("unexpected pong %s", got)
	}

	// The documented 100 bytes are enough for the longest possible address.
	long, _ := json.Marshal(rvMessage{Op: "pong", Self: "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:65535"})
	if len(rvMagic)+len(long) > 100 {
		t.Fatalf("longest pong is %d bytes", len(rvMagic)+len(long))
	}

	// A real client's failed registration is large enough to get its error back.
	bad, _ := json.Marshal(rvMessage{Op: "register", Session: randomID(), Role: rolePC, Token: strings.Repeat("0", 64)})
	req := "RCRV" + string(bad)
	got = udpExchange(t, sock, to, req)
	if got == nil || len(got) > len(req) || !bytes.Contains(got, []byte("invalid_session")) {
		t.Fatalf("unexpected error reply %s", got)
	}
}

func TestRendezvousLimitsEachSource(t *testing.T) {
	hub := NewHub()
	rv, err := NewRendezvous("127.0.0.1:0", hub)
	if err != nil {
		t.Fatal(err)
	}
	rv.limiter = NewRateLimiter(3, time.Hour)
	go rv.Serve()
	defer rv.Close()
	sock, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer sock.Close()

	ping := `RCRV{"op":"ping","pad":"` + strings.Repeat("x", 100) + `"}`
	for i := 0; i < 3; i++ {
		if udpExchange(t, sock, rv.LocalAddr(), ping) == nil {
			t.Fatalf("ping %d not answered", i)
		}
	}
	if got := udpExchange(t, sock, rv.LocalAddr(), ping); got != nil {
		t.Fatalf("answered over the limit: %s", got)
	}

	// The default leaves room for clients retrying every 300 ms for half a minute.
	if rvPerSourcePerMinute < 100 {
		t.Fatalf("limit of %d/min is too tight for a registering client", rvPerSourcePerMinute)
	}
}

func TestSecurityHeaders(t *testing.T) {
	e := newEnv(t)
	get := func(e *testEnv, path string) http.Header {
		res, err := http.Get(e.srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		res.Body.Close()
		return res.Header
	}
	if h := get(e, "/").Get("Strict-Transport-Security"); h != "" {
		t.Fatalf("HSTS on a plain http server: %q", h)
	}
	csp := get(e, "/").Get("Content-Security-Policy")
	for _, want := range []string{"default-src 'none'", "script-src 'self'", "style-src 'self'", "connect-src 'self'", "frame-ancestors 'none'"} {
		if !strings.Contains(csp, want) {
			t.Fatalf("CSP lacks %q: %s", want, csp)
		}
	}
	if strings.Contains(csp, "unsafe") {
		t.Fatalf("unsafe CSP: %s", csp)
	}

	secure := newEnvWith(t, func(c *Config) { c.PublicURL = "https://rincast.example.com" }, false)
	for _, p := range []string{"/", "/app.js", "/api/v1/health"} {
		if h := get(secure, p).Get("Strict-Transport-Security"); !strings.Contains(h, "max-age=") {
			t.Fatalf("no HSTS on %s: %q", p, h)
		}
	}

	// The site has to live within that policy: nothing inline.
	for _, f := range []string{"web/index.html", "web/app.js", "web/logo.svg"} {
		data, err := webFS.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		if m := regexp.MustCompile(`(?i)\sstyle\s*=|<style|<script(?:[^>]*)>[^<]|\son[a-z]+\s*=\s*["']|javascript:`).Find(data); m != nil {
			t.Fatalf("%s has inline content the CSP blocks: %q", f, m)
		}
	}
	css, _ := webFS.ReadFile("web/app.css")
	if bytes.Contains(css, []byte("url(")) {
		t.Fatal("app.css loads a resource (image or font) the CSP may block")
	}
}

func TestForwardedForWarning(t *testing.T) {
	var logged bytes.Buffer
	defer log.SetOutput(log.Writer())
	log.SetOutput(&logged)
	const warning = "-trust-proxy is off"

	send := func(e *testEnv, forwarded string) {
		req, _ := http.NewRequest("GET", e.srv.URL+"/api/v1/health", nil)
		if forwarded != "" {
			req.Header.Set("X-Forwarded-For", forwarded)
		}
		res, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		res.Body.Close()
	}

	e := newEnv(t)
	send(e, "")
	if strings.Contains(logged.String(), warning) {
		t.Fatal("warned without the header")
	}
	send(e, "1.2.3.4")
	send(e, "<forged>")
	if n := strings.Count(logged.String(), warning); n != 1 {
		t.Fatalf("warned %d times: %s", n, logged.String())
	}
	if strings.Contains(logged.String(), "forged") {
		t.Fatalf("logged a client-supplied value: %s", logged.String())
	}

	logged.Reset()
	send(newEnvWith(t, func(c *Config) { c.TrustProxy = true }, false), "1.2.3.4")
	if strings.Contains(logged.String(), warning) {
		t.Fatal("warned with -trust-proxy on")
	}
}

func TestWebFallback(t *testing.T) {
	e := newEnv(t)
	for _, p := range []string{"/", "/link", "/account", "/app.js", "/logo.svg"} {
		res, err := http.Get(e.srv.URL + p)
		if err != nil || res.StatusCode != 200 {
			t.Fatalf("GET %s: %v %v", p, err, res.StatusCode)
		}
		res.Body.Close()
	}
}
