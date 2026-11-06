package main

import (
	"net"
	"sync"
	"time"
)

// RateLimiter is a per-key token bucket. Good enough to blunt credential
// stuffing against the auth endpoints of a self-hosted server.
type RateLimiter struct {
	mu      sync.Mutex
	burst   float64
	refill  float64 // tokens per second
	buckets map[string]*bucket
}

type bucket struct {
	tokens float64
	last   time.Time
}

func NewRateLimiter(perWindow int, window time.Duration) *RateLimiter {
	return &RateLimiter{
		burst:   float64(perWindow),
		refill:  float64(perWindow) / window.Seconds(),
		buckets: map[string]*bucket{},
	}
}

func (l *RateLimiter) Allow(key string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := time.Now()
	b := l.buckets[key]
	if b == nil {
		if len(l.buckets) >= maxTrackedKeys {
			l.evictLocked(now)
		}
		b = &bucket{tokens: l.burst, last: now}
		l.buckets[key] = b
	}
	b.tokens += now.Sub(b.last).Seconds() * l.refill
	if b.tokens > l.burst {
		b.tokens = l.burst
	}
	b.last = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

func (l *RateLimiter) Sweep() {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.sweepLocked(time.Now())
}

func (l *RateLimiter) sweepLocked(now time.Time) {
	for k, b := range l.buckets {
		if now.Sub(b.last) > 10*time.Minute {
			delete(l.buckets, k)
		}
	}
}

// evictLocked keeps memory bounded when keys can be spoofed (UDP sources).
func (l *RateLimiter) evictLocked(now time.Time) {
	l.sweepLocked(now)
	if len(l.buckets) >= maxTrackedKeys {
		clear(l.buckets)
	}
}

// maxTrackedKeys bounds each limiter's memory to a few tens of megabytes.
const maxTrackedKeys = 100_000

// limitKey is the rate limit key for an address. One IPv6 customer usually holds
// a whole /64, so that is what counts as one source.
func limitKey(addr string) string {
	ip := net.ParseIP(addr)
	if ip == nil {
		return addr
	}
	if v4 := ip.To4(); v4 != nil {
		return v4.String()
	}
	return ip.Mask(net.CIDRMask(64, 128)).String()
}

// LoginBackoff slows password guessing against one account from any number of
// addresses. It counts failures per email whether or not the account exists, so
// being throttled says nothing about which emails are registered.
type LoginBackoff struct {
	mu    sync.Mutex
	fails map[string]*loginFailures
}

type loginFailures struct {
	count int
	until time.Time
	last  time.Time
}

const (
	loginFreeAttempts = 5
	loginBaseDelay    = 30 * time.Second
	loginMaxDelay     = 15 * time.Minute
)

func NewLoginBackoff() *LoginBackoff {
	return &LoginBackoff{fails: map[string]*loginFailures{}}
}

// Keys are hashed so the map holds no addresses and has a fixed entry size.
func backoffKey(email string) string { return sha256Hex(normalizeEmail(email)) }

// Wait returns how long sign-in for this email stays blocked, or 0.
func (b *LoginBackoff) Wait(email string) time.Duration {
	b.mu.Lock()
	defer b.mu.Unlock()
	if f := b.fails[backoffKey(email)]; f != nil {
		if d := time.Until(f.until); d > 0 {
			return d
		}
	}
	return 0
}

func (b *LoginBackoff) Fail(email string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	key := backoffKey(email)
	f := b.fails[key]
	if f == nil {
		if len(b.fails) >= maxTrackedKeys {
			b.sweepLocked()
			if len(b.fails) >= maxTrackedKeys {
				clear(b.fails)
			}
		}
		f = &loginFailures{}
		b.fails[key] = f
	}
	now := time.Now()
	f.count++
	f.last = now
	if f.count >= loginFreeAttempts {
		delay := loginMaxDelay
		if shift := f.count - loginFreeAttempts; shift < 5 {
			delay = loginBaseDelay << shift // 30 s doubling to 8 min, then the maximum
		}
		f.until = now.Add(delay)
	}
}

func (b *LoginBackoff) Reset(email string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	delete(b.fails, backoffKey(email))
}

func (b *LoginBackoff) Sweep() {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.sweepLocked()
}

func (b *LoginBackoff) sweepLocked() {
	for k, f := range b.fails {
		if time.Since(f.last) > time.Hour {
			delete(b.fails, k)
		}
	}
}
