package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"net"
	"strconv"
	"sync"
	"time"
)

// Hub keeps the volatile state: device codes, PC presence, pending events and
// connect sessions. Nothing here needs to survive a restart; clients simply
// re-register.
type Hub struct {
	mu          sync.Mutex
	deviceCodes map[string]*DeviceCode // by device code
	userCodes   map[string]string      // user code -> device code
	presence    map[string]*Presence   // by PC id
	sessions    map[string]*ConnectSession
	tokenKey    []byte
}

type DeviceCode struct {
	DeviceCode     string
	UserCode       string
	DeviceID       string
	DeviceName     string
	Model          string
	ExpiresAt      time.Time
	ApprovedUserID string
	Consumed       bool
	LastPoll       time.Time
}

type PCEvent struct {
	Type string         `json:"type"`
	Data map[string]any `json:"data"`
}

type Presence struct {
	LastSeen time.Time
	Busy     bool
	// Where the PC says it can be reached. Held in memory only, and dropped by
	// Sweep once the PC goes offline.
	LocalAddrs []string
	events     []PCEvent
	notify     chan struct{}
}

type ConnectSession struct {
	ID          string
	UserID      string
	PCID        string
	HeadsetID   string
	HeadsetName string
	Secret      string
	CreatedAt   time.Time
	LastActive  time.Time
	// Per role (0 = pc, 1 = headset): addresses the rendezvous observed the side
	// sending from (one per IP family) and the addresses it reported itself (LAN,
	// IPv6, router port mappings). The peer gets both and punches toward all.
	Observed   [2][]string
	Candidates [2][]string
}

const (
	rolePC      = 0
	roleHeadset = 1
)

// presenceTTL is how long a PC counts as online after it was last heard from.
const presenceTTL = 70 * time.Second

func NewHub() *Hub {
	key := make([]byte, 32)
	if _, err := rand.Read(key); err != nil {
		panic(err)
	}
	return &Hub{
		deviceCodes: map[string]*DeviceCode{},
		userCodes:   map[string]string{},
		presence:    map[string]*Presence{},
		sessions:    map[string]*ConnectSession{},
		tokenKey:    key,
	}
}

// --- device codes ---

func (h *Hub) NewDeviceCode(deviceID, name, model string, ttl time.Duration) *DeviceCode {
	h.mu.Lock()
	defer h.mu.Unlock()
	var userCode string
	for {
		userCode = randomUserCode()
		if _, taken := h.userCodes[userCode]; !taken {
			break
		}
	}
	dc := &DeviceCode{
		DeviceCode: randomToken(32),
		UserCode:   userCode,
		DeviceID:   deviceID,
		DeviceName: name,
		Model:      model,
		ExpiresAt:  time.Now().Add(ttl),
	}
	h.deviceCodes[dc.DeviceCode] = dc
	h.userCodes[userCode] = dc.DeviceCode
	return dc
}

func (h *Hub) DeviceCodeByUserCode(code string) *DeviceCode {
	h.mu.Lock()
	defer h.mu.Unlock()
	dc := h.deviceCodes[h.userCodes[normalizeUserCode(code)]]
	if dc == nil || time.Now().After(dc.ExpiresAt) || dc.Consumed {
		return nil
	}
	return dc
}

func (h *Hub) ApproveDeviceCode(code, userID string) *DeviceCode {
	h.mu.Lock()
	defer h.mu.Unlock()
	dc := h.deviceCodes[h.userCodes[normalizeUserCode(code)]]
	if dc == nil || time.Now().After(dc.ExpiresAt) || dc.Consumed {
		return nil
	}
	dc.ApprovedUserID = userID
	return dc
}

