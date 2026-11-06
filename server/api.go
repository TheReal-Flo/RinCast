package main

import (
	"context"
	"encoding/json"
	"errors"
	"log"
	"net"
	"net/http"
	"net/mail"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

const (
	accessTokenTTL   = time.Hour
	refreshTokenTTL  = 180 * 24 * time.Hour
	deviceCodeTTL    = 10 * time.Minute
	deviceCodePollIv = 3 * time.Second
	maxBodyBytes     = 64 * 1024

	maxPCsPerUser      = 25
	maxHeadsetsPerUser = 25
)

type Config struct {
	PublicURL      string // e.g. https://rincast.example.com
	RendezvousHost string // host the clients use for the UDP rendezvous
	RendezvousPort int
	AllowSignups   bool
	TrustProxy     bool
	MaxUsers       int    // 0 means no cap
	PrivacyURL     string // shown on the account site when set
	ImprintURL     string
	DownloadURL    string // where to get the apps
	Contact        string // how to reach the operator, for password resets
}

type Server struct {
	cfg     Config
	store   *Store
	hub     *Hub
	signer  *TokenSigner
	limiter *RateLimiter // sign-in, registration and password checks

	linkLimiter    *RateLimiter // entering a headset code
	pollLimiter    *RateLimiter // headsets polling for approval
	connectLimiter *RateLimiter
	pcLimiter      *RateLimiter
	loginBackoff   *LoginBackoff

	forwardedWarning sync.Once
}

type ctxKey int

const claimsKey ctxKey = 1

func NewServer(cfg Config, store *Store, hub *Hub, signingKey []byte) *Server {
	return &Server{cfg: cfg, store: store, hub: hub, signer: &TokenSigner{key: signingKey},
		limiter:        NewRateLimiter(20, time.Minute),
		linkLimiter:    NewRateLimiter(20, time.Minute),
		pollLimiter:    NewRateLimiter(120, time.Minute), // 20/min per headset at the normal 3 s interval
		connectLimiter: NewRateLimiter(30, time.Minute),
		pcLimiter:      NewRateLimiter(240, time.Minute), // a device makes about 5/min
		loginBackoff:   NewLoginBackoff()}
}

// SweepLimits forgets rate limit state that has gone idle. Call periodically.
func (s *Server) SweepLimits() {
	for _, l := range []*RateLimiter{s.limiter, s.linkLimiter, s.pollLimiter, s.connectLimiter, s.pcLimiter} {
		l.Sweep()
	}
	s.loginBackoff.Sweep()
}

func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"ok": true, "time": time.Now().UTC()})
	})
	mux.HandleFunc("GET /api/v1/config", s.handleConfig)
	mux.HandleFunc("GET /api/v1/info", s.handleInfo)
	mux.HandleFunc("POST /api/v1/auth/register", s.limited(s.handleRegister))
	mux.HandleFunc("POST /api/v1/auth/login", s.limited(s.handleLogin))
	mux.HandleFunc("POST /api/v1/auth/refresh", s.limited(s.handleRefresh))
	mux.HandleFunc("POST /api/v1/auth/logout", s.handleLogout)
	mux.HandleFunc("GET /api/v1/me", s.authed(s.handleMe))
	mux.HandleFunc("PATCH /api/v1/me", s.limited(s.authed(s.handleUpdateMe)))
	mux.HandleFunc("DELETE /api/v1/me", s.limited(s.authed(s.handleDeleteMe)))
	mux.HandleFunc("GET /api/v1/me/export", s.limited(s.authed(s.handleExportMe)))

	mux.HandleFunc("POST /api/v1/device/code", s.limited(s.handleDeviceCode))
	mux.HandleFunc("POST /api/v1/device/token", s.limitedBy(s.pollLimiter, s.handleDeviceToken))
	mux.HandleFunc("GET /api/v1/device/lookup", s.limitedBy(s.linkLimiter, s.authed(s.handleDeviceLookup)))
	mux.HandleFunc("POST /api/v1/device/approve", s.limitedBy(s.linkLimiter, s.authed(s.handleDeviceApprove)))

	mux.HandleFunc("GET /api/v1/pcs", s.limitedBy(s.pcLimiter, s.authed(s.handleListPCs)))
	mux.HandleFunc("POST /api/v1/pcs", s.limitedBy(s.pcLimiter, s.authed(s.handleRegisterPC)))
	mux.HandleFunc("DELETE /api/v1/pcs/{id}", s.limitedBy(s.pcLimiter, s.authed(s.handleDeletePC)))
	mux.HandleFunc("POST /api/v1/pcs/{id}/heartbeat", s.limitedBy(s.pcLimiter, s.authed(s.handleHeartbeat)))
	mux.HandleFunc("GET /api/v1/pcs/{id}/events", s.limitedBy(s.pcLimiter, s.authed(s.handleEvents)))

	mux.HandleFunc("GET /api/v1/headsets", s.authed(s.handleListHeadsets))
	mux.HandleFunc("DELETE /api/v1/headsets/{id}", s.authed(s.handleDeleteHeadset))

	mux.HandleFunc("POST /api/v1/connect", s.limitedBy(s.connectLimiter, s.authed(s.handleConnect)))

	mux.Handle("/", webHandler())
	return s.securityHeaders(mux)
}

