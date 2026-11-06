package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"strings"
	"time"

	bolt "go.etcd.io/bbolt"
)

var (
	bucketUsers         = []byte("users")
	bucketUsersByEmail  = []byte("users_by_email")
	bucketRefreshTokens = []byte("refresh_tokens")
	bucketPCs           = []byte("pcs")
	bucketHeadsets      = []byte("headsets")

	errNotFound = errors.New("not found")
	errExists   = errors.New("already exists")
	errLimit    = errors.New("limit reached")
)

type User struct {
	ID           string    `json:"id"`
	Email        string    `json:"email"`
	Name         string    `json:"name"`
	PasswordHash string    `json:"passwordHash"`
	CreatedAt    time.Time `json:"createdAt"`
	// Access tokens issued before this Unix time are rejected. Set when the
	// password changes, so a stolen access token dies with the refresh tokens.
	TokensValidAfter int64 `json:"tokensValidAfter,omitempty"`
}

func (u *User) Public() map[string]any {
	return map[string]any{"id": u.ID, "email": u.Email, "name": u.Name, "createdAt": u.CreatedAt}
}

// RefreshToken is stored by the SHA-256 of the opaque token value.
type RefreshToken struct {
	UserID     string    `json:"userId"`
	SessionID  string    `json:"sessionId,omitempty"` // survives rotation; also in the access token
	DeviceKind string    `json:"deviceKind"`          // web, pc, headset
	DeviceID   string    `json:"deviceId"`
	DeviceName string    `json:"deviceName"`
	CreatedAt  time.Time `json:"createdAt"`
	ExpiresAt  time.Time `json:"expiresAt"`
	LastUsedAt time.Time `json:"lastUsedAt"`
}

type PC struct {
	ID         string    `json:"id"`
	UserID     string    `json:"userId"`
	Name       string    `json:"name"`
	GPU        string    `json:"gpu"`
	OS         string    `json:"os"`
	Version    string    `json:"version"`
	CreatedAt  time.Time `json:"createdAt"`
	LastSeenAt time.Time `json:"lastSeenAt"`
}

type Headset struct {
	ID         string    `json:"id"`
	UserID     string    `json:"userId"`
	Name       string    `json:"name"`
	Model      string    `json:"model"`
	CreatedAt  time.Time `json:"createdAt"`
	LastSeenAt time.Time `json:"lastSeenAt"`
}

type Store struct {
	db *bolt.DB
}

func OpenStore(path string) (*Store, error) {
	db, err := bolt.Open(path, 0o600, &bolt.Options{Timeout: 2 * time.Second})
	if err != nil {
		return nil, err
	}
	err = db.Update(func(tx *bolt.Tx) error {
		for _, b := range [][]byte{bucketUsers, bucketUsersByEmail, bucketRefreshTokens, bucketPCs, bucketHeadsets} {
			if _, err := tx.CreateBucketIfNotExists(b); err != nil {
				return err
			}
		}
		return scrubStoredAddrs(tx)
	})
	if err != nil {
		db.Close()
		return nil, err
	}
	return &Store{db: db}, nil
}

// scrubStoredAddrs rewrites PC records from older versions, which kept the PC's
// addresses on disk. They now live in the Hub, only while the PC is online.
func scrubStoredAddrs(tx *bolt.Tx) error {
	b := tx.Bucket(bucketPCs)
	var stale []PC
	err := b.ForEach(func(k, v []byte) error {
		var pc PC
		if bytes.Contains(v, []byte(`"localAddrs"`)) && json.Unmarshal(v, &pc) == nil {
			stale = append(stale, pc)
		}
		return nil
	})
	if err != nil {
		return err
	}
	for i := range stale {
		if err := put(tx, bucketPCs, stale[i].ID, &stale[i]); err != nil {
			return err
		}
	}
	return nil
}

func (s *Store) Close() error { return s.db.Close() }

func put(tx *bolt.Tx, bucket []byte, key string, v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return tx.Bucket(bucket).Put([]byte(key), data)
}

func get(tx *bolt.Tx, bucket []byte, key string, v any) error {
	data := tx.Bucket(bucket).Get([]byte(key))
	if data == nil {
		return errNotFound
	}
	return json.Unmarshal(data, v)
}

func normalizeEmail(email string) string { return strings.ToLower(strings.TrimSpace(email)) }

// CreateUser adds an account. maxUsers caps the total; 0 means no cap.
func (s *Store) CreateUser(u *User, maxUsers int) error {
	return s.db.Update(func(tx *bolt.Tx) error {
		email := normalizeEmail(u.Email)
		if tx.Bucket(bucketUsersByEmail).Get([]byte(email)) != nil {
			return errExists
		}
		if maxUsers > 0 && tx.Bucket(bucketUsers).Stats().KeyN >= maxUsers {
			return errLimit
		}
		u.Email = email
		if err := tx.Bucket(bucketUsersByEmail).Put([]byte(email), []byte(u.ID)); err != nil {
			return err
		}
		return put(tx, bucketUsers, u.ID, u)
	})
}