// PollDeviceCode returns (code, tooFast). Consumes it when approved.
func (h *Hub) PollDeviceCode(deviceCode string, minInterval time.Duration) (*DeviceCode, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	dc := h.deviceCodes[deviceCode]
	if dc == nil || time.Now().After(dc.ExpiresAt) || dc.Consumed {
		return nil, false
	}
	now := time.Now()
	tooFast := now.Sub(dc.LastPoll) < minInterval
	dc.LastPoll = now
	if dc.ApprovedUserID != "" {
		dc.Consumed = true
		delete(h.userCodes, dc.UserCode)
	}
	cp := *dc
	return &cp, tooFast
}

// --- presence & events ---

func (h *Hub) presenceLocked(pcID string) *Presence {
	p := h.presence[pcID]
	if p == nil {
		p = &Presence{notify: make(chan struct{}, 1)}
		h.presence[pcID] = p
	}
	return p
}

func (h *Hub) Touch(pcID string, busy *bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	p := h.presenceLocked(pcID)
	p.LastSeen = time.Now()
	if busy != nil {
		p.Busy = *busy
	}
}

func (h *Hub) Online(pcID string) (online bool, busy bool, lastSeen time.Time) {
	h.mu.Lock()
	defer h.mu.Unlock()
	p := h.presence[pcID]
	if p == nil {
		return false, false, time.Time{}
	}
	return time.Since(p.LastSeen) < presenceTTL, p.Busy, p.LastSeen
}

func (h *Hub) SetLocalAddrs(pcID string, addrs []string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.presenceLocked(pcID).LocalAddrs = addrs
}

// LocalAddrs returns the addresses a PC advertised; never nil.
func (h *Hub) LocalAddrs(pcID string) []string {
	h.mu.Lock()
	defer h.mu.Unlock()
	if p := h.presence[pcID]; p != nil && p.LocalAddrs != nil {
		return append([]string{}, p.LocalAddrs...)
	}
	return []string{}
}

func (h *Hub) PushEvent(pcID string, ev PCEvent) {
	h.mu.Lock()
	p := h.presenceLocked(pcID)
	p.events = append(p.events, ev)
	if len(p.events) > 32 {
		p.events = p.events[len(p.events)-32:]
	}
	ch := p.notify
	h.mu.Unlock()
	select {
	case ch <- struct{}{}:
	default:
	}
}

// WaitEvents long-polls for PC events.
func (h *Hub) WaitEvents(pcID string, wait time.Duration, done <-chan struct{}) []PCEvent {
	deadline := time.NewTimer(wait)
	defer deadline.Stop()
	for {
		h.mu.Lock()
		p := h.presenceLocked(pcID)
		p.LastSeen = time.Now()
		if len(p.events) > 0 {
			evs := p.events
			p.events = nil
			h.mu.Unlock()
			return evs
		}
		ch := p.notify
		h.mu.Unlock()
		select {
		case <-ch:
		case <-deadline.C:
			return []PCEvent{}
		case <-done:
			return []PCEvent{}
		}
	}
}

// --- connect sessions ---

func (h *Hub) NewSession(userID, pcID, headsetID, headsetName string) *ConnectSession {
	secret := make([]byte, 32)
	if _, err := rand.Read(secret); err != nil {
		panic(err)
	}
	s := &ConnectSession{
		ID:          randomID(),
		UserID:      userID,
		PCID:        pcID,
		HeadsetID:   headsetID,
		HeadsetName: headsetName,
		Secret:      hex.EncodeToString(secret),
		CreatedAt:   time.Now(),
		LastActive:  time.Now(),
	}
	h.mu.Lock()
	h.sessions[s.ID] = s
	h.mu.Unlock()
	return s
}

// SessionToken authorizes one side of a session at the rendezvous. (Sent to
// clients as "relayToken" for compatibility; nothing is relayed any more.)
func (h *Hub) SessionToken(sessionID string, role int) string {
	mac := hmac.New(sha256.New, h.tokenKey)
	mac.Write([]byte(sessionID))
	mac.Write([]byte{byte(role)})
	return hex.EncodeToString(mac.Sum(nil))
}

const maxCandidates = 16