// --- middleware & helpers ---------------------------------------------------

// siteCSP suits server/web: same-origin scripts, styles, images and fetches, and
// nothing inline. Exports download through a blob: link, which CSP doesn't restrict.
const siteCSP = "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; font-src 'self'; connect-src 'self'; " +
	"base-uri 'none'; form-action 'self'; frame-ancestors 'none'"

func (s *Server) securityHeaders(next http.Handler) http.Handler {
	https := strings.HasPrefix(strings.ToLower(s.cfg.PublicURL), "https://")
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !s.cfg.TrustProxy && r.Header.Get("X-Forwarded-For") != "" {
			s.forwardedWarning.Do(func() {
				log.Printf("warning: requests carry X-Forwarded-For but -trust-proxy is off, so rate limits treat every client behind the proxy as one. Set -trust-proxy if a reverse proxy you control is in front of this server.")
			})
		}
		h := w.Header()
		if https {
			h.Set("Strict-Transport-Security", "max-age=31536000")
		}
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("X-Frame-Options", "DENY")
		h.Set("Referrer-Policy", "same-origin")
		if !strings.HasPrefix(r.URL.Path, "/api/") {
			h.Set("Content-Security-Policy", siteCSP)
		}
		next.ServeHTTP(w, r)
	})
}

type apiError struct {
	Error   string `json:"error"`
	Message string `json:"message,omitempty"`
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, status int, code, message string) {
	writeJSON(w, status, apiError{Error: code, Message: message})
}

func readJSON(w http.ResponseWriter, r *http.Request, v any) bool {
	r.Body = http.MaxBytesReader(w, r.Body, maxBodyBytes)
	dec := json.NewDecoder(r.Body)
	if err := dec.Decode(v); err != nil {
		writeErr(w, 400, "bad_request", "Malformed JSON body")
		return false
	}
	return true
}

