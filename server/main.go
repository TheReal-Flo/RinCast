// RinCast cloud server.
//
// Provides accounts, headset sign-in via short codes, PC presence, and the
// rendezvous service that lets a headset reach its owner's PCs from
// any network. Local (same-LAN) streaming never touches this server.
package main

import (
	"context"
	"crypto/rand"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"syscall"
	"time"
)

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func loadOrCreateKey(path string) ([]byte, error) {
	if data, err := os.ReadFile(path); err == nil && len(data) >= 32 {
		return data, nil
	}
	key := make([]byte, 64)
	if _, err := rand.Read(key); err != nil {
		return nil, err
	}
	if err := os.WriteFile(path, key, 0o600); err != nil {
		return nil, err
	}
	return key, nil
}

// resetPassword gives an account a new random password and signs out all its
// devices. It opens the database directly, so the server must not be running.
func resetPassword(dataDir, email string, out io.Writer) error {
	path := filepath.Join(dataDir, "rincast.db")
	if _, err := os.Stat(path); err != nil {
		return fmt.Errorf("no database at %s; pass -data if it is elsewhere", path)
	}
	store, err := OpenStore(path)
	if err != nil {
		return fmt.Errorf("could not open %s (stop the server first, it keeps the database locked): %w", path, err)
	}
	defer store.Close()
	u, err := store.UserByEmail(email)
	if err != nil {
		return fmt.Errorf("no account with the email %q", email)
	}
	password := randomToken(18)
	if u.PasswordHash, err = HashPassword(password); err != nil {
		return err
	}
	u.TokensValidAfter = time.Now().Unix()
	if err := store.ChangePassword(u, ""); err != nil {
		return err
	}
	fmt.Fprintf(out, "New password for %s: %s\nAll of this account's devices are signed out. Give it to the user and ask them to change it in their account settings; it isn't shown again.\n", u.Email, password)
	return nil
}

func envInt(key string, fallback int) int {
	if v, err := strconv.Atoi(os.Getenv(key)); err == nil {
		return v
	}
	return fallback
}

func main() {
	httpAddr := flag.String("http", envOr("RINCAST_HTTP", ":8080"), "HTTP listen address")
	udpAddr := flag.String("udp", envOr("RINCAST_UDP", ":38720"), "UDP rendezvous listen address")
	dataDir := flag.String("data", envOr("RINCAST_DATA", "./data"), "data directory")
	publicURL := flag.String("public-url", envOr("RINCAST_PUBLIC_URL", "http://localhost:8080"), "public base URL")
	rvHost := flag.String("rendezvous-host", envOr("RINCAST_RENDEZVOUS_HOST", "localhost"), "host clients use for UDP rendezvous")
	allowSignups := flag.Bool("allow-signups", envOr("RINCAST_ALLOW_SIGNUPS", "true") == "true", "allow account registration")
	trustProxy := flag.Bool("trust-proxy", envOr("RINCAST_TRUST_PROXY", "false") == "true", "trust X-Forwarded-For")
	maxUsers := flag.Int("max-users", envInt("RINCAST_MAX_USERS", 0), "maximum number of accounts (0 = no limit)")
	privacyURL := flag.String("privacy-url", envOr("RINCAST_PRIVACY_URL", ""), "link to the privacy policy, shown on the account site")
	imprintURL := flag.String("imprint-url", envOr("RINCAST_IMPRINT_URL", ""), "link to the legal notice, shown on the account site")
	downloadURL := flag.String("download-url", envOr("RINCAST_DOWNLOAD_URL", ""), "where to get the apps, shown on the account site")
	contact := flag.String("contact", envOr("RINCAST_CONTACT", ""), "how to reach the operator, shown next to the password help")
	resetEmail := flag.String("reset-password", "", "set a new random password for this account's email, print it and exit (stop the server first)")
	// Kept so existing service files keep starting; the server no longer relays traffic.
	relayMB := flag.Int("relay-max-mb", 0, "deprecated, ignored (traffic is never relayed)")
	flag.Parse()

	if *resetEmail != "" {
		if err := resetPassword(*dataDir, *resetEmail, os.Stdout); err != nil {
			log.Fatalf("reset password: %v", err)
		}
		return
	}

	if err := os.MkdirAll(*dataDir, 0o700); err != nil {
		log.Fatalf("data dir: %v", err)
	}
	store, err := OpenStore(filepath.Join(*dataDir, "rincast.db"))
	if err != nil {
		log.Fatalf("open store: %v", err)
	}
	defer store.Close()
	key, err := loadOrCreateKey(filepath.Join(*dataDir, "signing.key"))
	if err != nil {
		log.Fatalf("signing key: %v", err)
	}

	hub := NewHub()
	if *relayMB != 0 {
		log.Printf("note: -relay-max-mb is ignored; the server only introduces peers and never relays traffic")
	}
	rv, err := NewRendezvous(*udpAddr, hub)
	if err != nil {
		log.Fatalf("rendezvous: %v", err)
	}
	go rv.Serve()

	rvPort := rv.LocalAddr().Port
	if p, err := strconv.Atoi(os.Getenv("RINCAST_RENDEZVOUS_PORT")); err == nil {
		rvPort = p // externally mapped port, e.g. behind a container NAT
	}
	srv := NewServer(Config{
		PublicURL:      *publicURL,
		RendezvousHost: *rvHost,
		RendezvousPort: rvPort,
		AllowSignups:   *allowSignups,
		TrustProxy:     *trustProxy,
		MaxUsers:       *maxUsers,
		PrivacyURL:     *privacyURL,
		ImprintURL:     *imprintURL,
		DownloadURL:    *downloadURL,
		Contact:        *contact,
	}, store, hub, key)

	httpServer := &http.Server{
		Addr:              *httpAddr,
		Handler:           srv.Routes(),
		ReadHeaderTimeout: 10 * time.Second,
		WriteTimeout:      70 * time.Second, // long-poll + margin
		IdleTimeout:       120 * time.Second,
	}

	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for now := range t.C {
			hub.Sweep(now)
			srv.SweepLimits()
			rv.limiter.Sweep()
		}
	}()
	go func() {
		t := time.NewTicker(6 * time.Hour)
		defer t.Stop()
		for now := range t.C {
			if n, err := store.PurgeExpiredTokens(now); err == nil && n > 0 {
				log.Printf("purged %d expired refresh tokens", n)
			}
		}
	}()

	go func() {
		log.Printf("RinCast server: http %s, udp %s, public %s", *httpAddr, rv.LocalAddr(), *publicURL)
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatalf("http: %v", err)
		}
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	<-sig
	log.Printf("shutting down")
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = httpServer.Shutdown(ctx)
	_ = rv.Close()
}