// RegisterEndpoint records where one side was seen from and the candidates it
// reported. Returns everything known about the peer so far (observed endpoints
// first, then its own candidates), deduplicated.
func (h *Hub) RegisterEndpoint(sessionID string, role int, token string, addr *net.UDPAddr, candidates []string) (self string, peer []string, ok bool) {
	if !hmac.Equal([]byte(token), []byte(h.SessionToken(sessionID, role))) {
		return "", nil, false
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	s := h.sessions[sessionID]
	if s == nil {
		return "", nil, false
	}
	self = addr.String()
	s.Observed[role] = addOnce(s.Observed[role], self, 4)
	for _, c := range candidates {
		if valid := validCandidate(c); valid != "" {
			s.Candidates[role] = addOnce(s.Candidates[role], valid, maxCandidates)
		}
	}
	s.LastActive = time.Now()
	return self, h.peerAddrs(s, 1-role), true
}

// PeerAddrs returns what is known about one side of a session.
func (h *Hub) PeerAddrs(sessionID string, role int) []string {
	h.mu.Lock()
	defer h.mu.Unlock()
	if s := h.sessions[sessionID]; s != nil {
		return h.peerAddrs(s, role)
	}
	return nil
}

func (h *Hub) peerAddrs(s *ConnectSession, role int) []string {
	var out []string
	for _, a := range s.Observed[role] {
		out = addOnce(out, a, 2*maxCandidates)
	}
	for _, a := range s.Candidates[role] {
		out = addOnce(out, a, 2*maxCandidates)
	}
	return out
}

func addOnce(list []string, v string, limit int) []string {
	for _, x := range list {
		if x == v {
			return list
		}
	}
	if len(list) >= limit {
		return list
	}
	return append(list, v)
}

// validCandidate normalizes "ip:port" / "[ipv6]:port" and rejects anything that
// is not a concrete unicast address (no hostnames, wildcards or multicast).
func validCandidate(c string) string {
	host, port, err := net.SplitHostPort(c)
	if err != nil {
		return ""
	}
	ip := net.ParseIP(host)
	p, err := strconv.Atoi(port)
	if ip == nil || err != nil || p <= 0 || p > 65535 || ip.IsUnspecified() || ip.IsMulticast() || ip.IsLoopback() {
		return ""
	}
	return net.JoinHostPort(ip.String(), strconv.Itoa(p))
}

// sanitizeAddrs keeps the well-formed, distinct endpoints from what a PC reported.
func sanitizeAddrs(addrs []string) []string {
	out := []string{}
	for _, a := range addrs {
		if valid := validCandidate(a); valid != "" {
			out = addOnce(out, valid, maxCandidates)
		}
	}
	return out
}

// DropSessions ends the user's connect sessions, so their rendezvous tokens stop working.
func (h *Hub) DropSessions(userID string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for id, s := range h.sessions {
		if s.UserID == userID {
			delete(h.sessions, id)
		}
	}
}

func (h *Hub) Session(id string) *ConnectSession {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.sessions[id]
}

// Sweep expires stale state. Call periodically.
func (h *Hub) Sweep(now time.Time) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for k, dc := range h.deviceCodes {
		if now.After(dc.ExpiresAt.Add(time.Minute)) {
			delete(h.userCodes, dc.UserCode)
			delete(h.deviceCodes, k)
		}
	}
	for id, s := range h.sessions {
		idle := now.Sub(s.LastActive)
		if idle > 2*time.Minute {
			delete(h.sessions, id)
		}
	}
	for id, p := range h.presence {
		if now.Sub(p.LastSeen) >= presenceTTL {
			p.LocalAddrs = nil
		}
		if now.Sub(p.LastSeen) > 24*time.Hour {
			delete(h.presence, id)
		}
	}
}

// ObservedEndpoints returns where the rendezvous has seen one side send from.
func (h *Hub) ObservedEndpoints(sessionID string, role int) []string {
	h.mu.Lock()
	defer h.mu.Unlock()
	if s := h.sessions[sessionID]; s != nil {
		return append([]string(nil), s.Observed[role]...)
	}
	return nil
}