func (s *Server) clientIP(r *http.Request) string {
	if s.cfg.TrustProxy {
		// The proxy in front of us appends the address it saw; earlier entries come
		// from the client and can be forged, so only the last one is trustworthy.
		if fwd := r.Header.Values("X-Forwarded-For"); len(fwd) > 0 {
			parts := strings.Split(fwd[len(fwd)-1], ",")
			if last := strings.TrimSpace(parts[len(parts)-1]); last != "" {
				return last
			}
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func (s *Server) limited(h http.HandlerFunc) http.HandlerFunc { return s.limitedBy(s.limiter, h) }

func (s *Server) limitedBy(l *RateLimiter, h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !l.Allow(limitKey(s.clientIP(r))) {
			w.Header().Set("Retry-After", "30")
			writeErr(w, 429, "rate_limited", "Too many requests, slow down")
			return
		}
		h(w, r)
	}
}

func (s *Server) authed(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		auth := r.Header.Get("Authorization")
		if !strings.HasPrefix(auth, "Bearer ") {
			writeErr(w, 401, "unauthorized", "Missing bearer token")
			return
		}
		claims, err := s.signer.Verify(strings.TrimPrefix(auth, "Bearer "), time.Now())
		if err != nil {
			writeErr(w, 401, "unauthorized", err.Error())
			return
		}
		// Access tokens are stateless, so check they haven't outlived the account
		// or a password change. Clients answer this 401 by refreshing.
		u, err := s.store.UserByID(claims.Subject)
		if errors.Is(err, errNotFound) || (err == nil && claims.IssuedAt < u.TokensValidAfter) {
			writeErr(w, 401, "unauthorized", "Session expired, please sign in again")
			return
		}
		if err != nil {
			writeErr(w, 500, "internal", "Could not check your session")
			return
		}
		h(w, r.WithContext(context.WithValue(r.Context(), claimsKey, claims)))
	}
}

func claimsFrom(r *http.Request) *Claims { return r.Context().Value(claimsKey).(*Claims) }

type tokenResponse struct {
	AccessToken  string         `json:"accessToken"`
	RefreshToken string         `json:"refreshToken"`
	ExpiresIn    int            `json:"expiresIn"`
	User         map[string]any `json:"user"`
}

func (s *Server) issueTokens(u *User, kind, deviceID, deviceName string) (*tokenResponse, error) {
	return s.issueSession(u, randomID(), kind, deviceID, deviceName)
}

func (s *Server) issueSession(u *User, sessionID, kind, deviceID, deviceName string) (*tokenResponse, error) {
	now := time.Now()
	refresh := randomToken(32)
	err := s.store.PutRefreshToken(sha256Hex(refresh), &RefreshToken{
		UserID: u.ID, SessionID: sessionID, DeviceKind: kind, DeviceID: deviceID, DeviceName: deviceName,
		CreatedAt: now, ExpiresAt: now.Add(refreshTokenTTL), LastUsedAt: now,
	})
	if err != nil {
		return nil, err
	}
	access := s.signer.Sign(Claims{Subject: u.ID, DeviceKind: kind, DeviceID: deviceID, SessionID: sessionID,
		IssuedAt: now.Unix(), ExpiresAt: now.Add(accessTokenTTL).Unix()})
	return &tokenResponse{AccessToken: access, RefreshToken: refresh, ExpiresIn: int(accessTokenTTL.Seconds()), User: u.Public()}, nil
}

func validDeviceKind(kind string) string {
	switch kind {
	case "pc", "headset", "web":
		return kind
	}
	return "web"
}

func clip(s string, n int) string {
	s = strings.TrimSpace(s)
	if utf8.RuneCountInString(s) <= n {
		return s
	}
	return string([]rune(s)[:n])
}

// --- handlers: auth ------------------------------------------------------------

func (s *Server) serverInfo() map[string]any {
	return map[string]any{
		"publicUrl":     s.cfg.PublicURL,
		"linkUrl":       strings.TrimRight(s.cfg.PublicURL, "/") + "/link",
		"rendezvous":    map[string]any{"host": s.cfg.RendezvousHost, "port": s.cfg.RendezvousPort},
		"allowSignups":  s.signupsOpen(),
		"minAppVersion": "1.0.0",
	}
}

func (s *Server) signupsOpen() bool {
	if !s.cfg.AllowSignups {
		return false
	}
	if s.cfg.MaxUsers > 0 {
		if n, err := s.store.UserCount(); err == nil && n >= s.cfg.MaxUsers {
			return false
		}
	}
	return true
}

func (s *Server) handleConfig(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, 200, s.serverInfo())
}

func (s *Server) handleInfo(w http.ResponseWriter, r *http.Request) {
	info := s.serverInfo()
	if s.cfg.PrivacyURL != "" {
		info["privacyUrl"] = s.cfg.PrivacyURL
	}
	if s.cfg.ImprintURL != "" {
		info["imprintUrl"] = s.cfg.ImprintURL
	}
	if s.cfg.DownloadURL != "" {
		info["downloadUrl"] = s.cfg.DownloadURL
	}
	if s.cfg.Contact != "" {
		info["contact"] = s.cfg.Contact
	}
	writeJSON(w, 200, info)
}

// validEmail requires a bare address: mail.ParseAddress alone also accepts
// "Name <a@b>", which would then be stored as the account's email. Expects a
// normalized email.
func validEmail(email string) bool {
	if len(email) > 254 {
		return false
	}
	addr, err := mail.ParseAddress(email)
	return err == nil && addr.Name == "" && addr.Address == email
}

func validPassword(password string) bool { return len(password) >= 8 && len(password) <= 256 }

const weakPasswordMessage = "Password must be between 8 and 256 characters"