// UserCount returns the number of accounts.
func (s *Store) UserCount() (int, error) {
	n := 0
	err := s.db.View(func(tx *bolt.Tx) error {
		n = tx.Bucket(bucketUsers).Stats().KeyN
		return nil
	})
	return n, err
}

func (s *Store) UserByEmail(email string) (*User, error) {
	var u User
	err := s.db.View(func(tx *bolt.Tx) error {
		id := tx.Bucket(bucketUsersByEmail).Get([]byte(normalizeEmail(email)))
		if id == nil {
			return errNotFound
		}
		return get(tx, bucketUsers, string(id), &u)
	})
	if err != nil {
		return nil, err
	}
	return &u, nil
}

func (s *Store) UserByID(id string) (*User, error) {
	var u User
	if err := s.db.View(func(tx *bolt.Tx) error { return get(tx, bucketUsers, id, &u) }); err != nil {
		return nil, err
	}
	return &u, nil
}

func (s *Store) UpdateUser(u *User) error {
	return s.db.Update(func(tx *bolt.Tx) error { return put(tx, bucketUsers, u.ID, u) })
}

// ChangePassword saves u (with its new hash) and revokes the user's refresh
// tokens in one step, except those of keepSession (may be empty).
func (s *Store) ChangePassword(u *User, keepSession string) error {
	return s.db.Update(func(tx *bolt.Tx) error {
		if err := put(tx, bucketUsers, u.ID, u); err != nil {
			return err
		}
		return deleteMatching(tx, bucketRefreshTokens, func(v []byte) bool {
			var t RefreshToken
			return json.Unmarshal(v, &t) == nil && t.UserID == u.ID && (keepSession == "" || t.SessionID != keepSession)
		})
	})
}

// DeleteUser removes the account and everything stored for it. It returns the
// ids of the PCs that were linked, so the caller can tell them.
func (s *Store) DeleteUser(u *User) ([]string, error) {
	var pcIDs []string
	err := s.db.Update(func(tx *bolt.Tx) error {
		if err := tx.Bucket(bucketUsersByEmail).Delete([]byte(normalizeEmail(u.Email))); err != nil {
			return err
		}
		if err := tx.Bucket(bucketUsers).Delete([]byte(u.ID)); err != nil {
			return err
		}
		err := deleteMatching(tx, bucketRefreshTokens, func(v []byte) bool {
			var t RefreshToken
			return json.Unmarshal(v, &t) == nil && t.UserID == u.ID
		})
		if err != nil {
			return err
		}
		err = deleteMatching(tx, bucketPCs, func(v []byte) bool {
			var pc PC
			if json.Unmarshal(v, &pc) != nil || pc.UserID != u.ID {
				return false
			}
			pcIDs = append(pcIDs, pc.ID)
			return true
		})
		if err != nil {
			return err
		}
		return deleteMatching(tx, bucketHeadsets, func(v []byte) bool {
			var h Headset
			return json.Unmarshal(v, &h) == nil && h.UserID == u.ID
		})
	})
	return pcIDs, err
}

func deleteMatching(tx *bolt.Tx, bucket []byte, match func(v []byte) bool) error {
	b := tx.Bucket(bucket)
	var doomed [][]byte
	err := b.ForEach(func(k, v []byte) error {
		if match(v) {
			doomed = append(doomed, append([]byte(nil), k...))
		}
		return nil
	})
	if err != nil {
		return err
	}
	for _, k := range doomed {
		if err := b.Delete(k); err != nil {
			return err
		}
	}
	return nil
}

func (s *Store) PutRefreshToken(hash string, t *RefreshToken) error {
	return s.db.Update(func(tx *bolt.Tx) error { return put(tx, bucketRefreshTokens, hash, t) })
}

func (s *Store) RefreshTokenByHash(hash string) (*RefreshToken, error) {
	var t RefreshToken
	if err := s.db.View(func(tx *bolt.Tx) error { return get(tx, bucketRefreshTokens, hash, &t) }); err != nil {
		return nil, err
	}
	return &t, nil
}

func (s *Store) DeleteRefreshToken(hash string) error {
	return s.db.Update(func(tx *bolt.Tx) error { return tx.Bucket(bucketRefreshTokens).Delete([]byte(hash)) })
}

// DeleteRefreshTokensForDevice revokes every token issued to a device.
func (s *Store) DeleteRefreshTokensForDevice(userID, deviceID string) error {
	return s.db.Update(func(tx *bolt.Tx) error {
		b := tx.Bucket(bucketRefreshTokens)
		var doomed [][]byte
		err := b.ForEach(func(k, v []byte) error {
			var t RefreshToken
			if json.Unmarshal(v, &t) == nil && t.UserID == userID && t.DeviceID == deviceID {
				doomed = append(doomed, append([]byte(nil), k...))
			}
			return nil
		})
		if err != nil {
			return err
		}
		for _, k := range doomed {
			if err := b.Delete(k); err != nil {
				return err
			}
		}
		return nil
	})
}

