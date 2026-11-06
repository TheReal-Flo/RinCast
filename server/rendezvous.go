package main

import (
	"bytes"
	"encoding/json"
	"log"
	"net"
	"time"
)

// Rendezvous introduces the two sides of a connect session so they can reach
// each other directly (UDP hole punching). It never forwards stream traffic:
// datagrams that are not rendezvous messages are dropped.
//
// Datagrams start with "RCRV" followed by JSON:
//
//	{"op":"register","session":..,"role":0|1,"token":..,"candidates":["ip:port",..]}
//	  -> {"op":"registered","self":"<observed ip:port>","peer":"<first peer addr>","peers":[..]}
//	{"op":"ping","pad":"<filler>"} -> {"op":"pong","self":"<observed ip:port>"}
//
// UDP sources can be spoofed, so nobody can use this server to bounce more bytes
// at a victim than they send: "pong" and "error" are only sent when they are no
// larger than the request. A ping therefore needs padding (100 bytes in total
// are enough for any address), and each source gets a limited number of datagrams.
//
// "peers" lists the endpoints the server observed the other side sending from
// (IPv4 and, when the client reached the server over IPv6, IPv6) followed by
// the candidates the other side reported itself (LAN addresses, global IPv6
// addresses, router port mappings). Whenever one side registers, the other side
// receives an updated "registered" message too, so both start punching at once.
type Rendezvous struct {
	conn    *net.UDPConn
	hub     *Hub
	limiter *RateLimiter
}

var rvMagic = []byte("RCRV")

// Clients register every 300 ms until answered, then every few seconds, per
// address family. This leaves room for several sessions behind one address.
const rvPerSourcePerMinute = 120

type rvMessage struct {
	Op         string   `json:"op"`
	Session    string   `json:"session,omitempty"`
	Role       int      `json:"role"`
	Token      string   `json:"token,omitempty"`
	Candidates []string `json:"candidates,omitempty"`
	Self       string   `json:"self,omitempty"`
	Peer       string   `json:"peer,omitempty"`
	Peers      []string `json:"peers,omitempty"`
	Error      string   `json:"error,omitempty"`
}

func NewRendezvous(addr string, hub *Hub) (*Rendezvous, error) {
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		return nil, err
	}
	return &Rendezvous{conn: conn, hub: hub, limiter: NewRateLimiter(rvPerSourcePerMinute, time.Minute)}, nil
}

func (rv *Rendezvous) LocalAddr() *net.UDPAddr { return rv.conn.LocalAddr().(*net.UDPAddr) }

func (rv *Rendezvous) Close() error { return rv.conn.Close() }

func (rv *Rendezvous) send(to *net.UDPAddr, m rvMessage) {
	body, _ := json.Marshal(m)
	_, _ = rv.conn.WriteToUDP(append(append([]byte{}, rvMagic...), body...), to)
}

// reply answers a datagram of requestSize bytes whose source may be forged.
// Anything larger than the request is dropped.
func (rv *Rendezvous) reply(to *net.UDPAddr, m rvMessage, requestSize int) {
	body, _ := json.Marshal(m)
	if len(rvMagic)+len(body) <= requestSize {
		_, _ = rv.conn.WriteToUDP(append(append([]byte{}, rvMagic...), body...), to)
	}
}

func (rv *Rendezvous) Serve() {
	buf := make([]byte, 4096)
	for {
		n, from, err := rv.conn.ReadFromUDP(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			log.Printf("rendezvous: stopped: %v", err)
			return
		}
		if pkt := buf[:n]; bytes.HasPrefix(pkt, rvMagic) && rv.limiter.Allow(limitKey(from.IP.String())) {
			rv.handleControl(pkt[len(rvMagic):], from)
		}
	}
}

func (rv *Rendezvous) handleControl(body []byte, from *net.UDPAddr) {
	var m rvMessage
	if err := json.Unmarshal(body, &m); err != nil {
		return
	}
	size := len(rvMagic) + len(body)
	switch m.Op {
	case "ping":
		rv.reply(from, rvMessage{Op: "pong", Self: canonical(from)}, size)
	case "register":
		if m.Role != rolePC && m.Role != roleHeadset {
			return
		}
		self, peers, ok := rv.hub.RegisterEndpoint(m.Session, m.Role, m.Token, normalize(from), m.Candidates)
		if !ok {
			rv.reply(from, rvMessage{Op: "error", Session: m.Session, Error: "invalid_session"}, size)
			return
		}
		reply := rvMessage{Op: "registered", Session: m.Session, Role: m.Role, Self: self, Peers: peers}
		if len(peers) > 0 {
			reply.Peer = peers[0]
		}
		rv.send(from, reply)
		// Tell the other side about us right away so both punch at the same time.
		mine := rv.hub.PeerAddrs(m.Session, m.Role)
		for _, ep := range rv.hub.ObservedEndpoints(m.Session, 1-m.Role) {
			if to, err := net.ResolveUDPAddr("udp", ep); err == nil {
				rv.send(to, rvMessage{Op: "registered", Session: m.Session, Role: 1 - m.Role, Self: ep, Peer: self, Peers: mine})
			}
		}
	}
}

// normalize turns IPv4-mapped IPv6 sources (seen on dual-stack sockets) back into IPv4.
func normalize(a *net.UDPAddr) *net.UDPAddr {
	if v4 := a.IP.To4(); v4 != nil {
		return &net.UDPAddr{IP: v4, Port: a.Port}
	}
	return a
}

func canonical(a *net.UDPAddr) string { return normalize(a).String() }