// checkPassword verifies the signed-in user's password for a sensitive change
// and writes the error if it fails. A stolen access token shouldn't allow more
// guesses than the login form does, so failures count toward the same back-off.
func (s *Server) checkPassword(w http.ResponseWriter, u *User, password string) bool {
	if s.refuseLogin(w, u.Email) {
		return false
	}
	if !VerifyPassword(u.PasswordHash, password) {
		s.loginBackoff.Fail(u.Email)
		// 403, not 401: clients take a 401 to mean the session expired.
		writeErr(w, 403, "invalid_credentials", "That password is incorrect")
		return false
	}
	s.loginBackoff.Reset(u.Email)
	return true
}

// refuseLogin writes a 429 if this email is backing off.
func (s *Server) refuseLogin(w http.ResponseWriter, email string) bool {
	wait := s.loginBackoff.Wait(email)
	if wait <= 0 {
		return false
	}
	w.Header().Set("Retry-After", strconv.Itoa(int(wait.Seconds())+1))
	writeErr(w, 429, "rate_limited", "Too many failed sign-in attempts. Wait a few minutes and try again.")
	return true
}

type credentials struct {
	Email      string `json:"email"`
	Password   string `json:"password"`
	Name       string `json:"name"`
	DeviceKind string `json:"deviceKind"`
	DeviceID   string `json:"deviceId"`
	DeviceName string `json:"deviceName"`
}

func (s *Server) handleRegister(w http.ResponseWriter, r *http.Request) {
	if !s.cfg.AllowSignups {
		writeErr(w, 403, "signups_disabled", "Registration is disabled on this server")
		return
	}
	var c credentials
	if !readJSON(w, r, &c) {
		return
	}
	email := normalizeEmail(c.Email)
	if !validEmail(email) {
		writeErr(w, 400, "invalid_email", "Please enter a valid email address")
		return
	}
	if !validPassword(c.Password) {
		writeErr(w, 400, "weak_password", weakPasswordMessage)
		return
	}
	name := clip(c.Name, 64)
	if name == "" {
		name = clip(strings.Split(email, "@")[0], 64)
	}
	hash, err := HashPassword(c.Password)
	if err != nil {
		writeErr(w, 500, "internal", "Could not hash password")
		return
	}
	u := &User{ID: randomID(), Email: email, Name: name, PasswordHash: hash, CreatedAt: time.Now().UTC()}
	if err := s.store.CreateUser(u, s.cfg.MaxUsers); err != nil {
		if errors.Is(err, errExists) {
			writeErr(w, 409, "email_taken", "An account with this email already exists")
			return
		}
		if errors.Is(err, errLimit) {
			writeErr(w, 403, "signups_full", "This server isn't accepting new accounts right now")
			return
		}
		writeErr(w, 500, "internal", "Could not create account")
		return
	}
	resp, err := s.issueTokens(u, validDeviceKind(c.DeviceKind), clip(c.DeviceID, 64), clip(c.DeviceName, 64))
	if err != nil {
		writeErr(w, 500, "internal", "Could not issue tokens")
		return
	}
	log.Printf("account created: %s", u.ID)
	writeJSON(w, 201, resp)
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	var c credentials
	if !readJSON(w, r, &c) {
		return
	}
	if s.refuseLogin(w, c.Email) {
		return
	}
	u, err := s.store.UserByEmail(c.Email)
	if err != nil || !VerifyPassword(u.PasswordHash, c.Password) {
		s.loginBackoff.Fail(c.Email)
		if err != nil {
			// Burn comparable CPU so timing doesn't reveal which emails exist.
			VerifyPassword("argon2id$2$65536$2$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", c.Password)
		}
		writeErr(w, 401, "invalid_credentials", "Email or password is incorrect")
		return
	}
	s.loginBackoff.Reset(c.Email)
	resp, err := s.issueTokens(u, validDeviceKind(c.DeviceKind), clip(c.DeviceID, 64), clip(c.DeviceName, 64))
	if err != nil {
		writeErr(w, 500, "internal", "Could not issue tokens")
		return
	}
	writeJSON(w, 200, resp)
}