// RefreshTokensForUser lists a user's sessions. The hashes stay in the store.
func (s *Store) RefreshTokensForUser(userID string) ([]*RefreshToken, error) {
	var out []*RefreshToken
	err := s.db.View(func(tx *bolt.Tx) error {
		return tx.Bucket(bucketRefreshTokens).ForEach(func(k, v []byte) error {
			var t RefreshToken
			if json.Unmarshal(v, &t) == nil && t.UserID == userID {
				out = append(out, &t)
			}
			return nil
		})
	})
	return out, err
}

func (s *Store) PurgeExpiredTokens(now time.Time) (int, error) {
	n := 0
	err := s.db.Update(func(tx *bolt.Tx) error {
		b := tx.Bucket(bucketRefreshTokens)
		var doomed [][]byte
		_ = b.ForEach(func(k, v []byte) error {
			var t RefreshToken
			if json.Unmarshal(v, &t) != nil || now.After(t.ExpiresAt) {
				doomed = append(doomed, append([]byte(nil), k...))
			}
			return nil
		})
		for _, k := range doomed {
			if err := b.Delete(k); err != nil {
				return err
			}
			n++
		}
		return nil
	})
	return n, err
}

func (s *Store) PutPC(pc *PC) error {
	return s.db.Update(func(tx *bolt.Tx) error {
		var existing PC
		if err := get(tx, bucketPCs, pc.ID, &existing); err == nil && existing.UserID != pc.UserID {
			// A PC id is bound to one account; re-linking to another account
			// requires unlinking first.
			return errExists
		}
		return put(tx, bucketPCs, pc.ID, pc)
	})
}

// RegisterPC stores a PC, refusing a new one once the user has limit PCs.
func (s *Store) RegisterPC(pc *PC, limit int) error {
	return s.db.Update(func(tx *bolt.Tx) error {
		var existing PC
		if err := get(tx, bucketPCs, pc.ID, &existing); err == nil {
			if existing.UserID != pc.UserID {
				return errExists
			}
			return put(tx, bucketPCs, pc.ID, pc)
		}
		n := 0
		err := tx.Bucket(bucketPCs).ForEach(func(k, v []byte) error {
			var other PC
			if json.Unmarshal(v, &other) == nil && other.UserID == pc.UserID {
				n++
			}
			return nil
		})
		if err != nil {
			return err
		}
		if n >= limit {
			return errLimit
		}
		return put(tx, bucketPCs, pc.ID, pc)
	})
}

func (s *Store) PCByID(id string) (*PC, error) {
	var pc PC
	if err := s.db.View(func(tx *bolt.Tx) error { return get(tx, bucketPCs, id, &pc) }); err != nil {
		return nil, err
	}
	return &pc, nil
}

func (s *Store) DeletePC(id string) error {
	return s.db.Update(func(tx *bolt.Tx) error { return tx.Bucket(bucketPCs).Delete([]byte(id)) })
}

func (s *Store) PCsForUser(userID string) ([]*PC, error) {
	var out []*PC
	err := s.db.View(func(tx *bolt.Tx) error {
		return tx.Bucket(bucketPCs).ForEach(func(k, v []byte) error {
			var pc PC
			if json.Unmarshal(v, &pc) == nil && pc.UserID == userID {
				out = append(out, &pc)
			}
			return nil
		})
	})
	return out, err
}

func (s *Store) PutHeadset(h *Headset) error {
	return s.db.Update(func(tx *bolt.Tx) error { return put(tx, bucketHeadsets, h.ID, h) })
}

// AddHeadset links a headset, refusing a new one once the user has limit headsets.
func (s *Store) AddHeadset(h *Headset, limit int) error {
	return s.db.Update(func(tx *bolt.Tx) error {
		n := 0
		err := tx.Bucket(bucketHeadsets).ForEach(func(k, v []byte) error {
			var other Headset
			if json.Unmarshal(v, &other) == nil && other.UserID == h.UserID && other.ID != h.ID {
				n++
			}
			return nil
		})
		if err != nil {
			return err
		}
		if n >= limit {
			return errLimit
		}
		return put(tx, bucketHeadsets, h.ID, h)
	})
}

func (s *Store) HeadsetsForUser(userID string) ([]*Headset, error) {
	var out []*Headset
	err := s.db.View(func(tx *bolt.Tx) error {
		return tx.Bucket(bucketHeadsets).ForEach(func(k, v []byte) error {
			var h Headset
			if json.Unmarshal(v, &h) == nil && h.UserID == userID {
				out = append(out, &h)
			}
			return nil
		})
	})
	return out, err
}

func (s *Store) HeadsetByID(id string) (*Headset, error) {
	var h Headset
	if err := s.db.View(func(tx *bolt.Tx) error { return get(tx, bucketHeadsets, id, &h) }); err != nil {
		return nil, err
	}
	return &h, nil
}

func (s *Store) DeleteHeadset(id string) error {
	return s.db.Update(func(tx *bolt.Tx) error { return tx.Bucket(bucketHeadsets).Delete([]byte(id)) })
}