func (s *Server) handleRefresh(w http.ResponseWriter, r *http.Request) {
	var body struct {
		RefreshToken string `json:"refreshToken"`
	}
	if !readJSON(w, r, &body) {
		return
	}
	hash := sha256Hex(body.RefreshToken)
	t, err := s.store.RefreshTokenByHash(hash)
	if err != nil || time.Now().After(t.ExpiresAt) {
		writeErr(w, 401, "invalid_refresh_token", "Session expired, please sign in again")
		return
	}
	u, err := s.store.UserByID(t.UserID)
	if err != nil {
		writeErr(w, 401, "invalid_refresh_token", "Account no longer exists")
		return
	}
	// Rotate: the old token is single-use.
	_ = s.store.DeleteRefreshToken(hash)
	sessionID := t.SessionID
	if sessionID == "" {
		sessionID = randomID() // issued before sessions had ids
	}
	resp, err := s.issueSession(u, sessionID, t.DeviceKind, t.DeviceID, t.DeviceName)
	if err != nil {
		writeErr(w, 500, "internal", "Could not issue tokens")
		return
	}
	writeJSON(w, 200, resp)
}

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	var body struct {
		RefreshToken string `json:"refreshToken"`
	}
	if !readJSON(w, r, &body) {
		return
	}
	_ = s.store.DeleteRefreshToken(sha256Hex(body.RefreshToken))
	writeJSON(w, 200, map[string]any{"ok": true})
}

func (s *Server) handleMe(w http.ResponseWriter, r *http.Request) {
	u, err := s.store.UserByID(claimsFrom(r).Subject)
	if err != nil {
		writeErr(w, 404, "not_found", "Account not found")
		return
	}
	writeJSON(w, 200, u.Public())
}

func (s *Server) handleUpdateMe(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name            *string `json:"name"`
		CurrentPassword string  `json:"currentPassword"`
		NewPassword     *string `json:"newPassword"`
	}
	if !readJSON(w, r, &body) {
		return
	}
	u, err := s.store.UserByID(claimsFrom(r).Subject)
	if err != nil {
		writeErr(w, 404, "not_found", "Account not found")
		return
	}
	if body.Name != nil {
		if n := clip(*body.Name, 64); n != "" {
			u.Name = n
		}
	}
	if body.NewPassword != nil {
		if !s.checkPassword(w, u, body.CurrentPassword) {
			return
		}
		if !validPassword(*body.NewPassword) {
			writeErr(w, 400, "weak_password", weakPasswordMessage)
			return
		}
		if u.PasswordHash, err = HashPassword(*body.NewPassword); err != nil {
			writeErr(w, 500, "internal", "Could not hash password")
			return
		}
		// Everyone else is signed out. This session's access token goes too, but
		// its refresh token stays, so it renews without asking again.
		u.TokensValidAfter = time.Now().Unix()
		err = s.store.ChangePassword(u, claimsFrom(r).SessionID)
	} else {
		err = s.store.UpdateUser(u)
	}
	if err != nil {
		writeErr(w, 500, "internal", "Could not update account")
		return
	}
	writeJSON(w, 200, u.Public())
}

func (s *Server) handleDeleteMe(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Password string `json:"password"`
	}
	if !readJSON(w, r, &body) {
		return
	}
	u, err := s.store.UserByID(claimsFrom(r).Subject)
	if err != nil {
		writeErr(w, 404, "not_found", "Account not found")
		return
	}
	if !s.checkPassword(w, u, body.Password) {
		return
	}
	pcIDs, err := s.store.DeleteUser(u)
	if err != nil {
		writeErr(w, 500, "internal", "Could not delete the account")
		return
	}
	s.hub.DropSessions(u.ID)
	for _, id := range pcIDs {
		s.hub.SetLocalAddrs(id, nil)
		s.hub.PushEvent(id, PCEvent{Type: "unlinked", Data: map[string]any{}})
	}
	log.Printf("account deleted: %s", u.ID)
	writeJSON(w, 200, map[string]any{"ok": true})
}

// handleExportMe returns what the server holds about the user. Password and
// token hashes are left out, and so are the ids tying tokens to sessions.
func (s *Server) handleExportMe(w http.ResponseWriter, r *http.Request) {
	id := claimsFrom(r).Subject
	u, err := s.store.UserByID(id)
	if err != nil {
		writeErr(w, 404, "not_found", "Account not found")
		return
	}
	pcs, err1 := s.store.PCsForUser(id)
	headsets, err2 := s.store.HeadsetsForUser(id)
	tokens, err3 := s.store.RefreshTokensForUser(id)
	if err := errors.Join(err1, err2, err3); err != nil {
		writeErr(w, 500, "internal", "Could not export your data")
		return
	}
	type pcExport struct {
		*PC
		LocalAddrs []string `json:"localAddrs"` // current addresses, while online
	}
	type sessionExport struct {
		DeviceKind string    `json:"deviceKind"`
		DeviceID   string    `json:"deviceId"`
		DeviceName string    `json:"deviceName"`
		CreatedAt  time.Time `json:"createdAt"`
		ExpiresAt  time.Time `json:"expiresAt"`
		LastUsedAt time.Time `json:"lastUsedAt"`
	}
	outPCs := make([]pcExport, 0, len(pcs))
	for _, pc := range pcs {
		outPCs = append(outPCs, pcExport{PC: pc, LocalAddrs: s.hub.LocalAddrs(pc.ID)})
	}
	sessions := make([]sessionExport, 0, len(tokens))
	for _, t := range tokens {
		sessions = append(sessions, sessionExport{t.DeviceKind, t.DeviceID, t.DeviceName, t.CreatedAt, t.ExpiresAt, t.LastUsedAt})
	}
	if headsets == nil {
		headsets = []*Headset{}
	}
	writeJSON(w, 200, map[string]any{
		"exportedAt": time.Now().UTC(),
		"user":       u.Public(),
		"pcs":        outPCs,
		"headsets":   headsets,
		"sessions":   sessions,
	})
}

// --- handlers: device code flow (headset sign-in) ---------------------------

func (s *Server) handleDeviceCode(w http.ResponseWriter, r *http.Request) {
	var body struct {
		DeviceID   string `json:"deviceId"`
		DeviceName string `json:"deviceName"`
		Model      string `json:"model"`
	}
	if !readJSON(w, r, &body) {
		return
	}
	if body.DeviceID == "" {
		writeErr(w, 400, "bad_request", "deviceId is required")
		return
	}
	dc := s.hub.NewDeviceCode(clip(body.DeviceID, 64), clip(body.DeviceName, 64), clip(body.Model, 64), deviceCodeTTL)
	base := strings.TrimRight(s.cfg.PublicURL, "/")
	writeJSON(w, 200, map[string]any{
		"deviceCode":              dc.DeviceCode,
		"userCode":                dc.UserCode,
		"verificationUri":         base + "/link",
		"verificationUriComplete": base + "/link?code=" + dc.UserCode,
		"interval":                int(deviceCodePollIv.Seconds()),
		"expiresIn":               int(deviceCodeTTL.Seconds()),
	})
}

func (s *Server) handleDeviceToken(w http.ResponseWriter, r *http.Request) {
	var body struct {
		DeviceCode string `json:"deviceCode"`
	}
	if !readJSON(w, r, &body) {
		return
	}
	dc, tooFast := s.hub.PollDeviceCode(body.DeviceCode, deviceCodePollIv-500*time.Millisecond)
	if dc == nil {
		writeErr(w, 400, "expired_token", "The code expired, request a new one")
		return
	}
	if dc.ApprovedUserID == "" {
		if tooFast {
			writeErr(w, 400, "slow_down", "Polling too fast")
			return
		}
		writeErr(w, 400, "authorization_pending", "Waiting for approval")
		return
	}
	u, err := s.store.UserByID(dc.ApprovedUserID)
	if err != nil {
		writeErr(w, 400, "access_denied", "Account not found")
		return
	}
	now := time.Now().UTC()
	err = s.store.AddHeadset(&Headset{ID: dc.DeviceID, UserID: u.ID, Name: dc.DeviceName, Model: dc.Model,
		CreatedAt: now, LastSeenAt: now}, maxHeadsetsPerUser)
	if err != nil {
		writeErr(w, 400, "access_denied", "This account can't link more headsets")
		return
	}
	resp, err := s.issueTokens(u, "headset", dc.DeviceID, dc.DeviceName)
	if err != nil {
		writeErr(w, 500, "internal", "Could not issue tokens")
		return
	}
	writeJSON(w, 200, resp)
}

func (s *Server) handleDeviceLookup(w http.ResponseWriter, r *http.Request) {
	dc := s.hub.DeviceCodeByUserCode(r.URL.Query().Get("code"))
	if dc == nil {
		writeErr(w, 404, "invalid_code", "This code is invalid or expired")
		return
	}
	writeJSON(w, 200, map[string]any{"deviceName": dc.DeviceName, "model": dc.Model, "userCode": dc.UserCode})
}

func (s *Server) handleDeviceApprove(w http.ResponseWriter, r *http.Request) {
	var body struct {
		UserCode string `json:"userCode"`
	}
	if !readJSON(w, r, &body) {
		return
	}
	userID := claimsFrom(r).Subject
	if pending := s.hub.DeviceCodeByUserCode(body.UserCode); pending != nil {
		if hs, err := s.store.HeadsetsForUser(userID); err == nil && len(hs) >= maxHeadsetsPerUser && !hasHeadset(hs, pending.DeviceID) {
			writeErr(w, 409, "too_many_headsets", "You've linked the most headsets this server allows. Remove one first.")
			return
		}
	}
	dc := s.hub.ApproveDeviceCode(body.UserCode, userID)
	if dc == nil {
		writeErr(w, 404, "invalid_code", "This code is invalid or expired")
		return
	}
	writeJSON(w, 200, map[string]any{"ok": true, "deviceName": dc.DeviceName})
}

func hasHeadset(hs []*Headset, id string) bool {
	for _, h := range hs {
		if h.ID == id {
			return true
		}
	}
	return false
}

// --- handlers: PCs ---------------------------------------------------------------

type pcView struct {
	ID         string    `json:"id"`
	Name       string    `json:"name"`
	GPU        string    `json:"gpu"`
	OS         string    `json:"os"`
	Version    string    `json:"version"`
	Online     bool      `json:"online"`
	Busy       bool      `json:"busy"`
	LastSeenAt time.Time `json:"lastSeenAt"`
}

func (s *Server) handleListPCs(w http.ResponseWriter, r *http.Request) {
	pcs, err := s.store.PCsForUser(claimsFrom(r).Subject)
	if err != nil {
		writeErr(w, 500, "internal", "Could not list PCs")
		return
	}
	out := make([]pcView, 0, len(pcs))
	for _, pc := range pcs {
		online, busy, seen := s.hub.Online(pc.ID)
		if seen.IsZero() {
			seen = pc.LastSeenAt
		}
		out = append(out, pcView{ID: pc.ID, Name: pc.Name, GPU: pc.GPU, OS: pc.OS, Version: pc.Version,
			Online: online, Busy: busy, LastSeenAt: seen})
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Online != out[j].Online {
			return out[i].Online
		}
		return out[i].Name < out[j].Name
	})
	writeJSON(w, 200, map[string]any{"pcs": out})
}

func (s *Server) handleRegisterPC(w http.ResponseWriter, r *http.Request) {
	c := claimsFrom(r)
	var body struct {
		ID         string   `json:"id"`
		Name       string   `json:"name"`
		GPU        string   `json:"gpu"`
		OS         string   `json:"os"`
		Version    string   `json:"version"`
		LocalAddrs []string `json:"localAddrs"`
	}
	if !readJSON(w, r, &body) {
		return
	}
	if body.ID == "" || len(body.ID) > 64 {
		writeErr(w, 400, "bad_request", "A PC id is required")
		return
	}
	now := time.Now().UTC()
	pc := &PC{ID: body.ID, UserID: c.Subject, Name: clip(body.Name, 64), GPU: clip(body.GPU, 128),
		OS: clip(body.OS, 64), Version: clip(body.Version, 32), LastSeenAt: now}
	if existing, err := s.store.PCByID(body.ID); err == nil {
		pc.CreatedAt = existing.CreatedAt
	} else {
		pc.CreatedAt = now
	}
	if err := s.store.RegisterPC(pc, maxPCsPerUser); err != nil {
		if errors.Is(err, errExists) {
			writeErr(w, 409, "pc_claimed", "This PC is linked to another account")
			return
		}
		if errors.Is(err, errLimit) {
			writeErr(w, 409, "too_many_pcs", "You've linked the most PCs this server allows. Remove one first.")
			return
		}
		writeErr(w, 500, "internal", "Could not register PC")
		return
	}
	s.hub.Touch(pc.ID, nil)
	s.hub.SetLocalAddrs(pc.ID, sanitizeAddrs(body.LocalAddrs))
	writeJSON(w, 200, map[string]any{"ok": true, "id": pc.ID})
}

func (s *Server) ownedPC(w http.ResponseWriter, r *http.Request) *PC {
	pc, err := s.store.PCByID(r.PathValue("id"))
	if err != nil || pc.UserID != claimsFrom(r).Subject {
		writeErr(w, 404, "not_found", "PC not found")
		return nil
	}
	return pc
}

func (s *Server) handleDeletePC(w http.ResponseWriter, r *http.Request) {
	pc := s.ownedPC(w, r)
	if pc == nil {
		return
	}
	_ = s.store.DeletePC(pc.ID)
	_ = s.store.DeleteRefreshTokensForDevice(pc.UserID, pc.ID)
	s.hub.SetLocalAddrs(pc.ID, nil)
	s.hub.PushEvent(pc.ID, PCEvent{Type: "unlinked", Data: map[string]any{}})
	writeJSON(w, 200, map[string]any{"ok": true})
}

func (s *Server) handleHeartbeat(w http.ResponseWriter, r *http.Request) {
	pc := s.ownedPC(w, r)
	if pc == nil {
		return
	}
	var body struct {
		Busy       *bool    `json:"busy"`
		LocalAddrs []string `json:"localAddrs"`
	}
	if !readJSON(w, r, &body) {
		return
	}
	s.hub.Touch(pc.ID, body.Busy)
	if body.LocalAddrs != nil {
		s.hub.SetLocalAddrs(pc.ID, sanitizeAddrs(body.LocalAddrs))
		pc.LastSeenAt = time.Now().UTC()
		_ = s.store.PutPC(pc)
	}
	writeJSON(w, 200, map[string]any{"ok": true})
}

func (s *Server) handleEvents(w http.ResponseWriter, r *http.Request) {
	pc := s.ownedPC(w, r)
	if pc == nil {
		return
	}
	wait := 25 * time.Second
	if v, err := strconv.Atoi(r.URL.Query().Get("wait")); err == nil && v >= 0 && v <= 55 {
		wait = time.Duration(v) * time.Second
	}
	events := s.hub.WaitEvents(pc.ID, wait, r.Context().Done())
	writeJSON(w, 200, map[string]any{"events": events})
}

func (s *Server) handleListHeadsets(w http.ResponseWriter, r *http.Request) {
	hs, err := s.store.HeadsetsForUser(claimsFrom(r).Subject)
	if err != nil {
		writeErr(w, 500, "internal", "Could not list headsets")
		return
	}
	if hs == nil {
		hs = []*Headset{}
	}
	writeJSON(w, 200, map[string]any{"headsets": hs})
}

func (s *Server) handleDeleteHeadset(w http.ResponseWriter, r *http.Request) {
	c := claimsFrom(r)
	h, err := s.store.HeadsetByID(r.PathValue("id"))
	if err != nil || h.UserID != c.Subject {
		writeErr(w, 404, "not_found", "Headset not found")
		return
	}
	_ = s.store.DeleteHeadset(h.ID)
	_ = s.store.DeleteRefreshTokensForDevice(c.Subject, h.ID)
	writeJSON(w, 200, map[string]any{"ok": true})
}

// --- handlers: connect ------------------------------------------------------------

func (s *Server) handleConnect(w http.ResponseWriter, r *http.Request) {
	c := claimsFrom(r)
	var body struct {
		PCID string `json:"pcId"`
	}
	if !readJSON(w, r, &body) {
		return
	}
	pc, err := s.store.PCByID(body.PCID)
	if err != nil || pc.UserID != c.Subject {
		writeErr(w, 404, "not_found", "PC not found")
		return
	}
	if online, _, _ := s.hub.Online(pc.ID); !online {
		writeErr(w, 409, "pc_offline", "This PC is offline. Make sure the RinCast Streamer is running.")
		return
	}
	headsetName := "Headset"
	if h, err := s.store.HeadsetByID(c.DeviceID); err == nil && h.UserID == c.Subject {
		headsetName = h.Name
		h.LastSeenAt = time.Now().UTC()
		_ = s.store.PutHeadset(h)
	}
	sess := s.hub.NewSession(c.Subject, pc.ID, c.DeviceID, headsetName)
	rv := map[string]any{"host": s.cfg.RendezvousHost, "port": s.cfg.RendezvousPort}
	s.hub.PushEvent(pc.ID, PCEvent{Type: "connect", Data: map[string]any{
		"sessionId":     sess.ID,
		"connectSecret": sess.Secret,
		"relayToken":    s.hub.SessionToken(sess.ID, rolePC),
		"headsetId":     c.DeviceID,
		"headsetName":   headsetName,
		"rendezvous":    rv,
	}})
	writeJSON(w, 200, map[string]any{
		"sessionId":     sess.ID,
		"connectSecret": sess.Secret,
		"relayToken":    s.hub.SessionToken(sess.ID, roleHeadset),
		"rendezvous":    rv,
		"pcLocalAddrs":  s.hub.LocalAddrs(pc.ID),
		"pcName":        pc.Name,
	})
}
